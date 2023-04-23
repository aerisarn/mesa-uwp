/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_upper_case_globals)]
#![allow(unstable_name_collisions)]

use crate::nak_ir::*;
use crate::nir::*;
use crate::util::DivCeil;

use nak_bindings::*;

use std::cmp::min;
use std::collections::HashMap;

struct ShaderFromNir<'a> {
    nir: &'a nir_shader,
    func: Option<Function>,
    blocks: Vec<BasicBlock>,
    instrs: Vec<Instr>,
    fs_out_regs: Vec<Src>,
    end_block_id: u32,
    ssa_map: HashMap<u32, Vec<SSAValue>>,
    num_phis: u32,
    phi_map: HashMap<(u32, u8), u32>,
}

impl<'a> ShaderFromNir<'a> {
    fn new(nir: &'a nir_shader) -> Self {
        let mut fs_out_regs = Vec::new();
        if nir.info.stage() == MESA_SHADER_FRAGMENT {
            fs_out_regs
                .resize(nir.num_outputs.try_into().unwrap(), Src::new_zero());
        }

        Self {
            nir: nir,
            func: None,
            blocks: Vec::new(),
            instrs: Vec::new(),
            fs_out_regs: fs_out_regs,
            end_block_id: 0,
            ssa_map: HashMap::new(),
            num_phis: 0,
            phi_map: HashMap::new(),
        }
    }

    fn alloc_ssa(&mut self, file: RegFile) -> SSAValue {
        self.func.as_mut().unwrap().ssa_alloc.alloc(file)
    }

    fn get_ssa(&mut self, def: &nir_def) -> &[SSAValue] {
        self.ssa_map.entry(def.index).or_insert_with(|| {
            let (file, comps) = if def.bit_size == 1 {
                (RegFile::Pred, def.num_components)
            } else {
                assert!(def.bit_size == 32 || def.bit_size == 64);
                let comps = (def.bit_size / 32) * def.num_components;
                (RegFile::GPR, comps)
            };
            let mut vec = Vec::new();
            for i in 0..comps {
                vec.push(self.func.as_mut().unwrap().ssa_alloc.alloc(file))
            }
            vec
        })
    }

    fn get_ssa_comp(&mut self, def: &nir_def, c: u8) -> SSARef {
        let vec = self.get_ssa(def);
        match def.bit_size {
            1 | 32 => vec[usize::from(c)].into(),
            64 => [vec[usize::from(c) * 2], vec[usize::from(c) * 2 + 1]].into(),
            _ => panic!("Unsupported bit size"),
        }
    }

    fn get_src(&mut self, src: &nir_src) -> Src {
        SSARef::try_from(self.get_ssa(&src.as_def()))
            .unwrap()
            .into()
    }

    fn get_io_addr_offset(
        &mut self,
        addr: &nir_src,
        imm_bits: u8,
    ) -> (Src, i32) {
        let addr = addr.as_def();
        let addr_offset = unsafe {
            nak_get_io_addr_offset(addr as *const _ as *mut _, imm_bits)
        };

        if let Some(base_def) = std::ptr::NonNull::new(addr_offset.base.def) {
            let base_def = unsafe { base_def.as_ref() };
            let base_comp = u8::try_from(addr_offset.base.comp).unwrap();
            let base = self.get_ssa_comp(base_def, base_comp);
            (base.into(), addr_offset.offset)
        } else {
            (SrcRef::Zero.into(), addr_offset.offset)
        }
    }

    fn get_dst(&mut self, dst: &nir_def) -> Dst {
        SSARef::try_from(self.get_ssa(dst)).unwrap().into()
    }

    fn get_phi_id(&mut self, phi: &nir_phi_instr, comp: u8) -> u32 {
        let ssa = phi.def.as_def();
        *self.phi_map.entry((ssa.index, comp)).or_insert_with(|| {
            let id = self.num_phis;
            self.num_phis += 1;
            id
        })
    }

    fn parse_alu(&mut self, alu: &nir_alu_instr) {
        let mut srcs = Vec::new();
        for (i, alu_src) in alu.srcs_as_slice().iter().enumerate() {
            let bit_size = alu_src.src.bit_size();
            let comps = alu.src_components(i.try_into().unwrap());

            let alu_src_ssa = self.get_ssa(&alu_src.src.as_def());
            let mut src_comps = Vec::new();
            for c in 0..comps {
                let s = usize::from(alu_src.swizzle[usize::from(c)]);
                if bit_size == 1 || bit_size == 32 {
                    src_comps.push(alu_src_ssa[s]);
                } else if bit_size == 64 {
                    src_comps.push(alu_src_ssa[s * 2]);
                    src_comps.push(alu_src_ssa[s * 2 + 1]);
                } else {
                    panic!("Unhandled bit size");
                }
            }
            srcs.push(Src::from(SSARef::try_from(src_comps).unwrap()));
        }

        /* Handle vectors as a special case since they're the only ALU ops that
         * can produce more than a 16B of data.
         */
        match alu.op {
            nir_op_mov | nir_op_vec2 | nir_op_vec3 | nir_op_vec4
            | nir_op_vec5 | nir_op_vec8 | nir_op_vec16 => {
                let mut pcopy = OpParCopy::new();
                for src in srcs {
                    for v in src.as_ssa().unwrap().iter() {
                        pcopy.srcs.push((*v).into());
                    }
                }
                for v in self.get_ssa(&alu.def.as_def()) {
                    pcopy.dsts.push((*v).into());
                }
                assert!(pcopy.srcs.len() == pcopy.dsts.len());
                self.instrs.push(Instr::new(Op::ParCopy(pcopy)));
                return;
            }
            _ => (),
        }

        let dst = self.get_dst(&alu.def);

        match alu.op {
            nir_op_b2f32 => {
                self.instrs.push(Instr::new(Op::Sel(OpSel {
                    dst: dst,
                    cond: srcs[0].not(),
                    srcs: [Src::new_zero(), Src::new_imm_u32(0x3f800000)],
                })));
            }
            nir_op_b2i32 => {
                self.instrs.push(Instr::new(Op::Sel(OpSel {
                    dst: dst,
                    cond: srcs[0].not(),
                    srcs: [Src::new_zero(), Src::new_imm_u32(1)],
                })));
            }
            nir_op_bcsel => {
                self.instrs
                    .push(Instr::new_sel(dst, srcs[0], srcs[1], srcs[2]));
            }
            nir_op_f2i32 | nir_op_f2u32 => {
                let src_bits = usize::from(alu.get_src(0).bit_size());
                let dst_bits = usize::from(alu.def.bit_size());
                let dst_is_signed = alu.info().output_type & 2 != 0;
                self.instrs.push(Instr::new(Op::F2I(OpF2I {
                    dst: dst,
                    src: srcs[0],
                    src_type: FloatType::from_bytes(src_bits / 8),
                    dst_type: IntType::from_bytes(dst_bits / 8, dst_is_signed),
                    rnd_mode: FRndMode::Zero,
                })));
            }
            nir_op_fabs => {
                self.instrs.push(Instr::new(Op::FMov(OpFMov {
                    dst: dst,
                    src: srcs[0].abs(),
                    saturate: false,
                })));
            }
            nir_op_fadd => {
                self.instrs.push(Instr::new_fadd(dst, srcs[0], srcs[1]));
            }
            nir_op_fcos => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(Instr::new_fmul(
                    tmp.into(),
                    srcs[0],
                    Src::new_imm_u32(frac_1_2pi.to_bits()),
                ));
                self.instrs
                    .push(Instr::new_mufu(dst, MuFuOp::Cos, tmp.into()));
            }
            nir_op_feq => {
                self.instrs.push(Instr::new_fsetp(
                    dst,
                    FloatCmpOp::OrdEq,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_fexp2 => {
                self.instrs
                    .push(Instr::new_mufu(dst, MuFuOp::Exp2, srcs[0]));
            }
            nir_op_ffma => {
                self.instrs.push(Instr::new(Op::FFma(OpFFma {
                    dst: dst,
                    srcs: [srcs[0], srcs[1], srcs[2]],
                    saturate: false,
                    rnd_mode: FRndMode::NearestEven,
                })));
            }
            nir_op_fge => {
                self.instrs.push(Instr::new_fsetp(
                    dst,
                    FloatCmpOp::OrdGe,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_flog2 => {
                self.instrs
                    .push(Instr::new_mufu(dst, MuFuOp::Log2, srcs[0]));
            }
            nir_op_flt => {
                self.instrs.push(Instr::new_fsetp(
                    dst,
                    FloatCmpOp::OrdLt,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_fmax => {
                self.instrs.push(Instr::new(Op::FMnMx(OpFMnMx {
                    dst: dst,
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::False.into(),
                })));
            }
            nir_op_fmin => {
                self.instrs.push(Instr::new(Op::FMnMx(OpFMnMx {
                    dst: dst,
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::True.into(),
                })));
            }
            nir_op_fmul => {
                self.instrs.push(Instr::new_fmul(dst, srcs[0], srcs[1]));
            }
            nir_op_fneu => {
                self.instrs.push(Instr::new_fsetp(
                    dst,
                    FloatCmpOp::UnordNe,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_fneg => {
                self.instrs.push(Instr::new(Op::FMov(OpFMov {
                    dst: dst,
                    src: srcs[0].neg(),
                    saturate: false,
                })));
            }
            nir_op_fquantize2f16 => {
                let tmp = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(
                    OpF2F {
                        dst: tmp.into(),
                        src: srcs[0],
                        src_type: FloatType::F32,
                        dst_type: FloatType::F16,
                        rnd_mode: FRndMode::NearestEven,
                        ftz: true,
                    }
                    .into(),
                );
                self.instrs.push(
                    OpF2F {
                        dst: dst,
                        src: tmp.into(),
                        src_type: FloatType::F16,
                        dst_type: FloatType::F32,
                        rnd_mode: FRndMode::NearestEven,
                        ftz: true,
                    }
                    .into(),
                );
            }
            nir_op_frcp => {
                self.instrs.push(Instr::new_mufu(dst, MuFuOp::Rcp, srcs[0]));
            }
            nir_op_frsq => {
                self.instrs.push(Instr::new_mufu(dst, MuFuOp::Rsq, srcs[0]));
            }
            nir_op_fsat => {
                self.instrs.push(Instr::new(Op::FMov(OpFMov {
                    dst: dst,
                    src: srcs[0],
                    saturate: true,
                })));
            }
            nir_op_fsign => {
                let lz = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(Instr::new_fset(
                    lz.into(),
                    FloatCmpOp::OrdLt,
                    srcs[0],
                    Src::new_zero(),
                ));

                let gz = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(Instr::new_fset(
                    gz.into(),
                    FloatCmpOp::OrdGt,
                    srcs[0],
                    Src::new_zero(),
                ));

                self.instrs.push(Instr::new_fadd(
                    dst,
                    gz.into(),
                    Src::from(lz).neg(),
                ));
            }
            nir_op_fsin => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(Instr::new_fmul(
                    tmp.into(),
                    srcs[0],
                    Src::new_imm_u32(frac_1_2pi.to_bits()),
                ));
                self.instrs
                    .push(Instr::new_mufu(dst, MuFuOp::Sin, tmp.into()));
            }
            nir_op_fsqrt => {
                self.instrs
                    .push(Instr::new_mufu(dst, MuFuOp::Sqrt, srcs[0]));
            }
            nir_op_i2f32 => {
                self.instrs.push(Instr::new_i2f(dst, srcs[0]));
            }
            nir_op_iabs => {
                self.instrs.push(Instr::new(Op::IMov(OpIMov {
                    dst: dst,
                    src: srcs[0].abs(),
                })));
            }
            nir_op_iadd => {
                if alu.def.bit_size == 64 {
                    let x = srcs[0].as_ssa().unwrap();
                    let y = srcs[1].as_ssa().unwrap();
                    let sum = dst.as_ssa().unwrap();
                    let carry = self.alloc_ssa(RegFile::Pred);

                    self.instrs.push(Instr::new(Op::IAdd3(OpIAdd3 {
                        dst: sum[0].into(),
                        overflow: carry.into(),
                        srcs: [x[0].into(), y[0].into(), Src::new_zero()],
                        carry: SrcRef::False.into(),
                    })));
                    self.instrs.push(Instr::new(Op::IAdd3(OpIAdd3 {
                        dst: sum[1].into(),
                        overflow: Dst::None,
                        srcs: [x[1].into(), y[1].into(), Src::new_zero()],
                        carry: carry.into(),
                    })));
                } else {
                    self.instrs.push(Instr::new_iadd(dst, srcs[0], srcs[1]));
                }
            }
            nir_op_iand => {
                self.instrs.push(Instr::new_lop2(
                    dst,
                    LogicOp::new_lut(&|x, y, _| x & y),
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_ieq => {
                if alu.get_src(0).bit_size() == 1 {
                    self.instrs.push(Instr::new_plop3(
                        dst,
                        LogicOp::new_lut(&|x, y, _| !(x ^ y)),
                        srcs[0],
                        srcs[1],
                        Src::new_imm_bool(true),
                    ));
                } else {
                    self.instrs.push(Instr::new_isetp(
                        dst,
                        IntCmpType::I32,
                        IntCmpOp::Eq,
                        srcs[0],
                        srcs[1],
                    ));
                }
            }
            nir_op_ige => {
                self.instrs.push(Instr::new_isetp(
                    dst,
                    IntCmpType::I32,
                    IntCmpOp::Ge,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_ilt => {
                self.instrs.push(Instr::new_isetp(
                    dst,
                    IntCmpType::I32,
                    IntCmpOp::Lt,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_ine => {
                if alu.get_src(0).bit_size() == 1 {
                    self.instrs.push(Instr::new_plop3(
                        dst,
                        LogicOp::new_lut(&|x, y, _| !(x ^ y)),
                        srcs[0],
                        srcs[1],
                        Src::new_imm_bool(true),
                    ));
                } else {
                    self.instrs.push(Instr::new_isetp(
                        dst,
                        IntCmpType::I32,
                        IntCmpOp::Ne,
                        srcs[0],
                        srcs[1],
                    ));
                }
            }
            nir_op_imax | nir_op_imin | nir_op_umax | nir_op_umin => {
                let (tp, min) = match alu.op {
                    nir_op_imax => (IntCmpType::I32, SrcRef::False),
                    nir_op_imin => (IntCmpType::I32, SrcRef::True),
                    nir_op_umax => (IntCmpType::U32, SrcRef::False),
                    nir_op_umin => (IntCmpType::U32, SrcRef::True),
                    _ => panic!("Not an integer min/max"),
                };
                self.instrs.push(Instr::new(Op::IMnMx(OpIMnMx {
                    dst: dst,
                    cmp_type: tp,
                    srcs: [srcs[0], srcs[1]],
                    min: min.into(),
                })));
            }
            nir_op_imul => {
                self.instrs.push(Instr::new(Op::IMad(OpIMad {
                    dst: dst,
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: false,
                })));
            }
            nir_op_imul_2x32_64 | nir_op_umul_2x32_64 => {
                self.instrs.push(Instr::new(Op::IMad64(OpIMad64 {
                    dst: dst,
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: alu.op == nir_op_imul_2x32_64,
                })));
            }
            nir_op_imul_high | nir_op_umul_high => {
                let dst_hi = dst.as_ssa().unwrap()[0];
                let dst_lo = self.alloc_ssa(RegFile::GPR);
                self.instrs.push(Instr::new(Op::IMad64(OpIMad64 {
                    dst: [dst_lo, dst_hi].into(),
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: alu.op == nir_op_imul_high,
                })));
            }
            nir_op_ineg => {
                self.instrs.push(Instr::new(Op::IMov(OpIMov {
                    dst: dst,
                    src: srcs[0].neg(),
                })));
            }
            nir_op_inot => {
                self.instrs.push(Instr::new_lop2(
                    dst,
                    LogicOp::new_lut(&|x, _, _| !x),
                    srcs[0],
                    Src::new_imm_bool(true),
                ));
            }
            nir_op_ior => {
                self.instrs.push(Instr::new_lop2(
                    dst,
                    LogicOp::new_lut(&|x, y, _| x | y),
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_ishl => {
                self.instrs.push(Instr::new(Op::Shf(OpShf {
                    dst: dst,
                    low: srcs[0],
                    high: Src::new_zero(),
                    shift: srcs[1],
                    right: false,
                    wrap: true,
                    data_type: IntType::U32,
                    dst_high: false,
                })));
            }
            nir_op_ishr => {
                self.instrs.push(Instr::new(Op::Shf(OpShf {
                    dst: dst,
                    low: Src::new_zero(),
                    high: srcs[0],
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::I32,
                    dst_high: true,
                })));
            }
            nir_op_ixor => {
                self.instrs.push(Instr::new_lop2(
                    dst,
                    LogicOp::new_lut(&|x, y, _| x ^ y),
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_mov => {
                self.instrs.push(Instr::new_mov(dst, srcs[0]));
            }
            nir_op_pack_64_2x32_split => {
                let dst_ssa = dst.as_ssa().unwrap();
                let mut pcopy = OpParCopy::new();
                pcopy.push(srcs[0], dst_ssa[0].into());
                pcopy.push(srcs[1], dst_ssa[1].into());
                self.instrs.push(Instr::new(Op::ParCopy(pcopy)));
            }
            nir_op_u2f32 => {
                self.instrs.push(Instr::new_u2f(dst, srcs[0]));
            }
            nir_op_uge => {
                self.instrs.push(Instr::new_isetp(
                    dst,
                    IntCmpType::U32,
                    IntCmpOp::Ge,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_ult => {
                self.instrs.push(Instr::new_isetp(
                    dst,
                    IntCmpType::U32,
                    IntCmpOp::Lt,
                    srcs[0],
                    srcs[1],
                ));
            }
            nir_op_unpack_64_2x32_split_x => {
                let src0_x = srcs[0].as_ssa().unwrap()[0];
                self.instrs.push(Instr::new_mov(dst, src0_x.into()));
            }
            nir_op_unpack_64_2x32_split_y => {
                let src0_y = srcs[0].as_ssa().unwrap()[1];
                self.instrs.push(Instr::new_mov(dst, src0_y.into()));
            }
            nir_op_ushr => {
                self.instrs.push(Instr::new(Op::Shf(OpShf {
                    dst: dst,
                    low: srcs[0],
                    high: Src::new_zero(),
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::U32,
                    dst_high: false,
                })));
            }
            _ => panic!("Unsupported ALU instruction: {}", alu.info().name()),
        }
    }

    fn parse_jump(&mut self, jump: &nir_jump_instr) {
        /* Nothing to do */
    }

    fn parse_tex(&mut self, tex: &nir_tex_instr) {
        let dim = match tex.sampler_dim {
            GLSL_SAMPLER_DIM_1D => {
                if tex.is_array {
                    TexDim::Array1D
                } else {
                    TexDim::_1D
                }
            }
            GLSL_SAMPLER_DIM_2D => {
                if tex.is_array {
                    TexDim::Array2D
                } else {
                    TexDim::_2D
                }
            }
            GLSL_SAMPLER_DIM_3D => {
                assert!(!tex.is_array);
                TexDim::_3D
            }
            GLSL_SAMPLER_DIM_CUBE => {
                if tex.is_array {
                    TexDim::ArrayCube
                } else {
                    TexDim::Cube
                }
            }
            GLSL_SAMPLER_DIM_BUF => TexDim::_1D,
            GLSL_SAMPLER_DIM_MS => {
                if tex.is_array {
                    TexDim::Array2D
                } else {
                    TexDim::_2D
                }
            }
            _ => panic!("Unsupported texture dimension: {}", tex.sampler_dim),
        };

        let srcs = tex.srcs_as_slice();
        assert!(srcs[0].src_type == nir_tex_src_backend1);
        if srcs.len() > 1 {
            assert!(srcs.len() == 2);
            assert!(srcs[1].src_type == nir_tex_src_backend2);
        }

        let flags: nak_nir_tex_flags =
            unsafe { std::mem::transmute_copy(&tex.backend_flags) };

        let mask = tex.def.components_read();
        let mask = u8::try_from(mask).unwrap();

        let tex_dst = *self.get_dst(&tex.def).as_ssa().unwrap();
        let mut dst_comps = Vec::new();
        for (i, comp) in tex_dst.iter().enumerate() {
            if mask & (1 << i) == 0 {
                self.instrs
                    .push(Instr::new_mov((*comp).into(), Src::new_zero()));
            } else {
                dst_comps.push(*comp);
            }
        }

        let mut dsts = [Dst::None; 2];
        dsts[0] = SSARef::try_from(&dst_comps[..min(dst_comps.len(), 2)])
            .unwrap()
            .into();
        if dst_comps.len() > 2 {
            dsts[1] = SSARef::try_from(&dst_comps[2..]).unwrap().into();
        }

        if tex.op == nir_texop_hdr_dim_nv {
            let src = self.get_src(&srcs[0].src);
            self.instrs.push(Instr::new(Op::Txq(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::Dimension,
                mask: mask,
            })));
        } else if tex.op == nir_texop_tex_type_nv {
            let src = self.get_src(&srcs[0].src);
            self.instrs.push(Instr::new(Op::Txq(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::TextureType,
                mask: mask,
            })));
        } else {
            let lod_mode = match flags.lod_mode() {
                NAK_NIR_LOD_MODE_AUTO => TexLodMode::Auto,
                NAK_NIR_LOD_MODE_ZERO => TexLodMode::Zero,
                NAK_NIR_LOD_MODE_BIAS => TexLodMode::Bias,
                NAK_NIR_LOD_MODE_LOD => TexLodMode::Lod,
                NAK_NIR_LOD_MODE_CLAMP => TexLodMode::Clamp,
                NAK_NIR_LOD_MODE_BIAS_CLAMP => TexLodMode::BiasClamp,
                _ => panic!("Invalid LOD mode"),
            };

            let offset_mode = match flags.offset_mode() {
                NAK_NIR_OFFSET_MODE_NONE => Tld4OffsetMode::None,
                NAK_NIR_OFFSET_MODE_AOFFI => Tld4OffsetMode::AddOffI,
                NAK_NIR_OFFSET_MODE_PER_PX => Tld4OffsetMode::PerPx,
                _ => panic!("Invalid offset mode"),
            };

            let srcs = [self.get_src(&srcs[0].src), self.get_src(&srcs[1].src)];

            if tex.op == nir_texop_txd {
                assert!(lod_mode == TexLodMode::Auto);
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                assert!(!flags.has_z_cmpr());
                self.instrs.push(Instr::new(Op::Txd(OpTxd {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                })));
            } else if tex.op == nir_texop_lod {
                assert!(offset_mode == Tld4OffsetMode::None);
                self.instrs.push(Instr::new(Op::Tmml(OpTmml {
                    dsts: dsts,
                    srcs: srcs,
                    dim: dim,
                    mask: mask,
                })));
            } else if tex.op == nir_texop_txf {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                self.instrs.push(Instr::new(Op::Tld(OpTld {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    is_ms: false,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                })));
            } else if tex.op == nir_texop_tg4 {
                self.instrs.push(Instr::new(Op::Tld4(OpTld4 {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    comp: tex.component().try_into().unwrap(),
                    offset_mode: offset_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    mask: mask,
                })));
            } else {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                self.instrs.push(Instr::new(Op::Tex(OpTex {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                })));
            }
        }
    }

    fn get_image_dim(&mut self, intrin: &nir_intrinsic_instr) -> ImageDim {
        let is_array = intrin.image_array();
        let image_dim = intrin.image_dim();
        match intrin.image_dim() {
            GLSL_SAMPLER_DIM_1D => {
                if is_array {
                    ImageDim::_1DArray
                } else {
                    ImageDim::_1D
                }
            }
            GLSL_SAMPLER_DIM_2D => {
                if is_array {
                    ImageDim::_2DArray
                } else {
                    ImageDim::_2D
                }
            }
            GLSL_SAMPLER_DIM_3D => {
                assert!(!is_array);
                ImageDim::_3D
            }
            GLSL_SAMPLER_DIM_CUBE => ImageDim::_2DArray,
            GLSL_SAMPLER_DIM_BUF => {
                assert!(!is_array);
                ImageDim::_1DBuffer
            }
            _ => panic!("Unsupported image dimension: {}", image_dim),
        }
    }

    fn get_image_coord(
        &mut self,
        intrin: &nir_intrinsic_instr,
        dim: ImageDim,
    ) -> Src {
        let vec = self.get_ssa(intrin.get_src(1).as_def());
        /* let sample = self.get_src(&srcs[2]); */
        let comps = usize::from(dim.coord_comps());
        SSARef::try_from(&vec[0..comps]).unwrap().into()
    }

    fn parse_intrinsic(&mut self, intrin: &nir_intrinsic_instr) {
        let srcs = intrin.srcs_as_slice();
        match intrin.intrinsic {
            nir_intrinsic_bindless_image_load => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new(OpSuLd {
                    dst: dst,
                    resident: Dst::None,
                    image_dim: dim,
                    mem_order: MemOrder::Weak,
                    mem_scope: MemScope::CTA,
                    mask: 0xf,
                    handle: handle,
                    coord: coord,
                }));
            }
            nir_intrinsic_bindless_image_store => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let data = self.get_src(&srcs[3]);
                self.instrs.push(Instr::new(OpSuSt {
                    image_dim: dim,
                    mem_order: MemOrder::Weak,
                    mem_scope: MemScope::CTA,
                    mask: 0xf,
                    handle: handle,
                    coord: coord,
                    data: data,
                }));
            }
            nir_intrinsic_load_barycentric_centroid => (),
            nir_intrinsic_load_barycentric_pixel => (),
            nir_intrinsic_load_barycentric_sample => (),
            nir_intrinsic_load_global => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A64,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Global,
                    order: MemOrder::Strong,
                    scope: MemScope::System,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 32);
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new_ld(dst, access, addr, offset));
            }
            nir_intrinsic_load_input => {
                let addr = u16::try_from(intrin.base()).unwrap();
                let vtx = Src::new_zero();
                let offset = self.get_src(&srcs[0]);
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new_ald(dst, addr, vtx, offset));
            }
            nir_intrinsic_load_interpolated_input => {
                let bary =
                    srcs[0].as_def().parent_instr().as_intrinsic().unwrap();
                let addr = u16::try_from(intrin.base()).unwrap()
                    + u16::try_from(srcs[1].as_uint().unwrap()).unwrap();
                let freq = InterpFreq::Pass;
                let loc = match bary.intrinsic {
                    nir_intrinsic_load_barycentric_pixel => InterpLoc::Default,
                    _ => panic!("Unsupported interp mode"),
                };
                let dst = *self.get_dst(&intrin.def).as_ssa().unwrap();
                for c in 0..intrin.num_components {
                    self.instrs.push(Instr::new(Op::Ipa(OpIpa {
                        dst: dst[usize::from(c)].into(),
                        addr: addr + 4 * u16::from(c),
                        freq: freq,
                        loc: loc,
                        offset: SrcRef::Zero.into(),
                    })));
                }
            }
            nir_intrinsic_load_per_vertex_input => {
                let addr = u16::try_from(intrin.base()).unwrap();
                let vtx = self.get_src(&srcs[0]);
                let offset = self.get_src(&srcs[1]);
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new_ald(dst, addr, vtx, offset));
            }
            nir_intrinsic_load_scratch => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Local,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new_ld(dst, access, addr, offset));
            }
            nir_intrinsic_load_sysval_nv => {
                let idx = u8::try_from(intrin.base()).unwrap();
                let dst = self.get_dst(&intrin.def);
                self.instrs.push(Instr::new_s2r(dst, idx));
            }
            nir_intrinsic_load_ubo => {
                let idx = srcs[0];
                let offset = srcs[1];
                let dst = *self.get_dst(&intrin.def).as_ssa().unwrap();
                if let Some(imm_idx) = idx.as_uint() {
                    let imm_idx = u8::try_from(imm_idx).unwrap();
                    if let Some(imm_offset) = offset.as_uint() {
                        let imm_offset = u16::try_from(imm_offset).unwrap();
                        let mut pcopy = OpParCopy::new();
                        for (i, dst) in dst.iter().enumerate() {
                            let src = Src::new_cbuf(
                                imm_idx,
                                imm_offset + u16::try_from(i).unwrap() * 4,
                            );
                            pcopy.push(src, (*dst).into());
                        }
                        self.instrs.push(Instr::new(Op::ParCopy(pcopy)));
                    } else {
                        panic!("Indirect UBO offsets not yet supported");
                    }
                } else {
                    panic!("Indirect UBO indices not yet supported");
                }
            }
            nir_intrinsic_barrier => {
                if intrin.memory_scope() != SCOPE_NONE {
                    let mem_scope = match intrin.memory_scope() {
                        SCOPE_INVOCATION | SCOPE_SUBGROUP => MemScope::CTA,
                        SCOPE_WORKGROUP | SCOPE_QUEUE_FAMILY | SCOPE_DEVICE => {
                            MemScope::GPU
                        }
                        _ => panic!("Unhandled memory scope"),
                    };
                    self.instrs.push(OpMemBar { scope: mem_scope }.into());
                }
                if intrin.execution_scope() != SCOPE_NONE {
                    self.instrs.push(OpBar {}.into());
                }
            }
            nir_intrinsic_store_global => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A64,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Global,
                    order: MemOrder::Strong,
                    scope: MemScope::System,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 32);
                self.instrs.push(Instr::new_st(access, addr, offset, data));
            }
            nir_intrinsic_store_output => {
                if self.nir.info.stage() == MESA_SHADER_FRAGMENT {
                    /* We assume these only ever happen in the last block.
                     * This is ensured by nir_lower_io_to_temporaries()
                     */
                    let data = *self.get_src(&srcs[0]).as_ssa().unwrap();
                    assert!(srcs[1].is_zero());
                    let base: u8 = intrin.base().try_into().unwrap();
                    for c in 0..intrin.num_components {
                        self.fs_out_regs[usize::from(base + c)] =
                            data[usize::from(c)].into();
                    }
                } else {
                    let data = self.get_src(&srcs[0]);
                    let vtx = Src::new_zero();
                    let offset = self.get_src(&srcs[1]);
                    let addr: u16 = intrin.base().try_into().unwrap();
                    self.instrs.push(Instr::new_ast(addr, data, vtx, offset))
                }
            }
            nir_intrinsic_store_scratch => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Local,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 24);
                self.instrs.push(Instr::new_st(access, addr, offset, data));
            }
            _ => panic!(
                "Unsupported intrinsic instruction: {}",
                intrin.info().name()
            ),
        }
    }

    fn parse_load_const(&mut self, load_const: &nir_load_const_instr) {
        fn src_for_u32(u: u32) -> Src {
            if u == 0 {
                Src::new_zero()
            } else {
                Src::new_imm_u32(u)
            }
        }

        let mut pcopy = OpParCopy::new();
        for c in 0..load_const.def.num_components {
            if load_const.def.bit_size == 1 {
                let imm_b1 = unsafe { load_const.values()[c as usize].b };
                pcopy.srcs.push(Src::new_imm_bool(imm_b1));
            } else if load_const.def.bit_size == 32 {
                let imm_u32 = unsafe { load_const.values()[c as usize].u32_ };
                pcopy.srcs.push(src_for_u32(imm_u32));
            } else if load_const.def.bit_size == 64 {
                let imm_u64 = unsafe { load_const.values()[c as usize].u64_ };
                pcopy.srcs.push(src_for_u32(imm_u64 as u32));
                pcopy.srcs.push(src_for_u32((imm_u64 >> 32) as u32));
            }
        }

        for sv in self.get_ssa(&load_const.def) {
            pcopy.dsts.push((*sv).into());
        }

        assert!(pcopy.srcs.len() == pcopy.dsts.len());
        self.instrs.push(Instr::new(Op::ParCopy(pcopy)));
    }

    fn parse_undef(&mut self, undef: &nir_undef_instr) {
        let vec: Vec<_> = self.get_ssa(&undef.def).into();
        for ssa in vec {
            self.instrs.push(Instr::new(OpUndef { dst: ssa.into() }));
        }
    }

    fn parse_block(&mut self, nb: &nir_block) {
        let mut phi = OpPhiDsts::new();
        for ni in nb.iter_instr_list() {
            if ni.type_ == nir_instr_type_phi {
                let np = ni.as_phi().unwrap();
                let dst = *self.get_dst(&np.def).as_ssa().unwrap();
                for (i, dst) in dst.iter().enumerate() {
                    let phi_id = self.get_phi_id(np, i.try_into().unwrap());
                    phi.push(phi_id, (*dst).into());
                }
            } else {
                break;
            }
        }

        if !phi.ids.is_empty() {
            self.instrs.push(Instr::new(Op::PhiDsts(phi)));
        }

        for ni in nb.iter_instr_list() {
            match ni.type_ {
                nir_instr_type_alu => self.parse_alu(ni.as_alu().unwrap()),
                nir_instr_type_jump => self.parse_jump(ni.as_jump().unwrap()),
                nir_instr_type_tex => self.parse_tex(ni.as_tex().unwrap()),
                nir_instr_type_intrinsic => {
                    self.parse_intrinsic(ni.as_intrinsic().unwrap())
                }
                nir_instr_type_load_const => {
                    self.parse_load_const(ni.as_load_const().unwrap())
                }
                nir_instr_type_undef => {
                    self.parse_undef(ni.as_undef().unwrap())
                }
                nir_instr_type_phi => (),
                _ => panic!("Unsupported instruction type"),
            }
        }

        let succ = nb.successors();
        for sb in succ {
            let sb = match sb {
                Some(b) => b,
                None => continue,
            };

            let mut phi = OpPhiSrcs::new();

            for i in sb.iter_instr_list() {
                let np = match i.as_phi() {
                    Some(phi) => phi,
                    None => break,
                };

                for ps in np.iter_srcs() {
                    if ps.pred().index == nb.index {
                        let src = *self.get_src(&ps.src).as_ssa().unwrap();
                        for (i, src) in src.iter().enumerate() {
                            let phi_id =
                                self.get_phi_id(np, i.try_into().unwrap());
                            phi.push(phi_id, (*src).into());
                        }
                        break;
                    }
                }
            }

            if !phi.ids.is_empty() {
                self.instrs.push(Instr::new(Op::PhiSrcs(phi)));
            }
        }

        let s0 = succ[0].unwrap();
        if let Some(s1) = succ[1] {
            /* Jump to the else.  We'll come back and fix up the predicate as
             * part of our handling of nir_if.
             */
            self.instrs.push(Instr::new_bra(s1.index));
        } else if s0.index == self.end_block_id {
            self.instrs.push(Instr::new_exit());
        } else {
            self.instrs.push(Instr::new_bra(s0.index));
        }

        let mut b = BasicBlock::new(nb.index);
        b.instrs.append(&mut self.instrs);
        self.blocks.push(b);
    }

    fn parse_if(&mut self, ni: &nir_if) {
        let cond = self.get_ssa(&ni.condition.as_def())[0];

        let if_bra = self.blocks.last_mut().unwrap().branch_mut().unwrap();
        if_bra.pred = cond.into();
        /* This is the branch to jump to the else */
        if_bra.pred_inv = true;

        self.parse_cf_list(ni.iter_then_list());
        self.parse_cf_list(ni.iter_else_list());
    }

    fn parse_loop(&mut self, nl: &nir_loop) {
        self.parse_cf_list(nl.iter_body());
    }

    fn parse_cf_list(&mut self, list: ExecListIter<nir_cf_node>) {
        for node in list {
            match node.type_ {
                nir_cf_node_block => {
                    self.parse_block(node.as_block().unwrap());
                }
                nir_cf_node_if => {
                    self.parse_if(node.as_if().unwrap());
                }
                nir_cf_node_loop => {
                    self.parse_loop(node.as_loop().unwrap());
                }
                _ => panic!("Invalid inner CF node type"),
            }
        }
    }

    pub fn parse_function_impl(&mut self, nfi: &nir_function_impl) -> Function {
        self.func = Some(Function::new(0));
        self.end_block_id = nfi.end_block().index;

        self.parse_cf_list(nfi.iter_body());

        let end_block = self.blocks.last_mut().unwrap();

        if self.nir.info.stage() == MESA_SHADER_FRAGMENT
            && nfi.function().is_entrypoint
        {
            let fs_out_regs =
                std::mem::replace(&mut self.fs_out_regs, Vec::new());
            let fs_out = Instr::new_fs_out(&fs_out_regs);
            end_block.instrs.insert(end_block.instrs.len() - 1, fs_out);
        }

        let mut f = self.func.take().unwrap();
        f.blocks.append(&mut self.blocks);
        f
    }

    pub fn parse_shader(&mut self, sm: u8) -> Shader {
        let mut s = Shader::new(sm);
        for nf in self.nir.iter_functions() {
            if let Some(nfi) = nf.get_impl() {
                let f = self.parse_function_impl(nfi);
                s.functions.push(f);
            }
        }
        s
    }
}

pub fn nak_shader_from_nir(ns: &nir_shader, sm: u8) -> Shader {
    ShaderFromNir::new(ns).parse_shader(sm)
}
