/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::bitset::*;
use crate::nak_ir::*;

use std::ops::Range;

trait SrcMod {
    fn src_mod(&self) -> u8;
}

impl SrcMod for Src {
    fn src_mod(&self) -> u8 {
        0 /* TODO */
    }
}

enum ALUSrc {
    Imm(Immediate),
    Reg(RegRef),
    UReg(RegRef),
    CBuf(CBufRef),
}

impl ALUSrc {
    pub fn from_src(src: &Src, comps: u8) -> ALUSrc {
        match src {
            Src::Reg(reg) => {
                assert!(reg.comps() == comps);
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
        self.set_field(range, reg.base_idx());
    }

    fn set_src_mod(&mut self, range: Range<usize>, src_mod: u8) {
        assert!(range.len() == 2);
        self.set_field(range, src_mod);
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

    fn set_src_pred(&mut self, range: Range<usize>, pred: &Pred) {
        assert!(range.len() == 3);
        match pred {
            Pred::None => self.set_field(range, 0x7_u8),
            Pred::Reg(reg) => {
                self.set_reg(range, *reg);
            }
            Pred::SSA(_) => panic!("Cannot encode SSA value"),
        }
    }

    fn set_opcode(&mut self, opcode: u16) {
        self.set_field(0..12, opcode);
    }

    fn set_pred(&mut self, pred: &Pred, pred_inv: bool) {
        assert!(pred.is_none() || !pred_inv);
        self.set_src_pred(12..15, pred);
        self.set_bit(15, pred_inv);
    }

    fn set_dst_reg(&mut self, dst: RegRef) {
        self.set_reg(16..24, dst);
    }

    fn encode_alu(
        &mut self,
        opcode: u16,
        dst: Option<&Dst>,
        src0: Option<&Src>,
        src1: &Src,
        src2: Option<&Src>,
    ) {
        if let Some(dst) = dst {
            self.set_dst_reg(*dst.as_reg().unwrap());
        }

        if let Some(src0) = src0 {
            self.set_reg(24..32, *src0.as_reg().unwrap());
        }

        let form = match ALUSrc::from_src(src1, 1) {
            ALUSrc::Reg(reg1) => {
                if let Some(src2) = src2 {
                    match ALUSrc::from_src(src2, 1) {
                        ALUSrc::Reg(reg2) => {
                            self.set_reg(32..40, reg1);
                            self.set_src_mod(62..64, src1.src_mod());
                            self.set_reg(64..72, reg2);
                            self.set_src_mod(74..76, src2.src_mod());
                            1_u8 /* form */
                        }
                        ALUSrc::UReg(reg2) => {
                            self.set_ureg(32..40, reg2);
                            self.set_src_mod(62..64, src2.src_mod());
                            self.set_reg(64..72, reg1);
                            self.set_src_mod(74..76, src1.src_mod());
                            7_u8 /* form */
                        }
                        ALUSrc::Imm(imm) => {
                            self.set_src_imm(32..64, &imm);
                            self.set_reg(64..72, reg1);
                            self.set_src_mod(74..76, src1.src_mod());
                            2_u8 /* form */
                        }
                        ALUSrc::CBuf(cb) => {
                            /* TODO set_src_cx */
                            self.set_src_cb(38..59, &cb);
                            self.set_src_mod(62..64, src2.src_mod());
                            self.set_reg(64..72, reg1);
                            self.set_src_mod(74..76, src1.src_mod());
                            3_u8 /* form */
                        }
                        _ => panic!("Invalid instruction form"),
                    }
                } else {
                    self.set_reg(32..40, reg1);
                    self.set_src_mod(62..64, src1.src_mod());
                    1_u8 /* form */
                }
            }
            ALUSrc::UReg(reg1) => {
                self.set_ureg(32..40, reg1);
                self.set_src_mod(62..64, src1.src_mod());
                if let Some(src2) = src2 {
                    self.set_reg(64..72, *src2.as_reg().unwrap());
                    self.set_src_mod(74..76, src2.src_mod());
                }
                6_u8 /* form */
            }
            ALUSrc::Imm(imm) => {
                self.set_src_imm(32..64, &imm);
                if let Some(src2) = src2 {
                    self.set_reg(64..72, *src2.as_reg().unwrap());
                    self.set_src_mod(74..76, src2.src_mod());
                }
                4_u8 /* form */
            }
            ALUSrc::CBuf(cb) => {
                self.set_src_cb(38..59, &cb);
                self.set_src_mod(62..64, src1.src_mod());
                if let Some(src2) = src2 {
                    self.set_reg(64..72, *src2.as_reg().unwrap());
                    self.set_src_mod(74..76, src2.src_mod());
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

    fn encode_s2r(&mut self, instr: &Instr, idx: u8) {
        self.set_opcode(0x919);
        self.set_dst_reg(*instr.dst(0).as_reg().unwrap());
        self.set_field(72..80, idx);
    }

    fn encode_mov(&mut self, instr: &Instr) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 1);
        self.encode_alu(0x002, Some(instr.dst(0)), None, instr.src(0), None);
        self.set_field(72..76, 0xf_u32 /* TODO: Quad lanes */);
    }

    fn encode_sel(&mut self, instr: &Instr) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 3);
        self.encode_alu(
            0x007,
            Some(instr.dst(0)),
            Some(instr.src(1)),
            instr.src(2),
            None,
        );

        self.set_pred_reg(87..90, *instr.src(0).as_reg().unwrap());
        self.set_bit(90, false); /* not */
    }

    fn encode_iadd3(&mut self, instr: &Instr) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 3);
        self.encode_alu(
            0x010,
            Some(instr.dst(0)),
            Some(instr.src(0)),
            instr.src(1),
            Some(instr.src(2)),
        );

        self.set_field(81..84, 7_u32); /* pred */
        self.set_field(84..87, 7_u32); /* pred */
    }

    fn encode_lop3(&mut self, instr: &Instr, op: &LogicOp) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 3);
        self.encode_alu(
            0x012,
            Some(instr.dst(0)),
            Some(instr.src(0)),
            instr.src(1),
            Some(instr.src(2)),
        );

        self.set_field(72..80, op.lut);
        self.set_bit(80, false); /* .PAND */
        self.set_field(81..84, 7_u32); /* pred */
        self.set_field(84..87, 7_u32); /* pred */
        self.set_bit(90, true);
    }

    fn encode_plop3(&mut self, instr: &Instr, op: &LogicOp) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 3);

        self.set_opcode(0x81c);
        self.set_field(64..67, op.lut & 0x7);
        self.set_field(72..77, op.lut >> 3);

        self.set_pred_reg(68..71, *instr.src(2).as_reg().unwrap());
        self.set_bit(71, false); /* NOT(src2) */

        self.set_pred_reg(77..80, *instr.src(1).as_reg().unwrap());
        self.set_bit(80, false); /* NOT(src1) */
        self.set_pred_reg(81..84, *instr.dst(0).as_reg().unwrap());
        self.set_field(84..87, 7_u8); /* Def1 */

        self.set_pred_reg(87..90, *instr.src(0).as_reg().unwrap());
        self.set_bit(90, false); /* NOT(src0) */
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

    fn encode_isetp(&mut self, instr: &Instr, op: &IntCmpOp) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 2);
        self.encode_alu(0x00c, None, Some(instr.src(0)), instr.src(1), None);

        self.set_field(
            73..74,
            match op.cmp_type {
                IntCmpType::U32 => 0_u32,
                IntCmpType::I32 => 1_u32,
            },
        );
        self.set_field(74..76, 0_u32); /* pred combine op */
        self.set_cmp_op(76..79, &op.cmp_op);

        self.set_pred_reg(81..84, *instr.dst(0).as_reg().unwrap());
        self.set_field(84..87, 7_u32); /* dst1 */

        self.set_field(87..90, 7_u32); /* src pred */
        self.set_bit(90, false); /* src pred neg */
    }

    fn encode_shl(&mut self, instr: &Instr) {
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 2);
        self.encode_alu(
            0x019,
            Some(instr.dst(0)),
            Some(instr.src(0)),
            instr.src(1),
            None,
        );

        self.set_field(73..75, 3_u32 /* U32 */);
        self.set_bit(75, true /* W? */);
        self.set_bit(76, false /* Left */);
        self.set_bit(80, false /* HI */);
    }

    fn encode_ald(&mut self, instr: &Instr, attr: &AttrAccess) {
        self.set_opcode(0x321);
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 2);

        self.set_dst_reg(*instr.dst(0).as_reg().unwrap());
        self.set_reg(24..32, *instr.src(0).as_reg().unwrap());
        self.set_reg(32..40, *instr.src(1).as_reg().unwrap());

        self.set_field(40..50, attr.addr);
        self.set_field(74..76, attr.comps - 1);
        self.set_field(76..77, attr.patch);
        self.set_field(77..78, attr.flags);
        self.set_field(79..80, attr.out_load);
    }

    fn encode_ast(&mut self, instr: &Instr, attr: &AttrAccess) {
        self.set_opcode(0x322);
        assert!(instr.num_dsts() == 0);
        assert!(instr.num_srcs() == 3);

        self.set_reg(32..40, *instr.src(0).as_reg().unwrap());
        self.set_reg(24..32, *instr.src(1).as_reg().unwrap());
        self.set_reg(64..72, *instr.src(2).as_reg().unwrap());

        self.set_field(40..50, attr.addr);
        self.set_field(74..76, attr.comps - 1);
        self.set_field(76..77, attr.patch);
        self.set_field(77..78, attr.flags);
        assert!(!attr.out_load);
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

    fn encode_ld(&mut self, instr: &Instr, access: &MemAccess) {
        self.set_opcode(0x980);
        assert!(instr.num_dsts() == 1);
        assert!(instr.num_srcs() == 1);

        self.set_dst_reg(*instr.dst(0).as_reg().unwrap());
        self.set_reg(24..32, *instr.src(0).as_reg().unwrap());
        self.set_field(32..64, 0_u32 /* Immediate offset */);

        self.set_mem_access(access);
    }

    fn encode_st(&mut self, instr: &Instr, access: &MemAccess) {
        self.set_opcode(0x385);
        assert!(instr.num_dsts() == 0);
        assert!(instr.num_srcs() == 2);

        self.set_reg(24..32, *instr.src(0).as_reg().unwrap());
        self.set_field(32..64, 0_u32 /* Immediate offset */);
        self.set_reg(64..72, *instr.src(1).as_reg().unwrap());

        self.set_mem_access(access);
    }

    fn encode_exit(&mut self, instr: &Instr) {
        self.set_opcode(0x94d);
        assert!(instr.num_dsts() == 0);
        assert!(instr.num_srcs() == 0);

        /* ./.KEEPREFCOUNT/.PREEMPTED/.INVALID3 */
        self.set_field(84..85, false);
        self.set_field(85..86, false); /* .NO_ATEXIT */
        self.set_field(87..90, 0x7_u8); /* TODO: Predicate */
        self.set_field(90..91, false); /* NOT */
    }

    pub fn encode(instr: &Instr, sm: u8) -> [u32; 4] {
        assert!(sm >= 75);

        let mut si = SM75Instr {
            inst: [0; 4],
            sm: sm,
        };

        match &instr.op {
            Opcode::S2R(i) => si.encode_s2r(instr, *i),
            Opcode::MOV => si.encode_mov(instr),
            Opcode::SEL => si.encode_sel(instr),
            Opcode::IADD3 => si.encode_iadd3(instr),
            Opcode::LOP3(op) => si.encode_lop3(instr, &op),
            Opcode::PLOP3(op) => si.encode_plop3(instr, &op),
            Opcode::ISETP(op) => si.encode_isetp(instr, &op),
            Opcode::SHL => si.encode_shl(instr),
            Opcode::ALD(a) => si.encode_ald(instr, &a),
            Opcode::AST(a) => si.encode_ast(instr, &a),
            Opcode::LD(a) => si.encode_ld(instr, a),
            Opcode::ST(a) => si.encode_st(instr, a),
            Opcode::EXIT => si.encode_exit(instr),
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
