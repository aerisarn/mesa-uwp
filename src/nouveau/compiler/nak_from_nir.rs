/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_upper_case_globals)]

use crate::nak_cfg::CFGBuilder;
use crate::nak_ir::*;
use crate::nak_sph::{OutputTopology, PixelImap};
use crate::nir::*;

use nak_bindings::*;

use std::cmp::{max, min};
use std::collections::{HashMap, HashSet};

fn init_info_from_nir(nir: &nir_shader, sm: u8) -> ShaderInfo {
    ShaderInfo {
        sm: sm,
        num_gprs: 0,
        num_barriers: 0,
        slm_size: nir.scratch_size,
        uses_global_mem: false,
        writes_global_mem: false,
        // TODO: handle this.
        uses_fp64: false,
        stage: match nir.info.stage() {
            MESA_SHADER_COMPUTE => {
                ShaderStageInfo::Compute(ComputeShaderInfo {
                    local_size: [
                        nir.info.workgroup_size[0].into(),
                        nir.info.workgroup_size[1].into(),
                        nir.info.workgroup_size[2].into(),
                    ],
                    smem_size: nir.info.shared_size.try_into().unwrap(),
                })
            }
            MESA_SHADER_VERTEX => ShaderStageInfo::Vertex,
            MESA_SHADER_FRAGMENT => ShaderStageInfo::Fragment,
            MESA_SHADER_GEOMETRY => {
                let info_gs = unsafe { &nir.info.__bindgen_anon_1.gs };
                let output_topology = match info_gs.output_primitive {
                    MESA_PRIM_POINTS => OutputTopology::PointList,
                    MESA_PRIM_LINE_STRIP => OutputTopology::LineStrip,
                    MESA_PRIM_TRIANGLE_STRIP => OutputTopology::TriangleStrip,
                    _ => panic!(
                        "Invalid GS input primitive {}",
                        info_gs.input_primitive
                    ),
                };

                ShaderStageInfo::Geometry(GeometryShaderInfo {
                    stream_out_mask: info_gs.active_stream_mask(),
                    threads_per_input_primitive: info_gs.invocations,
                    output_topology: output_topology,
                    max_output_vertex_count: info_gs.vertices_out,
                })
            }
            MESA_SHADER_TESS_CTRL => {
                let info_tess = unsafe { &nir.info.__bindgen_anon_1.tess };
                ShaderStageInfo::TessellationInit(TessellationInitShaderInfo {
                    per_patch_attribute_count: 6,
                    threads_per_patch: info_tess.tcs_vertices_out,
                })
            }
            MESA_SHADER_TESS_EVAL => ShaderStageInfo::Tessellation,
            _ => panic!("Unknown shader stage"),
        },
        io: match nir.info.stage() {
            MESA_SHADER_COMPUTE => ShaderIoInfo::None,
            MESA_SHADER_FRAGMENT => ShaderIoInfo::Fragment(FragmentIoInfo {
                sysvals_in: SysValInfo {
                    // Required on fragment shaders, otherwise it cause a trap.
                    ab: 1 << 31,
                    c: 0,
                },
                attr_in: [PixelImap::Unused; 128],
                reads_sample_mask: false,
                uses_kill: false,
                writes_color: 0,
                writes_sample_mask: false,
                writes_depth: false,
            }),
            MESA_SHADER_VERTEX
            | MESA_SHADER_GEOMETRY
            | MESA_SHADER_TESS_CTRL
            | MESA_SHADER_TESS_EVAL => ShaderIoInfo::Vtg(VtgIoInfo {
                sysvals_in: SysValInfo::default(),
                sysvals_out: SysValInfo::default(),
                attr_in: [0; 4],
                attr_out: [0; 4],

                // TODO: figure out how to fill this.
                store_req_start: u8::MAX,
                store_req_end: 0,
            }),
            _ => panic!("Unknown shader stage"),
        },
    }
}

fn alloc_ssa_for_nir(b: &mut impl SSABuilder, ssa: &nir_def) -> Vec<SSAValue> {
    let (file, comps) = if ssa.bit_size == 1 {
        (RegFile::Pred, ssa.num_components)
    } else {
        let bits = ssa.bit_size * ssa.num_components;
        (RegFile::GPR, bits.div_ceil(32))
    };

    let mut vec = Vec::new();
    for _ in 0..comps {
        vec.push(b.alloc_ssa(file, 1)[0]);
    }
    vec
}

struct PhiAllocMap<'a> {
    alloc: &'a mut PhiAllocator,
    map: HashMap<(u32, u8), u32>,
}

impl<'a> PhiAllocMap<'a> {
    fn new(alloc: &'a mut PhiAllocator) -> PhiAllocMap<'a> {
        PhiAllocMap {
            alloc: alloc,
            map: HashMap::new(),
        }
    }

    fn get_phi_id(&mut self, phi: &nir_phi_instr, comp: u8) -> u32 {
        *self
            .map
            .entry((phi.def.index, comp))
            .or_insert_with(|| self.alloc.alloc())
    }
}

struct BarAlloc {
    used: u16,
    num_bars: u8,
}

impl BarAlloc {
    pub fn new() -> BarAlloc {
        BarAlloc {
            used: 0,
            num_bars: 0,
        }
    }

    pub fn num_bars(&self) -> u8 {
        self.num_bars
    }

    pub fn reserve(&mut self, idx: u8) {
        self.num_bars = max(self.num_bars, idx + 1);
        let bit = 1 << idx;
        assert!(self.used & bit == 0);
        self.used |= bit;
    }

    pub fn alloc(&mut self) -> BarRef {
        let idx = self.used.trailing_ones();
        assert!(idx < 16);
        let idx = idx as u8;
        self.reserve(idx);
        BarRef::new(idx)
    }

    pub fn free(&mut self, bar: BarRef) {
        let bit = 1 << bar.idx();
        assert!(self.used & bit != 0);
        self.used &= !bit;
    }
}

struct ShaderFromNir<'a> {
    nir: &'a nir_shader,
    info: ShaderInfo,
    cfg: CFGBuilder<u32, BasicBlock>,
    label_alloc: LabelAllocator,
    block_label: HashMap<u32, Label>,
    bar_alloc: BarAlloc,
    bar_ref_label: HashMap<u32, (BarRef, Label)>,
    fs_out_regs: [SSAValue; 34],
    end_block_id: u32,
    ssa_map: HashMap<u32, Vec<SSAValue>>,
    saturated: HashSet<*const nir_def>,
}

impl<'a> ShaderFromNir<'a> {
    fn new(nir: &'a nir_shader, sm: u8) -> Self {
        Self {
            nir: nir,
            info: init_info_from_nir(nir, sm),
            cfg: CFGBuilder::new(),
            label_alloc: LabelAllocator::new(),
            block_label: HashMap::new(),
            bar_alloc: BarAlloc::new(),
            bar_ref_label: HashMap::new(),
            fs_out_regs: [SSAValue::NONE; 34],
            end_block_id: 0,
            ssa_map: HashMap::new(),
            saturated: HashSet::new(),
        }
    }

    fn get_block_label(&mut self, block: &nir_block) -> Label {
        *self
            .block_label
            .entry(block.index)
            .or_insert_with(|| self.label_alloc.alloc())
    }

    fn get_ssa(&mut self, ssa: &nir_def) -> &[SSAValue] {
        self.ssa_map.get(&ssa.index).unwrap()
    }

    fn set_ssa(&mut self, def: &nir_def, vec: Vec<SSAValue>) {
        if def.bit_size == 1 {
            for s in &vec {
                assert!(s.is_predicate());
            }
        } else {
            for s in &vec {
                assert!(!s.is_predicate());
            }
            let bits = def.bit_size * def.num_components;
            assert!(vec.len() == bits.div_ceil(32).into());
        }
        self.ssa_map
            .entry(def.index)
            .and_modify(|_| panic!("Cannot set an SSA def twice"))
            .or_insert(vec);
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

    fn set_dst(&mut self, def: &nir_def, ssa: SSARef) {
        self.set_ssa(def, (*ssa).into());
    }

    fn try_saturate_alu_dst(&mut self, def: &nir_def) -> bool {
        if def.all_uses_are_fsat() {
            self.saturated.insert(def as *const _);
            true
        } else {
            false
        }
    }

    fn alu_src_is_saturated(&self, src: &nir_alu_src) -> bool {
        self.saturated.get(&(src.as_def() as *const _)).is_some()
    }

    fn parse_alu(&mut self, b: &mut impl SSABuilder, alu: &nir_alu_instr) {
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
                let file = if alu.def.bit_size == 1 {
                    RegFile::Pred
                } else {
                    RegFile::GPR
                };

                let mut dst_vec = Vec::new();
                for src in srcs {
                    for v in src.as_ssa().unwrap().iter() {
                        let dst = b.alloc_ssa(file, 1)[0];
                        b.copy_to(dst.into(), (*v).into());
                        dst_vec.push(dst);
                    }
                }
                self.set_ssa(&alu.def, dst_vec);
                return;
            }
            _ => (),
        }

        let dst: SSARef = match alu.op {
            nir_op_b2b1 => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.isetp(IntCmpType::I32, IntCmpOp::Ne, srcs[0], 0.into())
            }
            nir_op_b2b32 | nir_op_b2i32 => {
                b.sel(srcs[0].bnot(), 0.into(), 1.into())
            }
            nir_op_b2f32 => {
                b.sel(srcs[0].bnot(), 0.0_f32.into(), 1.0_f32.into())
            }
            nir_op_bcsel => b.sel(srcs[0], srcs[1], srcs[2]),
            nir_op_bit_count => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpPopC {
                    dst: dst.into(),
                    src: srcs[0],
                });
                dst
            }
            nir_op_bitfield_reverse => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBrev {
                    dst: dst.into(),
                    src: srcs[0],
                });
                dst
            }
            nir_op_find_lsb => {
                let tmp = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBrev {
                    dst: tmp.into(),
                    src: srcs[0],
                });
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFlo {
                    dst: dst.into(),
                    src: tmp.into(),
                    signed: alu.op == nir_op_ifind_msb,
                    return_shift_amount: true,
                });
                dst
            }
            nir_op_f2i32 | nir_op_f2u32 => {
                let src_bits = usize::from(alu.get_src(0).bit_size());
                let dst_bits = alu.def.bit_size();
                let dst = b.alloc_ssa(RegFile::GPR, dst_bits.div_ceil(32));
                let dst_is_signed = alu.info().output_type & 2 != 0;
                b.push_op(OpF2I {
                    dst: dst.into(),
                    src: srcs[0],
                    src_type: FloatType::from_bits(src_bits),
                    dst_type: IntType::from_bits(
                        dst_bits.into(),
                        dst_is_signed,
                    ),
                    rnd_mode: FRndMode::Zero,
                });
                dst
            }
            nir_op_fabs | nir_op_fadd | nir_op_fneg => {
                let (x, y) = match alu.op {
                    nir_op_fabs => (srcs[0].fabs(), 0.0_f32.into()),
                    nir_op_fadd => (srcs[0], srcs[1]),
                    nir_op_fneg => (Src::new_zero().fneg(), srcs[0].fneg()),
                    _ => panic!("Unhandled case"),
                };
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let saturate = self.try_saturate_alu_dst(&alu.def);
                b.push_op(OpFAdd {
                    dst: dst.into(),
                    srcs: [x, y],
                    saturate: saturate,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_fceil | nir_op_ffloor | nir_op_fround_even
            | nir_op_ftrunc => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let ty = FloatType::from_bits(alu.def.bit_size().into());
                let rnd_mode = match alu.op {
                    nir_op_fceil => FRndMode::PosInf,
                    nir_op_ffloor => FRndMode::NegInf,
                    nir_op_ftrunc => FRndMode::Zero,
                    nir_op_fround_even => FRndMode::NearestEven,
                    _ => unreachable!(),
                };
                b.push_op(OpFRnd {
                    dst: dst.into(),
                    src: srcs[0],
                    src_type: ty,
                    dst_type: ty,
                    rnd_mode,
                });
                dst
            }
            nir_op_fcos => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp = b.fmul(srcs[0], frac_1_2pi.into());
                b.mufu(MuFuOp::Cos, tmp.into())
            }
            nir_op_feq => b.fsetp(FloatCmpOp::OrdEq, srcs[0], srcs[1]),
            nir_op_fexp2 => b.mufu(MuFuOp::Exp2, srcs[0]),
            nir_op_ffma => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let ffma = OpFFma {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], srcs[2]],
                    saturate: self.try_saturate_alu_dst(&alu.def),
                    rnd_mode: FRndMode::NearestEven,
                };
                b.push_op(ffma);
                dst
            }
            nir_op_fge => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.fsetp(FloatCmpOp::OrdGe, srcs[0], srcs[1])
            }
            nir_op_flog2 => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Log2, srcs[0])
            }
            nir_op_flt => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.fsetp(FloatCmpOp::OrdLt, srcs[0], srcs[1])
            }
            nir_op_fmax => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFMnMx {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::False.into(),
                });
                dst
            }
            nir_op_fmin => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFMnMx {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::True.into(),
                });
                dst
            }
            nir_op_fmul => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let fmul = OpFMul {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    saturate: self.try_saturate_alu_dst(&alu.def),
                    rnd_mode: FRndMode::NearestEven,
                };
                b.push_op(fmul);
                dst
            }
            nir_op_fneu => b.fsetp(FloatCmpOp::UnordNe, srcs[0], srcs[1]),
            nir_op_fquantize2f16 => {
                let tmp = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpF2F {
                    dst: tmp.into(),
                    src: srcs[0],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: true,
                    high: false,
                });
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpF2F {
                    dst: dst.into(),
                    src: tmp.into(),
                    src_type: FloatType::F16,
                    dst_type: FloatType::F32,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: true,
                    high: false,
                });
                dst
            }
            nir_op_frcp => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Rcp, srcs[0])
            }
            nir_op_frsq => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Rsq, srcs[0])
            }
            nir_op_fsat => {
                assert!(alu.def.bit_size() == 32);
                if self.alu_src_is_saturated(&alu.srcs_as_slice()[0]) {
                    b.copy(srcs[0])
                } else {
                    let dst = b.alloc_ssa(RegFile::GPR, 1);
                    b.push_op(OpFAdd {
                        dst: dst.into(),
                        srcs: [srcs[0], 0.into()],
                        saturate: true,
                        rnd_mode: FRndMode::NearestEven,
                    });
                    dst
                }
            }
            nir_op_fsign => {
                assert!(alu.def.bit_size() == 32);
                let lz = b.fset(FloatCmpOp::OrdLt, srcs[0], 0.into());
                let gz = b.fset(FloatCmpOp::OrdGt, srcs[0], 0.into());
                b.fadd(gz.into(), Src::from(lz).fneg())
            }
            nir_op_fsin => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp = b.fmul(srcs[0], frac_1_2pi.into());
                b.mufu(MuFuOp::Sin, tmp.into())
            }
            nir_op_fsqrt => b.mufu(MuFuOp::Sqrt, srcs[0]),
            nir_op_i2f32 => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpI2F {
                    dst: dst.into(),
                    src: srcs[0],
                    dst_type: FloatType::F32,
                    src_type: IntType::I32,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_iabs => b.iabs(srcs[0]),
            nir_op_iadd => {
                if alu.def.bit_size == 64 {
                    let x = srcs[0].as_ssa().unwrap();
                    let y = srcs[1].as_ssa().unwrap();
                    let sum = b.alloc_ssa(RegFile::GPR, 2);
                    let carry = b.alloc_ssa(RegFile::Pred, 1);
                    b.push_op(OpIAdd3X {
                        dst: sum[0].into(),
                        overflow: [carry.into(), Dst::None],
                        high: false,
                        srcs: [x[0].into(), y[0].into(), 0.into()],
                        carry: [SrcRef::False.into(), SrcRef::False.into()],
                    });
                    b.push_op(OpIAdd3X {
                        dst: sum[1].into(),
                        overflow: [Dst::None, Dst::None],
                        high: true,
                        srcs: [x[1].into(), y[1].into(), 0.into()],
                        carry: [carry.into(), SrcRef::False.into()],
                    });
                    sum
                } else {
                    assert!(alu.def.bit_size() == 32);
                    b.iadd(srcs[0], srcs[1])
                }
            }
            nir_op_iand => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x & y), srcs[0], srcs[1])
            }
            nir_op_ieq => {
                if alu.get_src(0).bit_size() == 1 {
                    let lop = LogicOp::new_lut(&|x, y, _| !(x ^ y));
                    b.lop2(lop, srcs[0], srcs[1])
                } else {
                    b.isetp(IntCmpType::I32, IntCmpOp::Eq, srcs[0], srcs[1])
                }
            }
            nir_op_ifind_msb | nir_op_ufind_msb => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFlo {
                    dst: dst.into(),
                    src: srcs[0],
                    signed: alu.op == nir_op_ifind_msb,
                    return_shift_amount: false,
                });
                dst
            }
            nir_op_ige => {
                b.isetp(IntCmpType::I32, IntCmpOp::Ge, srcs[0], srcs[1])
            }
            nir_op_ilt => {
                b.isetp(IntCmpType::I32, IntCmpOp::Lt, srcs[0], srcs[1])
            }
            nir_op_ine => {
                if alu.get_src(0).bit_size() == 1 {
                    let lop = LogicOp::new_lut(&|x, y, _| (x ^ y));
                    b.lop2(lop, srcs[0], srcs[1])
                } else {
                    b.isetp(IntCmpType::I32, IntCmpOp::Ne, srcs[0], srcs[1])
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
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIMnMx {
                    dst: dst.into(),
                    cmp_type: tp,
                    srcs: [srcs[0], srcs[1]],
                    min: min.into(),
                });
                dst
            }
            nir_op_imul => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIMad {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], 0.into()],
                    signed: false,
                });
                dst
            }
            nir_op_imul_2x32_64 | nir_op_umul_2x32_64 => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                b.push_op(OpIMad64 {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], 0.into()],
                    signed: alu.op == nir_op_imul_2x32_64,
                });
                dst
            }
            nir_op_imul_high | nir_op_umul_high => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                b.push_op(OpIMad64 {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], 0.into()],
                    signed: alu.op == nir_op_imul_high,
                });
                dst[1].into()
            }
            nir_op_ineg => b.ineg(srcs[0]),
            nir_op_inot => {
                let lop = LogicOp::new_lut(&|x, _, _| !x);
                if alu.def.bit_size() == 1 {
                    b.lop2(lop, srcs[0], true.into())
                } else {
                    assert!(alu.def.bit_size() == 32);
                    b.lop2(lop, srcs[0], 0.into())
                }
            }
            nir_op_ior => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x | y), srcs[0], srcs[1])
            }
            nir_op_ishl => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: srcs[0],
                    high: 0.into(),
                    shift: srcs[1],
                    right: false,
                    wrap: true,
                    data_type: IntType::I32,
                    dst_high: false,
                });
                dst
            }
            nir_op_ishr => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: 0.into(),
                    high: srcs[0],
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::I32,
                    dst_high: true,
                });
                dst
            }
            nir_op_isign => {
                let gt_pred = b.alloc_ssa(RegFile::Pred, 1);
                let lt_pred = b.alloc_ssa(RegFile::Pred, 1);
                let gt = b.alloc_ssa(RegFile::GPR, 1);
                let lt = b.alloc_ssa(RegFile::GPR, 1);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpISetP {
                    dst: gt_pred.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Gt,
                    cmp_type: IntCmpType::I32,
                    srcs: [srcs[0], 0.into()],
                    accum: true.into(),
                });

                let cond = Src::from(gt_pred).bnot();
                b.push_op(OpSel {
                    dst: gt.into(),
                    cond,
                    srcs: [0.into(), u32::MAX.into()],
                });
                b.push_op(OpISetP {
                    dst: lt_pred.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Lt,
                    cmp_type: IntCmpType::I32,
                    srcs: [srcs[0], 0.into()],
                    accum: true.into(),
                });

                let cond = Src::from(lt_pred).bnot();
                b.push_op(OpSel {
                    dst: lt.into(),
                    cond,
                    srcs: [0.into(), u32::MAX.into()],
                });

                let dst_is_signed = alu.info().output_type & 2 != 0;
                let dst_type = IntType::from_bits(
                    alu.def.bit_size().into(),
                    dst_is_signed,
                );
                match dst_type {
                    IntType::I32 => {
                        let gt_neg = b.ineg(gt.into());
                        b.push_op(OpIAdd3 {
                            dst: dst.into(),
                            srcs: [lt.into(), gt_neg.into(), 0.into()],
                        });
                    }
                    IntType::I64 => {
                        let high = b.alloc_ssa(RegFile::GPR, 1);
                        let gt_neg = b.ineg(gt.into());
                        b.push_op(OpIAdd3 {
                            dst: high.into(),
                            srcs: [lt.into(), gt_neg.into(), 0.into()],
                        });
                        b.push_op(OpShf {
                            dst: dst.into(),
                            low: 0.into(),
                            high: high.into(),
                            shift: 31_u32.into(),
                            right: true,
                            wrap: true,
                            data_type: dst_type,
                            dst_high: true,
                        });
                    }
                    _ => panic!("Invalid IntType {}", dst_type),
                }
                dst
            }
            nir_op_ixor => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x ^ y), srcs[0], srcs[1])
            }
            nir_op_pack_64_2x32_split => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                b.copy_to(dst[0].into(), srcs[0]);
                b.copy_to(dst[1].into(), srcs[1]);
                dst
            }
            nir_op_pack_half_2x16_split => {
                assert!(alu.get_src(0).bit_size() == 32);
                let low = b.alloc_ssa(RegFile::GPR, 1);
                let high = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpF2F {
                    dst: low.into(),
                    src: srcs[0],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: false,
                });

                let src_bits = usize::from(alu.get_src(1).bit_size());
                let src_type = FloatType::from_bits(src_bits);
                assert!(matches!(src_type, FloatType::F32));
                b.push_op(OpF2F {
                    dst: high.into(),
                    src: srcs[1],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: false,
                });

                let dst = b.alloc_ssa(RegFile::GPR, 1);

                let selection = PrmtSelectionEval::from([
                    PrmtSelection {
                        src: PrmtSrc::Byte5,
                        sign_extend: false,
                    },
                    PrmtSelection {
                        src: PrmtSrc::Byte4,
                        sign_extend: false,
                    },
                    PrmtSelection {
                        src: PrmtSrc::Byte1,
                        sign_extend: false,
                    },
                    PrmtSelection {
                        src: PrmtSrc::Byte0,
                        sign_extend: false,
                    },
                ]);

                b.push_op(OpPrmt {
                    dst: dst.into(),
                    srcs: [low.into(), high.into()],
                    selection: selection.inner().into(),
                });
                dst
            }
            nir_op_u2f32 => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpI2F {
                    dst: dst.into(),
                    src: srcs[0],
                    dst_type: FloatType::F32,
                    src_type: IntType::U32,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_uge => {
                b.isetp(IntCmpType::U32, IntCmpOp::Ge, srcs[0], srcs[1])
            }
            nir_op_ult => {
                b.isetp(IntCmpType::U32, IntCmpOp::Lt, srcs[0], srcs[1])
            }
            nir_op_unpack_64_2x32_split_x => {
                let src0_x = srcs[0].as_ssa().unwrap()[0];
                b.copy(src0_x.into())
            }
            nir_op_unpack_64_2x32_split_y => {
                let src0_y = srcs[0].as_ssa().unwrap()[1];
                b.copy(src0_y.into())
            }
            nir_op_unpack_half_2x16_split_x
            | nir_op_unpack_half_2x16_split_y => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpF2F {
                    dst: dst[0].into(),
                    src: srcs[0],
                    src_type: FloatType::F16,
                    dst_type: FloatType::F32,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: alu.op == nir_op_unpack_half_2x16_split_y,
                });

                dst
            }
            nir_op_ushr => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: srcs[0],
                    high: 0.into(),
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::U32,
                    dst_high: false,
                });
                dst
            }
            nir_op_fddx | nir_op_fddx_coarse | nir_op_fddx_fine => {
                // TODO: Real coarse derivatives

                assert!(alu.def.bit_size() == 32);
                let scratch = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpShfl {
                    dst: scratch[0].into(),
                    src: srcs[0],
                    lane: 1_u32.into(),
                    c: (0x3_u32 | 0x1c_u32 << 8).into(),
                    op: ShflOp::Bfly,
                });

                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpFSwzAdd {
                    dst: dst[0].into(),
                    srcs: [scratch[0].into(), srcs[0]],
                    ops: [
                        FSwzAddOp::SubLeft,
                        FSwzAddOp::SubRight,
                        FSwzAddOp::SubLeft,
                        FSwzAddOp::SubRight,
                    ],
                    rnd_mode: FRndMode::NearestEven,
                });

                dst
            }
            nir_op_fddy | nir_op_fddy_coarse | nir_op_fddy_fine => {
                // TODO: Real coarse derivatives

                assert!(alu.def.bit_size() == 32);
                let scratch = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpShfl {
                    dst: scratch[0].into(),
                    src: srcs[0],
                    lane: 2_u32.into(),
                    c: (0x3_u32 | 0x1c_u32 << 8).into(),
                    op: ShflOp::Bfly,
                });

                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpFSwzAdd {
                    dst: dst[0].into(),
                    srcs: [scratch[0].into(), srcs[0]],
                    ops: [
                        FSwzAddOp::SubLeft,
                        FSwzAddOp::SubLeft,
                        FSwzAddOp::SubRight,
                        FSwzAddOp::SubRight,
                    ],
                    rnd_mode: FRndMode::NearestEven,
                });

                dst
            }
            _ => panic!("Unsupported ALU instruction: {}", alu.info().name()),
        };
        self.set_dst(&alu.def, dst);
    }

    fn parse_jump(&mut self, _b: &mut impl SSABuilder, _jump: &nir_jump_instr) {
        /* Nothing to do */
    }

    fn parse_tex(&mut self, b: &mut impl SSABuilder, tex: &nir_tex_instr) {
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

        let dst_comps = u8::try_from(mask.count_ones()).unwrap();
        let mut dsts = [Dst::None; 2];
        dsts[0] = b.alloc_ssa(RegFile::GPR, min(dst_comps, 2)).into();
        if dst_comps > 2 {
            dsts[1] = b.alloc_ssa(RegFile::GPR, dst_comps - 2).into();
        }

        if tex.op == nir_texop_hdr_dim_nv {
            let src = self.get_src(&srcs[0].src);
            b.push_op(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::Dimension,
                mask: mask,
            });
        } else if tex.op == nir_texop_tex_type_nv {
            let src = self.get_src(&srcs[0].src);
            b.push_op(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::TextureType,
                mask: mask,
            });
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
                b.push_op(OpTxd {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            } else if tex.op == nir_texop_lod {
                assert!(offset_mode == Tld4OffsetMode::None);
                b.push_op(OpTmml {
                    dsts: dsts,
                    srcs: srcs,
                    dim: dim,
                    mask: mask,
                });
            } else if tex.op == nir_texop_txf || tex.op == nir_texop_txf_ms {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                b.push_op(OpTld {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    is_ms: tex.op == nir_texop_txf_ms,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            } else if tex.op == nir_texop_tg4 {
                b.push_op(OpTld4 {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    comp: tex.component().try_into().unwrap(),
                    offset_mode: offset_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    mask: mask,
                });
            } else {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                b.push_op(OpTex {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            }
        }

        let mut di = 0_usize;
        let mut nir_dst = Vec::new();
        for i in 0..tex.def.num_components() {
            if mask & (1 << i) == 0 {
                nir_dst.push(b.copy(0.into())[0]);
            } else {
                nir_dst.push(dsts[di / 2].as_ssa().unwrap()[di % 2].into());
                di += 1;
            }
        }
        self.set_ssa(&tex.def.as_def(), nir_dst);
    }

    fn get_atomic_type(&self, intrin: &nir_intrinsic_instr) -> AtomType {
        let bit_size = intrin.def.bit_size();
        match intrin.atomic_op() {
            nir_atomic_op_iadd => AtomType::U(bit_size),
            nir_atomic_op_imin => AtomType::I(bit_size),
            nir_atomic_op_umin => AtomType::U(bit_size),
            nir_atomic_op_imax => AtomType::I(bit_size),
            nir_atomic_op_umax => AtomType::U(bit_size),
            nir_atomic_op_iand => AtomType::U(bit_size),
            nir_atomic_op_ior => AtomType::U(bit_size),
            nir_atomic_op_ixor => AtomType::U(bit_size),
            nir_atomic_op_xchg => AtomType::U(bit_size),
            nir_atomic_op_fadd => AtomType::F(bit_size),
            nir_atomic_op_fmin => AtomType::F(bit_size),
            nir_atomic_op_fmax => AtomType::F(bit_size),
            nir_atomic_op_cmpxchg => AtomType::U(bit_size),
            _ => panic!("Unsupported NIR atomic op"),
        }
    }

    fn get_atomic_op(&self, intrin: &nir_intrinsic_instr) -> AtomOp {
        match intrin.atomic_op() {
            nir_atomic_op_iadd => AtomOp::Add,
            nir_atomic_op_imin => AtomOp::Min,
            nir_atomic_op_umin => AtomOp::Min,
            nir_atomic_op_imax => AtomOp::Max,
            nir_atomic_op_umax => AtomOp::Max,
            nir_atomic_op_iand => AtomOp::And,
            nir_atomic_op_ior => AtomOp::Or,
            nir_atomic_op_ixor => AtomOp::Xor,
            nir_atomic_op_xchg => AtomOp::Exch,
            nir_atomic_op_fadd => AtomOp::Add,
            nir_atomic_op_fmin => AtomOp::Min,
            nir_atomic_op_fmax => AtomOp::Max,
            nir_atomic_op_cmpxchg => AtomOp::CmpExch,
            _ => panic!("Unsupported NIR atomic op"),
        }
    }

    fn get_eviction_priority(
        &mut self,
        access: gl_access_qualifier,
    ) -> MemEvictionPriority {
        if self.info.sm >= 70 && access & ACCESS_NON_TEMPORAL != 0 {
            MemEvictionPriority::First
        } else {
            MemEvictionPriority::Normal
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

    fn parse_intrinsic(
        &mut self,
        b: &mut impl SSABuilder,
        intrin: &nir_intrinsic_instr,
    ) {
        let srcs = intrin.srcs_as_slice();
        match intrin.intrinsic {
            nir_intrinsic_al2p_nv => {
                let offset = self.get_src(&srcs[0]);
                let addr = u16::try_from(intrin.base()).unwrap();

                let flags = intrin.flags();
                let flags: nak_nir_attr_io_flags =
                    unsafe { std::mem::transmute_copy(&flags) };

                let access = AttrAccess {
                    addr: addr,
                    comps: 1,
                    patch: flags.patch(),
                    output: flags.output(),
                    phys: false,
                };

                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpAL2P {
                    dst: dst.into(),
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_ald_nv | nir_intrinsic_ast_nv => {
                let addr = u16::try_from(intrin.base()).unwrap();
                let base = u16::try_from(intrin.range_base()).unwrap();
                let range = u16::try_from(intrin.range()).unwrap();
                let range = base..(base + range);

                let flags = intrin.flags();
                let flags: nak_nir_attr_io_flags =
                    unsafe { std::mem::transmute_copy(&flags) };
                assert!(!flags.patch() || !flags.phys());

                if let ShaderIoInfo::Vtg(io) = &mut self.info.io {
                    if flags.patch() {
                        match &mut self.info.stage {
                            ShaderStageInfo::TessellationInit(stage) => {
                                assert!(flags.output());
                                stage.per_patch_attribute_count = max(
                                    stage.per_patch_attribute_count,
                                    (range.end / 4).try_into().unwrap(),
                                );
                            }
                            ShaderStageInfo::Tessellation => (),
                            _ => panic!("Patch I/O not supported"),
                        }
                    } else {
                        if flags.output() {
                            if intrin.intrinsic == nir_intrinsic_ast_nv {
                                io.mark_store_req(range.clone());
                            }
                            io.mark_attrs_written(range);
                        } else {
                            io.mark_attrs_read(range);
                        }
                    }
                } else {
                    panic!("Must be a VTG stage");
                }

                let access = AttrAccess {
                    addr: addr,
                    comps: intrin.num_components,
                    patch: flags.patch(),
                    output: flags.output(),
                    phys: flags.phys(),
                };

                if intrin.intrinsic == nir_intrinsic_ald_nv {
                    let vtx = self.get_src(&srcs[0]);
                    let offset = self.get_src(&srcs[1]);

                    assert!(intrin.def.bit_size() == 32);
                    let dst = b.alloc_ssa(RegFile::GPR, access.comps);
                    b.push_op(OpALd {
                        dst: dst.into(),
                        vtx: vtx,
                        offset: offset,
                        access: access,
                    });
                    self.set_dst(&intrin.def, dst);
                } else if intrin.intrinsic == nir_intrinsic_ast_nv {
                    assert!(srcs[0].bit_size() == 32);
                    let data = self.get_src(&srcs[0]);
                    let vtx = self.get_src(&srcs[1]);
                    let offset = self.get_src(&srcs[2]);

                    b.push_op(OpASt {
                        data: data,
                        vtx: vtx,
                        offset: offset,
                        access: access,
                    });
                } else {
                    panic!("Invalid VTG I/O intrinsic");
                }
            }
            nir_intrinsic_ballot => {
                assert!(srcs[0].bit_size() == 1);
                let src = self.get_src(&srcs[0]);

                assert!(intrin.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpVote {
                    op: VoteOp::Any,
                    ballot: dst.into(),
                    vote: Dst::None,
                    pred: src,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_bar_break_nv => {
                let idx = &srcs[0].as_def().index;
                let (bar, _) = self.bar_ref_label.get(idx).unwrap();

                let brk = b.push_op(OpBreak {
                    bar: *bar,
                    cond: SrcRef::True.into(),
                });
                brk.deps.yld = true;
            }
            nir_intrinsic_bar_set_nv => {
                let label = self.label_alloc.alloc();
                let bar = self.bar_alloc.alloc();

                let bmov = b.push_op(OpBMov {
                    dst: Dst::None,
                    src: BMovSrc::Barrier(bar),
                    clear: true,
                });
                bmov.deps.yld = true;

                let bssy = b.push_op(OpBSSy {
                    bar: bar,
                    cond: SrcRef::True.into(),
                    target: label,
                });
                bssy.deps.yld = true;

                let old =
                    self.bar_ref_label.insert(intrin.def.index, (bar, label));
                assert!(old.is_none());
            }
            nir_intrinsic_bar_sync_nv => {
                let idx = &srcs[0].as_def().index;
                let (bar, label) = self.bar_ref_label.get(idx).unwrap();

                let bsync = b.push_op(OpBSync {
                    bar: *bar,
                    cond: SrcRef::True.into(),
                });
                bsync.deps.yld = true;

                self.bar_alloc.free(*bar);

                b.push_op(OpNop {
                    label: Some(*label),
                });
            }
            nir_intrinsic_bindless_image_atomic
            | nir_intrinsic_bindless_image_atomic_swap => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.bit_size() == 32);
                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                let data = if intrin.intrinsic
                    == nir_intrinsic_bindless_image_atomic_swap
                {
                    SSARef::from([
                        self.get_ssa(srcs[3].as_def())[0],
                        self.get_ssa(srcs[4].as_def())[0],
                    ])
                    .into()
                } else {
                    self.get_src(&srcs[3])
                };

                b.push_op(OpSuAtom {
                    dst: dst.into(),
                    resident: Dst::None,
                    handle: handle,
                    coord: coord,
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    image_dim: dim,
                    mem_order: MemOrder::Strong(MemScope::System),
                    mem_eviction_priority: self
                        .get_eviction_priority(intrin.access()),
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_bindless_image_load => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */

                let comps = u8::try_from(intrin.num_components).unwrap();
                assert!(intrin.def.bit_size() == 32);
                assert!(comps == 1 || comps == 4);

                let dst = b.alloc_ssa(RegFile::GPR, comps);

                b.push_op(OpSuLd {
                    dst: dst.into(),
                    resident: Dst::None,
                    image_dim: dim,
                    mem_order: MemOrder::Strong(MemScope::System),
                    mem_eviction_priority: self
                        .get_eviction_priority(intrin.access()),
                    mask: (1 << comps) - 1,
                    handle: handle,
                    coord: coord,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_bindless_image_store => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let data = self.get_src(&srcs[3]);

                let comps = u8::try_from(intrin.num_components).unwrap();
                assert!(srcs[3].bit_size() == 32);
                assert!(comps == 1 || comps == 4);

                b.push_op(OpSuSt {
                    image_dim: dim,
                    mem_order: MemOrder::Strong(MemScope::System),
                    mem_eviction_priority: self
                        .get_eviction_priority(intrin.access()),
                    mask: (1 << comps) - 1,
                    handle: handle,
                    coord: coord,
                    data: data,
                });
            }
            nir_intrinsic_demote
            | nir_intrinsic_discard
            | nir_intrinsic_terminate => {
                if let ShaderIoInfo::Fragment(info) = &mut self.info.io {
                    info.uses_kill = true;
                } else {
                    panic!("OpKill is only available in fragment shaders");
                }
                b.push_op(OpKill {});

                if intrin.intrinsic == nir_intrinsic_terminate {
                    b.push_op(OpExit {});
                }
            }
            nir_intrinsic_demote_if
            | nir_intrinsic_discard_if
            | nir_intrinsic_terminate_if => {
                if let ShaderIoInfo::Fragment(info) = &mut self.info.io {
                    info.uses_kill = true;
                } else {
                    panic!("OpKill is only available in fragment shaders");
                }
                let cond = self.get_ssa(&srcs[0].as_def())[0];
                b.predicate(cond.into()).push_op(OpKill {});

                if intrin.intrinsic == nir_intrinsic_terminate_if {
                    b.predicate(cond.into()).push_op(OpExit {});
                }
            }
            nir_intrinsic_global_atomic => {
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let data = self.get_src(&srcs[1]);
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: 0.into(),
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A64,
                    addr_offset: offset,
                    mem_space: MemSpace::Global,
                    mem_order: MemOrder::Strong(MemScope::System),
                    mem_eviction_priority: MemEvictionPriority::Normal, // Note: no intrinic access
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_global_atomic_swap => {
                assert!(intrin.atomic_op() == nir_atomic_op_cmpxchg);
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let cmpr = self.get_src(&srcs[1]);
                let data = self.get_src(&srcs[2]);
                let atom_type = AtomType::U(bit_size);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: cmpr,
                    data: data,
                    atom_op: AtomOp::CmpExch,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A64,
                    addr_offset: offset,
                    mem_space: MemSpace::Global,
                    mem_order: MemOrder::Strong(MemScope::System),
                    mem_eviction_priority: MemEvictionPriority::Normal, // Note: no intrinic access
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_ipa_nv => {
                let addr = u16::try_from(intrin.base()).unwrap();

                let flags = intrin.flags();
                let flags: nak_nir_ipa_flags =
                    unsafe { std::mem::transmute_copy(&flags) };

                let mode = match flags.interp_mode() {
                    NAK_INTERP_MODE_PERSPECTIVE => PixelImap::Perspective,
                    NAK_INTERP_MODE_SCREEN_LINEAR => PixelImap::ScreenLinear,
                    NAK_INTERP_MODE_CONSTANT => PixelImap::Constant,
                    _ => panic!("Unsupported interp mode"),
                };

                let freq = match flags.interp_freq() {
                    NAK_INTERP_FREQ_PASS => InterpFreq::Pass,
                    NAK_INTERP_FREQ_CONSTANT => InterpFreq::Constant,
                    NAK_INTERP_FREQ_STATE => InterpFreq::State,
                    _ => panic!("Invalid interp freq"),
                };

                let loc = match flags.interp_loc() {
                    NAK_INTERP_LOC_DEFAULT => InterpLoc::Default,
                    NAK_INTERP_LOC_CENTROID => InterpLoc::Centroid,
                    NAK_INTERP_LOC_OFFSET => InterpLoc::Offset,
                    _ => panic!("Invalid interp loc"),
                };

                let offset = if loc == InterpLoc::Offset {
                    self.get_src(&srcs[1])
                } else {
                    0.into()
                };

                let ShaderIoInfo::Fragment(io) = &mut self.info.io else {
                    panic!("OpIpa is only used for fragment shaders");
                };

                io.mark_attr_read(addr, mode);

                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIpa {
                    dst: dst.into(),
                    addr: addr,
                    freq: freq,
                    loc: loc,
                    offset: offset,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_isberd_nv => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIsberd {
                    dst: dst.into(),
                    idx: self.get_src(&srcs[0]),
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_barycentric_at_offset_nv => (),
            nir_intrinsic_load_barycentric_centroid => (),
            nir_intrinsic_load_barycentric_pixel => (),
            nir_intrinsic_load_barycentric_sample => (),
            nir_intrinsic_load_global | nir_intrinsic_load_global_constant => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let order =
                    if intrin.intrinsic == nir_intrinsic_load_global_constant {
                        MemOrder::Constant
                    } else {
                        MemOrder::Strong(MemScope::System)
                    };
                let access = MemAccess {
                    addr_type: MemAddrType::A64,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Global,
                    order: order,
                    eviction_priority: self
                        .get_eviction_priority(intrin.access()),
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 32);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_sample_id => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpPixLd {
                    dst: dst.into(),
                    val: PixVal::MyIndex,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_sample_mask_in => {
                if let ShaderIoInfo::Fragment(info) = &mut self.info.io {
                    info.reads_sample_mask = true;
                } else {
                    panic!(
                        "sample_mask_in is only available in fragment shaders"
                    );
                }

                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpPixLd {
                    dst: dst.into(),
                    val: PixVal::CovMask,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_tess_coord_xy => {
                // Loading gl_TessCoord in tessellation evaluation shaders is
                // weird.  It's treated as a per-vertex output which is indexed
                // by LANEID.
                match &self.info.stage {
                    ShaderStageInfo::Tessellation => (),
                    _ => panic!(
                        "load_tess_coord is only available in tessellation \
                         shaders"
                    ),
                };

                assert!(intrin.def.bit_size() == 32);
                assert!(intrin.def.num_components() == 2);

                let vtx = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpS2R {
                    dst: vtx.into(),
                    idx: 0,
                });

                let access = AttrAccess {
                    addr: NAK_ATTR_TESS_COORD,
                    comps: 2,
                    patch: false,
                    output: true,
                    phys: false,
                };

                // This is recorded as a patch output in parse_shader() because
                // the hardware requires it be in the SPH, whether we use it or
                // not.

                let dst = b.alloc_ssa(RegFile::GPR, access.comps);
                b.push_op(OpALd {
                    dst: dst.into(),
                    vtx: vtx.into(),
                    offset: 0.into(),
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_scratch => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Local,
                    order: MemOrder::Strong(MemScope::CTA),
                    eviction_priority: MemEvictionPriority::Normal,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_shared => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Shared,
                    order: MemOrder::Strong(MemScope::CTA),
                    eviction_priority: MemEvictionPriority::Normal,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let offset = offset + intrin.base();
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_sysval_nv => {
                let idx = u8::try_from(intrin.base()).unwrap();
                debug_assert!(intrin.def.num_components == 1);
                let dst = b.alloc_ssa(RegFile::GPR, intrin.def.bit_size() / 32);
                if intrin.def.bit_size() == 32 {
                    b.push_op(OpS2R {
                        dst: dst.into(),
                        idx: idx,
                    });
                } else if intrin.def.bit_size() == 64 {
                    b.push_op(OpCS2R {
                        dst: dst.into(),
                        idx: idx,
                    });
                } else {
                    panic!("Unknown sysval_nv bit size");
                }
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_ubo => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                let idx = srcs[0];
                let (off, off_imm) = self.get_io_addr_offset(&srcs[1], 16);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                if let Some(idx_imm) = idx.as_uint() {
                    let cb = CBufRef {
                        buf: CBuf::Binding(idx_imm.try_into().unwrap()),
                        offset: off_imm.try_into().unwrap(),
                    };
                    if off.is_zero() {
                        for (i, comp) in dst.iter().enumerate() {
                            let i = u16::try_from(i).unwrap();
                            b.copy_to((*comp).into(), cb.offset(i * 4).into());
                        }
                    } else {
                        b.push_op(OpLdc {
                            dst: dst.into(),
                            cb: cb.into(),
                            offset: off,
                            mem_type: MemType::from_size(size_B, false),
                        });
                    }
                } else {
                    panic!("Indirect UBO indices not yet supported");
                }
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_barrier => {
                let modes = intrin.memory_modes();
                let semantics = intrin.memory_semantics();
                if (modes & nir_var_mem_global) != 0
                    && (semantics & NIR_MEMORY_RELEASE) != 0
                {
                    b.push_op(OpCCtl {
                        op: CCtlOp::WBAll,
                        mem_space: MemSpace::Global,
                        addr: 0.into(),
                        addr_offset: 0,
                    });
                }
                match intrin.execution_scope() {
                    SCOPE_NONE => (),
                    SCOPE_WORKGROUP => {
                        if self.nir.info.stage() == MESA_SHADER_COMPUTE {
                            b.push_op(OpBar {}).deps.yld = true;
                            b.push_op(OpNop { label: None });
                        }
                    }
                    _ => panic!("Unhandled execution scope"),
                }
                if intrin.memory_scope() != SCOPE_NONE {
                    let mem_scope = match intrin.memory_scope() {
                        SCOPE_INVOCATION | SCOPE_SUBGROUP => MemScope::CTA,
                        SCOPE_WORKGROUP | SCOPE_QUEUE_FAMILY | SCOPE_DEVICE => {
                            MemScope::GPU
                        }
                        _ => panic!("Unhandled memory scope"),
                    };
                    b.push_op(OpMemBar { scope: mem_scope });
                }
                if (modes & nir_var_mem_global) != 0
                    && (semantics & NIR_MEMORY_ACQUIRE) != 0
                {
                    b.push_op(OpCCtl {
                        op: CCtlOp::IVAll,
                        mem_space: MemSpace::Global,
                        addr: 0.into(),
                        addr_offset: 0,
                    });
                }
            }
            nir_intrinsic_read_invocation
            | nir_intrinsic_shuffle
            | nir_intrinsic_shuffle_down
            | nir_intrinsic_shuffle_up
            | nir_intrinsic_shuffle_xor => {
                assert!(srcs[0].bit_size() == 32);
                assert!(srcs[0].num_components() == 1);
                let data = self.get_src(&srcs[0]);

                assert!(srcs[1].bit_size() == 32);
                let idx = self.get_src(&srcs[1]);

                assert!(intrin.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpShfl {
                    dst: dst.into(),
                    src: data,
                    lane: idx,
                    c: 0x1f.into(),
                    op: match intrin.intrinsic {
                        nir_intrinsic_read_invocation
                        | nir_intrinsic_shuffle => ShflOp::Idx,
                        nir_intrinsic_shuffle_down => ShflOp::Down,
                        nir_intrinsic_shuffle_up => ShflOp::Up,
                        nir_intrinsic_shuffle_xor => ShflOp::Bfly,
                        _ => panic!("Unknown vote intrinsic"),
                    },
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_shared_atomic => {
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let data = self.get_src(&srcs[1]);
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: 0.into(),
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A32,
                    addr_offset: offset,
                    mem_space: MemSpace::Shared,
                    mem_order: MemOrder::Strong(MemScope::CTA),
                    mem_eviction_priority: MemEvictionPriority::Normal,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_shared_atomic_swap => {
                assert!(intrin.atomic_op() == nir_atomic_op_cmpxchg);
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let cmpr = self.get_src(&srcs[1]);
                let data = self.get_src(&srcs[2]);
                let atom_type = AtomType::U(bit_size);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: cmpr,
                    data: data,
                    atom_op: AtomOp::CmpExch,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A32,
                    addr_offset: offset,
                    mem_space: MemSpace::Shared,
                    mem_order: MemOrder::Strong(MemScope::CTA),
                    mem_eviction_priority: MemEvictionPriority::Normal,
                });
                self.set_dst(&intrin.def, dst);
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
                    order: MemOrder::Strong(MemScope::System),
                    eviction_priority: self
                        .get_eviction_priority(intrin.access()),
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 32);

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            nir_intrinsic_store_output => {
                let ShaderIoInfo::Fragment(_) = &mut self.info.io else {
                    panic!("load_input is only used for fragment shaders");
                };
                let data = self.get_src(&srcs[0]);

                let addr = u16::try_from(intrin.base()).unwrap()
                    + u16::try_from(srcs[1].as_uint().unwrap()).unwrap()
                    + 4 * u16::try_from(intrin.component()).unwrap();
                assert!(addr % 4 == 0);

                for c in 0..usize::from(intrin.num_components) {
                    let idx = usize::from(addr / 4) + usize::from(c);
                    self.fs_out_regs[idx] = data.as_ssa().unwrap()[c];
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
                    order: MemOrder::Strong(MemScope::CTA),
                    eviction_priority: MemEvictionPriority::Normal,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 24);

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            nir_intrinsic_store_shared => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Shared,
                    order: MemOrder::Strong(MemScope::CTA),
                    eviction_priority: MemEvictionPriority::Normal,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 24);
                let offset = offset + intrin.base();

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            nir_intrinsic_emit_vertex_nv | nir_intrinsic_end_primitive_nv => {
                assert!(intrin.def.bit_size() == 32);
                assert!(intrin.def.num_components() == 1);

                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let handle = self.get_src(&srcs[0]);
                let stream_id = intrin.stream_id();

                b.push_op(OpOut {
                    dst: dst.into(),
                    handle: handle,
                    stream: stream_id.into(),
                    out_type: if intrin.intrinsic
                        == nir_intrinsic_emit_vertex_nv
                    {
                        OutType::Emit
                    } else {
                        OutType::Cut
                    },
                });
                self.set_dst(&intrin.def, dst);
            }

            nir_intrinsic_final_primitive_nv => {
                let handle = self.get_src(&srcs[0]);

                if self.info.sm >= 70 {
                    b.push_op(OpOutFinal { handle: handle });
                }
            }
            nir_intrinsic_vote_all
            | nir_intrinsic_vote_any
            | nir_intrinsic_vote_ieq => {
                assert!(srcs[0].bit_size() == 1);
                let src = self.get_src(&srcs[0]);

                assert!(intrin.def.bit_size() == 1);
                let dst = b.alloc_ssa(RegFile::Pred, 1);

                b.push_op(OpVote {
                    op: match intrin.intrinsic {
                        nir_intrinsic_vote_all => VoteOp::All,
                        nir_intrinsic_vote_any => VoteOp::Any,
                        nir_intrinsic_vote_ieq => VoteOp::Eq,
                        _ => panic!("Unknown vote intrinsic"),
                    },
                    ballot: Dst::None,
                    vote: dst.into(),
                    pred: src,
                });
                self.set_dst(&intrin.def, dst);
            }
            _ => panic!(
                "Unsupported intrinsic instruction: {}",
                intrin.info().name()
            ),
        }
    }

    fn parse_load_const(
        &mut self,
        b: &mut impl SSABuilder,
        load_const: &nir_load_const_instr,
    ) {
        let mut dst_vec = Vec::new();
        for c in 0..load_const.def.num_components {
            if load_const.def.bit_size == 1 {
                let imm_b1 = unsafe { load_const.values()[c as usize].b };
                dst_vec.push(b.copy(imm_b1.into())[0]);
            } else if load_const.def.bit_size == 32 {
                let imm_u32 = unsafe { load_const.values()[c as usize].u32_ };
                dst_vec.push(b.copy(imm_u32.into())[0]);
            } else if load_const.def.bit_size == 64 {
                let imm_u64 = unsafe { load_const.values()[c as usize].u64_ };
                dst_vec.push(b.copy((imm_u64 as u32).into())[0]);
                dst_vec.push(b.copy(((imm_u64 >> 32) as u32).into())[0]);
            }
        }

        self.set_ssa(&load_const.def, dst_vec);
    }

    fn parse_undef(
        &mut self,
        b: &mut impl SSABuilder,
        undef: &nir_undef_instr,
    ) {
        let dst = alloc_ssa_for_nir(b, &undef.def);
        for c in &dst {
            b.push_op(OpUndef { dst: (*c).into() });
        }
        self.set_ssa(&undef.def, dst);
    }

    fn store_fs_outputs(&mut self, b: &mut impl SSABuilder) {
        let ShaderIoInfo::Fragment(info) = &mut self.info.io else {
            return;
        };

        for i in 0..32 {
            // Assume that colors have to come a vec4 at a time
            if !self.fs_out_regs[i].is_none() {
                info.writes_color |= 0xf << (i & !3)
            }
        }
        let mask_idx = (NAK_FS_OUT_SAMPLE_MASK / 4) as usize;
        info.writes_sample_mask = !self.fs_out_regs[mask_idx].is_none();
        let depth_idx = (NAK_FS_OUT_DEPTH / 4) as usize;
        info.writes_depth = !self.fs_out_regs[depth_idx].is_none();

        let mut srcs = Vec::new();
        for i in 0..32 {
            if info.writes_color & (1 << i) != 0 {
                if self.fs_out_regs[i].is_none() {
                    srcs.push(0.into());
                } else {
                    srcs.push(self.fs_out_regs[i].into());
                }
            }
        }

        // These always come together for some reason
        if info.writes_sample_mask || info.writes_depth {
            if info.writes_sample_mask {
                srcs.push(self.fs_out_regs[mask_idx].into());
            } else {
                srcs.push(0.into());
            }
            if info.writes_depth {
                // Saturate depth writes.
                //
                // TODO: This seems wrong in light of unrestricted depth but
                // it's needed to pass CTS tests for now.
                let depth = self.fs_out_regs[depth_idx];
                let sat_depth = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFAdd {
                    dst: sat_depth.into(),
                    srcs: [depth.into(), 0.into()],
                    saturate: true,
                    rnd_mode: FRndMode::NearestEven,
                });
                srcs.push(sat_depth.into());
            }
        }

        b.push_op(OpFSOut { srcs: srcs });
    }

    fn parse_block<'b>(
        &mut self,
        ssa_alloc: &mut SSAValueAllocator,
        phi_map: &mut PhiAllocMap<'b>,
        nb: &nir_block,
    ) {
        let mut b = SSAInstrBuilder::new(ssa_alloc);

        if nb.index == 0 && self.nir.info.shared_size > 0 {
            // The blob seems to always do a BSYNC before accessing shared
            // memory.  Perhaps this is to ensure that our allocation is
            // actually available and not in use by another thread?
            let label = self.label_alloc.alloc();
            let bar = self.bar_alloc.alloc();

            let bmov = b.push_op(OpBMov {
                dst: Dst::None,
                src: BMovSrc::Barrier(bar),
                clear: true,
            });
            bmov.deps.yld = true;

            let bssy = b.push_op(OpBSSy {
                bar: bar,
                cond: SrcRef::True.into(),
                target: label,
            });
            bssy.deps.yld = true;

            let bsync = b.push_op(OpBSync {
                bar: bar,
                cond: SrcRef::True.into(),
            });
            bsync.deps.yld = true;

            self.bar_alloc.free(bar);

            b.push_op(OpNop { label: Some(label) });
        }

        let mut phi = OpPhiDsts::new();
        for ni in nb.iter_instr_list() {
            if ni.type_ == nir_instr_type_phi {
                let np = ni.as_phi().unwrap();
                let dst = alloc_ssa_for_nir(&mut b, np.def.as_def());
                for (i, dst) in dst.iter().enumerate() {
                    let phi_id = phi_map.get_phi_id(np, i.try_into().unwrap());
                    phi.dsts.push(phi_id, (*dst).into());
                }
                self.set_ssa(np.def.as_def(), dst);
            } else {
                break;
            }
        }

        if !phi.dsts.is_empty() {
            b.push_op(phi);
        }

        for ni in nb.iter_instr_list() {
            match ni.type_ {
                nir_instr_type_alu => {
                    self.parse_alu(&mut b, ni.as_alu().unwrap())
                }
                nir_instr_type_jump => {
                    self.parse_jump(&mut b, ni.as_jump().unwrap())
                }
                nir_instr_type_tex => {
                    self.parse_tex(&mut b, ni.as_tex().unwrap())
                }
                nir_instr_type_intrinsic => {
                    self.parse_intrinsic(&mut b, ni.as_intrinsic().unwrap())
                }
                nir_instr_type_load_const => {
                    self.parse_load_const(&mut b, ni.as_load_const().unwrap())
                }
                nir_instr_type_undef => {
                    self.parse_undef(&mut b, ni.as_undef().unwrap())
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
                                phi_map.get_phi_id(np, i.try_into().unwrap());
                            phi.srcs.push(phi_id, (*src).into());
                        }
                        break;
                    }
                }
            }

            if !phi.srcs.is_empty() {
                b.push_op(phi);
            }
        }

        if let Some(ni) = nb.following_if() {
            /* The fall-through edge has to come first */
            self.cfg.add_edge(nb.index, ni.first_then_block().index);
            self.cfg.add_edge(nb.index, ni.first_else_block().index);

            let mut bra = Instr::new_boxed(OpBra {
                target: self.get_block_label(ni.first_else_block()),
            });

            let cond = self.get_ssa(&ni.condition.as_def())[0];
            bra.pred = cond.into();
            /* This is the branch to jump to the else */
            bra.pred.pred_inv = true;

            b.push_instr(bra);
        } else {
            assert!(succ[1].is_none());
            let s0 = succ[0].unwrap();
            if s0.index == self.end_block_id {
                self.store_fs_outputs(&mut b);
                b.push_op(OpExit {});
            } else {
                self.cfg.add_edge(nb.index, s0.index);
                b.push_op(OpBra {
                    target: self.get_block_label(s0),
                });
            }
        }

        let mut bb = BasicBlock::new(self.get_block_label(nb));
        bb.instrs.append(&mut b.as_vec());
        self.cfg.add_node(nb.index, bb);
    }

    fn parse_if<'b>(
        &mut self,
        ssa_alloc: &mut SSAValueAllocator,
        phi_map: &mut PhiAllocMap<'b>,
        ni: &nir_if,
    ) {
        self.parse_cf_list(ssa_alloc, phi_map, ni.iter_then_list());
        self.parse_cf_list(ssa_alloc, phi_map, ni.iter_else_list());
    }

    fn parse_loop<'b>(
        &mut self,
        ssa_alloc: &mut SSAValueAllocator,
        phi_map: &mut PhiAllocMap<'b>,
        nl: &nir_loop,
    ) {
        self.parse_cf_list(ssa_alloc, phi_map, nl.iter_body());
    }

    fn parse_cf_list<'b>(
        &mut self,
        ssa_alloc: &mut SSAValueAllocator,
        phi_map: &mut PhiAllocMap<'b>,
        list: ExecListIter<nir_cf_node>,
    ) {
        for node in list {
            match node.type_ {
                nir_cf_node_block => {
                    let nb = node.as_block().unwrap();
                    self.parse_block(ssa_alloc, phi_map, nb);
                }
                nir_cf_node_if => {
                    let ni = node.as_if().unwrap();
                    self.parse_if(ssa_alloc, phi_map, ni);
                }
                nir_cf_node_loop => {
                    let nl = node.as_loop().unwrap();
                    self.parse_loop(ssa_alloc, phi_map, nl);
                }
                _ => panic!("Invalid inner CF node type"),
            }
        }
    }

    pub fn parse_function_impl(&mut self, nfi: &nir_function_impl) -> Function {
        let mut ssa_alloc = SSAValueAllocator::new();
        self.end_block_id = nfi.end_block().index;

        let mut phi_alloc = PhiAllocator::new();
        let mut phi_map = PhiAllocMap::new(&mut phi_alloc);

        self.parse_cf_list(&mut ssa_alloc, &mut phi_map, nfi.iter_body());

        let cfg = std::mem::take(&mut self.cfg).as_cfg();
        assert!(cfg.len() > 0);
        for i in 0..cfg.len() {
            if cfg[i].falls_through() {
                assert!(cfg.succ_indices(i)[0] == i + 1);
            }
        }

        Function {
            ssa_alloc: ssa_alloc,
            phi_alloc: phi_alloc,
            blocks: cfg,
        }
    }

    pub fn parse_shader(mut self) -> Shader {
        if self.nir.info.stage() == MESA_SHADER_COMPUTE
            && self.nir.info.uses_control_barrier()
        {
            // We know OpBar uses a barrier but we don't know which one.  Assume
            // it implicitly uses B0 and reserve it so it doesn't stomp any
            // other barriers
            self.bar_alloc.reserve(0);
        }

        let mut functions = Vec::new();
        for nf in self.nir.iter_functions() {
            if let Some(nfi) = nf.get_impl() {
                let f = self.parse_function_impl(nfi);
                functions.push(f);
            }
        }

        self.info.num_barriers = self.bar_alloc.num_bars();

        // Tessellation evaluation shaders MUST claim to read gl_TessCoord or
        // the hardware will throw an SPH error.
        match &self.info.stage {
            ShaderStageInfo::Tessellation => match &mut self.info.io {
                ShaderIoInfo::Vtg(io) => {
                    let tc = NAK_ATTR_TESS_COORD;
                    io.mark_attrs_written(tc..(tc + 8));
                }
                _ => panic!("Tessellation must have ShaderIoInfo::Vtg"),
            },
            _ => (),
        }

        Shader {
            info: self.info,
            functions: functions,
        }
    }
}

pub fn nak_shader_from_nir(ns: &nir_shader, sm: u8) -> Shader {
    ShaderFromNir::new(ns, sm).parse_shader()
}
