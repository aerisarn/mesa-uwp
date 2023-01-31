/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::bitset::*;
use crate::nak_ir::*;

use std::ops::Range;

enum ALUSrc {
    Imm(Immediate),
    Reg(RegRef),
    UReg(RegRef),
    CBuf(CBufRef),
}

impl ALUSrc {
    pub fn from_src(src: &Src, uniform: bool) -> ALUSrc {
        match src {
            Src::Zero => {
                if uniform {
                    ALUSrc::UReg(RegRef::zero(RegFile::UGPR, 1))
                } else {
                    ALUSrc::Reg(RegRef::zero(RegFile::GPR, 1))
                }
            }
            Src::Reg(reg) => {
                assert!(reg.comps() == 1);
                assert!(!uniform || reg.file() == RegFile::UGPR);
                match reg.file() {
                    RegFile::GPR => ALUSrc::Reg(*reg),
                    RegFile::UGPR => ALUSrc::UReg(*reg),
                    _ => panic!("Invalid ALU register file"),
                }
            }
            Src::Imm(i) => ALUSrc::Imm(*i),
            Src::CBuf(cb) => ALUSrc::CBuf(*cb),
            _ => panic!("Invalid ALU source"),
        }
    }
}

struct SM75Instr {
    inst: [u32; 4],
    sm: u8,
}

impl BitSetViewable for SM75Instr {
    fn bits(&self) -> usize {
        BitSetView::new(&self.inst).bits()
    }

    fn get_bit_range_u64(&self, range: Range<usize>) -> u64 {
        BitSetView::new(&self.inst).get_bit_range_u64(range)
    }
}

impl BitSet for SM75Instr {}

impl BitSetMutViewable for SM75Instr {
    fn set_bit_range_u64(&mut self, range: Range<usize>, val: u64) {
        BitSetMutView::new(&mut self.inst).set_bit_range_u64(range, val);
    }
}

impl BitSetMut for SM75Instr {}

impl SM75Instr {
    fn set_src_imm(&mut self, range: Range<usize>, imm: &Immediate) {
        assert!(range.len() == 32);
        self.set_field(range, imm.u);
    }

    fn set_reg(&mut self, range: Range<usize>, reg: RegRef) {
        assert!(range.len() == 8);
        assert!(reg.file() == RegFile::GPR);
        self.set_field(range, reg.base_idx());
    }

    fn set_ureg(&mut self, range: Range<usize>, reg: RegRef) {
        assert!(range.len() == 8);
        assert!(reg.file() == RegFile::UGPR);
        assert!(reg.base_idx() <= 63);
        self.set_field(range, reg.base_idx());
    }

    fn set_pred_reg(&mut self, range: Range<usize>, reg: RegRef) {
        assert!(range.len() == 3);
        assert!(reg.file() == RegFile::Pred);
        assert!(reg.base_idx() <= 7);
        assert!(reg.comps() == 1);
        self.set_field(range, reg.base_idx());
    }

    fn set_reg_src(&mut self, range: Range<usize>, src: Src) {
        match src {
            Src::Zero => self.set_reg(range, RegRef::zero(RegFile::GPR, 1)),
            Src::Reg(reg) => self.set_reg(range, reg),
            _ => panic!("Not a register"),
        }
    }

    fn set_pred_dst(&mut self, range: Range<usize>, dst: Dst) {
        self.set_pred_reg(range, *dst.as_reg().unwrap());
    }

    fn set_pred_src(&mut self, range: Range<usize>, src: Src) {
        match src {
            Src::Zero => {
                self.set_pred_reg(range, RegRef::zero(RegFile::Pred, 1));
            }
            Src::Reg(reg) => self.set_pred_reg(range, reg),
            _ => panic!("Not a register"),
        }
    }

    fn set_src_cb(&mut self, range: Range<usize>, cb: &CBufRef) {
        let mut v = self.subset_mut(range);
        v.set_field(0..16, cb.offset);
        if let CBuf::Binding(idx) = cb.buf {
            v.set_field(16..21, idx);
        } else {
            panic!("Must be a bound constant buffer");
        }
    }

    fn set_src_cx(&mut self, range: Range<usize>, cb: &CBufRef) {
        let mut v = self.subset_mut(range);
        if let CBuf::BindlessGPR(reg) = cb.buf {
            assert!(reg.base_idx() <= 63);
            assert!(reg.file() == RegFile::UGPR);
            v.set_field(0..8, reg.base_idx());
        } else {
            panic!("Must be a bound constant buffer");
        }
        assert!(cb.offset % 4 == 0);
        v.set_field(8..22, cb.offset / 4);
    }

    fn set_opcode(&mut self, opcode: u16) {
        self.set_field(0..12, opcode);
    }

    fn set_pred(&mut self, pred: &Pred, pred_inv: bool) {
        assert!(pred.is_none() || !pred_inv);
        self.set_pred_reg(
            12..15,
            match pred {
                Pred::None => RegRef::zero(RegFile::Pred, 1),
                Pred::Reg(reg) => *reg,
                Pred::SSA(_) => panic!("SSA values must be lowered"),
            },
        );
        self.set_bit(15, pred_inv);
    }

    fn set_dst(&mut self, dst: Dst) {
        self.set_reg(16..24, *dst.as_reg().unwrap());
    }

    fn encode_alu(
        &mut self,
        opcode: u16,
        dst: Option<Dst>,
        src0: Option<ModSrc>,
        src1: ModSrc,
        src2: Option<ModSrc>,
    ) {
        if let Some(dst) = dst {
            self.set_dst(dst);
        }

        if let Some(src0) = src0 {
            self.set_reg_src(24..32, src0.src);
            self.set_bit(72, src0.src_mod.has_neg());
            self.set_bit(73, src0.src_mod.has_abs());
        }

        let form = match ALUSrc::from_src(&src1.src, false) {
            ALUSrc::Reg(reg1) => {
                if let Some(src2) = src2 {
                    match ALUSrc::from_src(&src2.src, false) {
                        ALUSrc::Reg(reg2) => {
                            self.set_reg(32..40, reg1);
                            self.set_bit(62, src1.src_mod.has_abs());
                            self.set_bit(63, src1.src_mod.has_neg());
                            self.set_reg(64..72, reg2);
                            self.set_bit(74, src2.src_mod.has_abs());
                            self.set_bit(75, src2.src_mod.has_neg());
                            1_u8 /* form */
                        }
                        ALUSrc::UReg(reg2) => {
                            self.set_ureg(32..40, reg2);
                            self.set_bit(62, src2.src_mod.has_abs());
                            self.set_bit(63, src2.src_mod.has_neg());
                            self.set_reg(64..72, reg1);
                            self.set_bit(74, src1.src_mod.has_abs());
                            self.set_bit(75, src1.src_mod.has_neg());
                            7_u8 /* form */
                        }
                        ALUSrc::Imm(imm) => {
                            self.set_src_imm(32..64, &imm);
                            self.set_reg(64..72, reg1);
                            self.set_bit(74, src1.src_mod.has_abs());
                            self.set_bit(75, src1.src_mod.has_neg());
                            2_u8 /* form */
                        }
                        ALUSrc::CBuf(cb) => {
                            /* TODO set_src_cx */
                            self.set_src_cb(38..59, &cb);
                            self.set_bit(62, src2.src_mod.has_abs());
                            self.set_bit(63, src2.src_mod.has_neg());
                            self.set_reg(64..72, reg1);
                            self.set_bit(74, src1.src_mod.has_abs());
                            self.set_bit(75, src1.src_mod.has_neg());
                            3_u8 /* form */
                        }
                        _ => panic!("Invalid instruction form"),
                    }
                } else {
                    self.set_reg(32..40, reg1);
                    self.set_bit(62, src1.src_mod.has_abs());
                    self.set_bit(63, src1.src_mod.has_neg());
                    1_u8 /* form */
                }
            }
            ALUSrc::UReg(reg1) => {
                self.set_ureg(32..40, reg1);
                self.set_bit(62, src1.src_mod.has_abs());
                self.set_bit(63, src1.src_mod.has_neg());
                if let Some(src2) = src2 {
                    self.set_reg_src(64..72, src2.src);
                    self.set_bit(74, src2.src_mod.has_abs());
                    self.set_bit(75, src2.src_mod.has_neg());
                }
                6_u8 /* form */
            }
            ALUSrc::Imm(imm) => {
                self.set_src_imm(32..64, &imm);
                if let Some(src2) = src2 {
                    self.set_reg_src(64..72, src2.src);
                    self.set_bit(74, src2.src_mod.has_abs());
                    self.set_bit(75, src2.src_mod.has_neg());
                }
                4_u8 /* form */
            }
            ALUSrc::CBuf(cb) => {
                self.set_src_cb(38..59, &cb);
                self.set_bit(62, src1.src_mod.has_abs());
                self.set_bit(63, src1.src_mod.has_neg());
                if let Some(src2) = src2 {
                    self.set_reg_src(64..72, src2.src);
                    self.set_bit(74, src2.src_mod.has_abs());
                    self.set_bit(75, src2.src_mod.has_neg());
                }
                5_u8 /* form */
            }
            _ => panic!("Invalid instruction form"),
        };

        self.set_field(0..9, opcode);
        self.set_field(9..12, form);
    }

    fn set_instr_deps(&mut self, deps: &InstrDeps) {
        self.set_field(105..109, deps.delay);
        self.set_bit(109, deps.yld);
        self.set_field(110..113, deps.wr_bar().unwrap_or(7));
        self.set_field(113..116, deps.rd_bar().unwrap_or(7));
        self.set_field(116..122, deps.wt_bar_mask);
        self.set_field(122..126, deps.reuse_mask);
    }

    fn encode_iadd3(&mut self, op: &OpIAdd3) {
        self.encode_alu(
            0x010,
            Some(op.dst),
            Some(op.mod_src(0)),
            op.mod_src(1),
            Some(op.mod_src(2)),
        );

        self.set_pred_src(81..84, op.carry[0]);
        self.set_pred_src(84..87, op.carry[1]);
    }

    fn set_cmp_op(&mut self, range: Range<usize>, op: &CmpOp) {
        assert!(range.len() == 3);
        self.set_field(
            range,
            match op {
                CmpOp::Eq => 2_u8,
                CmpOp::Ne => 5_u8,
                CmpOp::Lt => 1_u8,
                CmpOp::Le => 3_u8,
                CmpOp::Gt => 4_u8,
                CmpOp::Ge => 6_u8,
            },
        );
    }

    fn encode_isetp(&mut self, op: &OpISetP) {
        self.encode_alu(
            0x00c,
            None,
            Some(op.srcs[0].into()),
            op.srcs[1].into(),
            None,
        );

        self.set_field(
            73..74,
            match op.cmp_type {
                IntCmpType::U32 => 0_u32,
                IntCmpType::I32 => 1_u32,
            },
        );
        self.set_field(74..76, 0_u32); /* pred combine op */
        self.set_cmp_op(76..79, &op.cmp_op);

        self.set_pred_dst(81..84, op.dst);
        self.set_field(84..87, 7_u32); /* dst1 */

        self.set_field(87..90, 7_u32); /* src pred */
        self.set_bit(90, false); /* src pred neg */
    }

    fn encode_lop3(&mut self, op: &OpLop3) {
        self.encode_alu(
            0x012,
            Some(op.dst),
            Some(op.srcs[0].into()),
            op.srcs[1].into(),
            Some(op.srcs[2].into()),
        );

        self.set_field(72..80, op.op.lut);
        self.set_bit(80, false); /* .PAND */
        self.set_field(81..84, 7_u32); /* pred */
        self.set_field(84..87, 7_u32); /* pred */
        self.set_bit(90, true);
    }

    fn encode_shl(&mut self, op: &OpShl) {
        self.encode_alu(
            0x019,
            Some(op.dst),
            Some(op.srcs[0].into()),
            op.srcs[1].into(),
            None,
        );

        self.set_field(73..75, 3_u32 /* U32 */);
        self.set_bit(75, true /* W? */);
        self.set_bit(76, false /* Left */);
        self.set_bit(80, false /* HI */);
    }

    fn encode_mov(&mut self, op: &OpMov) {
        self.encode_alu(0x002, Some(op.dst), None, op.src.into(), None);
        self.set_field(72..76, op.quad_lanes);
    }

    fn encode_sel(&mut self, op: &OpSel) {
        self.encode_alu(
            0x007,
            Some(op.dst),
            Some(op.srcs[0].into()),
            op.srcs[1].into(),
            None,
        );

        self.set_pred_src(87..90, op.cond);
        self.set_bit(90, op.cond_mod.has_not());
    }

    fn encode_plop3(&mut self, op: &OpPLop3) {
        self.set_opcode(0x81c);
        self.set_field(64..67, op.op.lut & 0x7);
        self.set_field(72..77, op.op.lut >> 3);

        self.set_pred_src(68..71, op.srcs[2]);
        self.set_bit(71, op.src_mods[2].has_not());

        self.set_pred_src(77..80, op.srcs[1]);
        self.set_bit(80, op.src_mods[1].has_not());
        self.set_pred_dst(81..84, op.dst);
        self.set_field(84..87, 7_u8); /* Def1 */

        self.set_pred_src(87..90, op.srcs[0]);
        self.set_bit(90, op.src_mods[0].has_not());
    }

    fn set_mem_access(&mut self, access: &MemAccess) {
        self.set_field(
            72..73,
            match access.addr_type {
                MemAddrType::A32 => 0_u8,
                MemAddrType::A64 => 1_u8,
            },
        );
        self.set_field(
            73..76,
            match access.mem_type {
                MemType::U8 => 0_u8,
                MemType::I8 => 1_u8,
                MemType::U16 => 2_u8,
                MemType::I16 => 3_u8,
                MemType::B32 => 4_u8,
                MemType::B64 => 5_u8,
                MemType::B128 => 6_u8,
            },
        );
        if self.sm <= 75 {
            self.set_field(
                77..79,
                match access.scope {
                    MemScope::CTA => 0_u8,
                    MemScope::Cluster => 1_u8,
                    MemScope::GPU => 2_u8,
                    MemScope::System => 3_u8,
                },
            );
            self.set_field(
                79..81,
                match access.order {
                    /* Constant => 0_u8, */
                    /* Weak? => 1_u8, */
                    MemOrder::Strong => 2_u8,
                    /* MMIO => 3_u8, */
                },
            );
        } else {
            assert!(access.scope == MemScope::System);
            assert!(access.order == MemOrder::Strong);
            self.set_field(77..81, 0xa);
        }
    }

    fn encode_ld(&mut self, op: &OpLd) {
        self.set_opcode(0x980);

        self.set_dst(op.dst);
        self.set_reg_src(24..32, op.addr);
        self.set_field(32..64, op.offset);

        self.set_mem_access(&op.access);
    }

    fn encode_st(&mut self, op: &OpSt) {
        self.set_opcode(0x385);

        self.set_reg_src(24..32, op.addr);
        self.set_field(32..64, op.offset);
        self.set_reg_src(64..72, op.data);

        self.set_mem_access(&op.access);
    }

    fn encode_ald(&mut self, op: &OpALd) {
        self.set_opcode(0x321);

        self.set_dst(op.dst);
        self.set_reg_src(24..32, op.vtx);
        self.set_reg_src(32..40, op.offset);

        self.set_field(40..50, op.access.addr);
        self.set_field(74..76, op.access.comps - 1);
        self.set_field(76..77, op.access.patch);
        self.set_field(77..78, op.access.flags);
        self.set_field(79..80, op.access.out_load);
    }

    fn encode_ast(&mut self, op: &OpASt) {
        self.set_opcode(0x322);

        self.set_reg_src(32..40, op.data);
        self.set_reg_src(24..32, op.vtx);
        self.set_reg_src(64..72, op.offset);

        self.set_field(40..50, op.access.addr);
        self.set_field(74..76, op.access.comps - 1);
        self.set_field(76..77, op.access.patch);
        self.set_field(77..78, op.access.flags);
        assert!(!op.access.out_load);
    }

    fn encode_exit(&mut self, op: &OpExit) {
        self.set_opcode(0x94d);

        /* ./.KEEPREFCOUNT/.PREEMPTED/.INVALID3 */
        self.set_field(84..85, false);
        self.set_field(85..86, false); /* .NO_ATEXIT */
        self.set_field(87..90, 0x7_u8); /* TODO: Predicate */
        self.set_field(90..91, false); /* NOT */
    }

    fn encode_s2r(&mut self, op: &OpS2R) {
        self.set_opcode(0x919);
        self.set_dst(op.dst);
        self.set_field(72..80, op.idx);
    }

    pub fn encode(instr: &Instr, sm: u8) -> [u32; 4] {
        assert!(sm >= 75);

        let mut si = SM75Instr {
            inst: [0; 4],
            sm: sm,
        };

        match &instr.op {
            Op::IAdd3(op) => si.encode_iadd3(&op),
            Op::ISetP(op) => si.encode_isetp(&op),
            Op::Lop3(op) => si.encode_lop3(&op),
            Op::Shl(op) => si.encode_shl(&op),
            Op::Mov(op) => si.encode_mov(&op),
            Op::Sel(op) => si.encode_sel(&op),
            Op::PLop3(op) => si.encode_plop3(&op),
            Op::Ld(op) => si.encode_ld(&op),
            Op::St(op) => si.encode_st(&op),
            Op::ALd(op) => si.encode_ald(&op),
            Op::ASt(op) => si.encode_ast(&op),
            Op::Exit(op) => si.encode_exit(&op),
            Op::S2R(op) => si.encode_s2r(&op),
            _ => panic!("Unhandled instruction"),
        }

        si.set_pred(&instr.pred, instr.pred_inv);
        si.set_instr_deps(&instr.deps);

        si.inst
    }
}

pub fn encode_shader(shader: &Shader) -> Vec<u32> {
    let mut encoded = Vec::new();
    assert!(shader.functions.len() == 1);
    let func = &shader.functions[0];
    for b in &func.blocks {
        for instr in &b.instrs {
            let e = SM75Instr::encode(instr, shader.sm);
            encoded.extend_from_slice(&e[..]);
        }
    }
    encoded
}
