/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

pub trait Builder {
    fn push_instr(&mut self, instr: Box<Instr>) -> &mut Instr;

    fn push_op(&mut self, op: impl Into<Op>) -> &mut Instr {
        self.push_instr(Instr::new_boxed(op))
    }

    fn predicate<'a>(&'a mut self, pred: Pred) -> PredicatedBuilder<'a, Self>
    where
        Self: Sized,
    {
        PredicatedBuilder {
            b: self,
            pred: pred,
        }
    }

    fn lop2_to(&mut self, dst: Dst, op: LogicOp, x: Src, y: Src) {
        /* Only uses x and y */
        assert!(!op.src_used(2));

        let is_predicate = match dst {
            Dst::None => panic!("No LOP destination"),
            Dst::SSA(ssa) => ssa.is_predicate(),
            Dst::Reg(reg) => reg.is_predicate(),
        };
        assert!(x.is_predicate() == is_predicate);
        assert!(y.is_predicate() == is_predicate);

        if is_predicate {
            self.push_op(OpPLop3 {
                dsts: [dst.into(), Dst::None],
                srcs: [x, y, Src::new_imm_bool(true)],
                ops: [op, LogicOp::new_const(false)],
            });
        } else {
            self.push_op(OpLop3 {
                dst: dst.into(),
                srcs: [x, y, Src::new_zero()],
                op: op,
            });
        }
    }

    fn prmt_to(&mut self, dst: Dst, x: Src, y: Src, sel: [u8; 4]) {
        if sel == [0, 1, 2, 3] {
            self.copy_to(dst, x);
        } else if sel == [4, 5, 6, 7] {
            self.copy_to(dst, y);
        } else {
            let mut sel_u32 = 0;
            for i in 0..4 {
                assert!(sel[i] < 16);
                sel_u32 |= u32::from(sel[i]) << (i * 4);
            }

            self.push_op(OpPrmt {
                dst: dst,
                srcs: [x, y],
                sel: sel_u32.into(),
                mode: PrmtMode::Index,
            });
        }
    }

    fn copy_to(&mut self, dst: Dst, src: Src) {
        self.push_op(OpCopy { dst: dst, src: src });
    }

    fn swap(&mut self, x: RegRef, y: RegRef) {
        assert!(x.file() == y.file());
        self.push_op(OpSwap {
            dsts: [x.into(), y.into()],
            srcs: [y.into(), x.into()],
        });
    }
}

pub trait SSABuilder: Builder {
    fn alloc_ssa(&mut self, file: RegFile, comps: u8) -> SSARef;

    fn fadd(&mut self, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpFAdd {
            dst: dst.into(),
            srcs: [x, y],
            saturate: false,
            rnd_mode: FRndMode::NearestEven,
        });
        dst
    }

    fn fmnmx(&mut self, x: Src, y: Src, min: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpFMnMx {
            dst: dst.into(),
            srcs: [x, y],
            min: min,
        });
        dst
    }

    fn fmul(&mut self, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpFMul {
            dst: dst.into(),
            srcs: [x, y],
            saturate: false,
            rnd_mode: FRndMode::NearestEven,
        });
        dst
    }

    fn fset(&mut self, cmp_op: FloatCmpOp, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpFSet {
            dst: dst.into(),
            cmp_op: cmp_op,
            srcs: [x, y],
        });
        dst
    }

    fn fsetp(&mut self, cmp_op: FloatCmpOp, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::Pred, 1);
        self.push_op(OpFSetP {
            dst: dst.into(),
            set_op: PredSetOp::And,
            cmp_op: cmp_op,
            srcs: [x, y],
            accum: SrcRef::True.into(),
        });
        dst
    }

    fn iabs(&mut self, i: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpIAbs {
            dst: dst.into(),
            src: i,
        });
        dst
    }

    fn iadd(&mut self, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpIAdd3 {
            dst: dst.into(),
            srcs: [Src::new_zero(), x, y],
            overflow: [Dst::None; 2],
        });
        dst
    }

    fn imnmx(&mut self, tp: IntCmpType, x: Src, y: Src, min: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpIMnMx {
            dst: dst.into(),
            cmp_type: tp,
            srcs: [x, y],
            min: min,
        });
        dst
    }

    fn imul(&mut self, x: Src, y: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpIMad {
            dst: dst.into(),
            srcs: [x, y, 0.into()],
            signed: false,
        });
        dst
    }

    fn ineg(&mut self, i: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpINeg {
            dst: dst.into(),
            src: i,
        });
        dst
    }

    fn isetp(
        &mut self,
        cmp_type: IntCmpType,
        cmp_op: IntCmpOp,
        x: Src,
        y: Src,
    ) -> SSARef {
        let dst = self.alloc_ssa(RegFile::Pred, 1);
        self.push_op(OpISetP {
            dst: dst.into(),
            set_op: PredSetOp::And,
            cmp_op: cmp_op,
            cmp_type: cmp_type,
            ex: false,
            srcs: [x, y],
            accum: true.into(),
            low_cmp: true.into(),
        });
        dst
    }

    fn isetp64(
        &mut self,
        cmp_type: IntCmpType,
        cmp_op: IntCmpOp,
        x: Src,
        y: Src,
    ) -> SSARef {
        let x = x.as_ssa().unwrap();
        let y = y.as_ssa().unwrap();

        // Low bits are always an unsigned comparison
        let low = self.isetp(IntCmpType::U32, cmp_op, x[0].into(), y[0].into());

        let dst = self.alloc_ssa(RegFile::Pred, 1);
        match cmp_op {
            IntCmpOp::Eq | IntCmpOp::Ne => {
                self.push_op(OpISetP {
                    dst: dst.into(),
                    set_op: match cmp_op {
                        IntCmpOp::Eq => PredSetOp::And,
                        IntCmpOp::Ne => PredSetOp::Or,
                        _ => panic!("Not an integer equality"),
                    },
                    cmp_op: cmp_op,
                    cmp_type: IntCmpType::U32,
                    ex: false,
                    srcs: [x[1].into(), y[1].into()],
                    accum: low.into(),
                    low_cmp: true.into(),
                });
            }
            IntCmpOp::Ge | IntCmpOp::Gt | IntCmpOp::Le | IntCmpOp::Lt => {
                self.push_op(OpISetP {
                    dst: dst.into(),
                    set_op: PredSetOp::And,
                    cmp_op: cmp_op,
                    cmp_type: cmp_type,
                    ex: true,
                    srcs: [x[1].into(), y[1].into()],
                    accum: true.into(),
                    low_cmp: low.into(),
                });
            }
        }
        dst
    }

    fn lop2(&mut self, op: LogicOp, x: Src, y: Src) -> SSARef {
        let dst = if x.is_predicate() {
            self.alloc_ssa(RegFile::Pred, 1)
        } else {
            self.alloc_ssa(RegFile::GPR, 1)
        };
        self.lop2_to(dst.into(), op, x, y);
        dst
    }

    fn mufu(&mut self, op: MuFuOp, src: Src) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpMuFu {
            dst: dst.into(),
            op: op,
            src: src,
        });
        dst
    }

    fn prmt(&mut self, x: Src, y: Src, sel: [u8; 4]) -> SSARef {
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.prmt_to(dst.into(), x, y, sel);
        dst
    }

    fn prmt4(&mut self, src: [Src; 4], sel: [u8; 4]) -> SSARef {
        let max_sel = *sel.iter().max().unwrap();
        if max_sel < 8 {
            self.prmt(src[0], src[1], sel)
        } else if max_sel < 12 {
            let mut sel_a = [0_u8; 4];
            let mut sel_b = [0_u8; 4];
            for i in 0..4_u8 {
                if sel[usize::from(i)] < 8 {
                    sel_a[usize::from(i)] = sel[usize::from(i)];
                    sel_b[usize::from(i)] = i;
                } else {
                    sel_b[usize::from(i)] = (sel[usize::from(i)] - 8) + 4;
                }
            }
            let a = self.prmt(src[0], src[1], sel_a);
            self.prmt(a.into(), src[2], sel_b)
        } else if max_sel < 16 {
            let mut sel_a = [0_u8; 4];
            let mut sel_b = [0_u8; 4];
            let mut sel_c = [0_u8; 4];
            for i in 0..4_u8 {
                if sel[usize::from(i)] < 8 {
                    sel_a[usize::from(i)] = sel[usize::from(i)];
                    sel_c[usize::from(i)] = i;
                } else {
                    sel_b[usize::from(i)] = sel[usize::from(i)] - 8;
                    sel_c[usize::from(i)] = 4 + i;
                }
            }
            let a = self.prmt(src[0], src[1], sel_a);
            let b = self.prmt(src[2], src[3], sel_b);
            self.prmt(a.into(), b.into(), sel_c)
        } else {
            panic!("Invalid permute value: {max_sel}");
        }
    }

    fn sel(&mut self, cond: Src, x: Src, y: Src) -> SSARef {
        assert!(cond.src_ref.is_predicate());
        assert!(x.is_predicate() == y.is_predicate());
        if x.is_predicate() {
            let dst = self.alloc_ssa(RegFile::Pred, 1);
            self.push_op(OpPLop3 {
                dsts: [dst.into(), Dst::None],
                srcs: [cond, x, y],
                ops: [
                    LogicOp::new_lut(&|c, x, y| (c & x) | (!c & y)),
                    LogicOp::new_const(false),
                ],
            });
            dst
        } else {
            let dst = self.alloc_ssa(RegFile::GPR, 1);
            self.push_op(OpSel {
                dst: dst.into(),
                cond: cond,
                srcs: [x, y],
            });
            dst
        }
    }

    fn copy(&mut self, src: Src) -> SSARef {
        let dst = if src.is_predicate() {
            self.alloc_ssa(RegFile::Pred, 1)
        } else {
            self.alloc_ssa(RegFile::GPR, 1)
        };
        self.copy_to(dst.into(), src);
        dst
    }

    fn bmov_to_bar(&mut self, src: Src) -> SSARef {
        assert!(src.src_ref.as_ssa().unwrap().file() == RegFile::GPR);
        let dst = self.alloc_ssa(RegFile::Bar, 1);
        self.push_op(OpBMov {
            dst: dst.into(),
            src: src,
            clear: false,
        });
        dst
    }

    fn bmov_to_gpr(&mut self, src: Src) -> SSARef {
        assert!(src.src_ref.as_ssa().unwrap().file() == RegFile::Bar);
        let dst = self.alloc_ssa(RegFile::GPR, 1);
        self.push_op(OpBMov {
            dst: dst.into(),
            src: src,
            clear: false,
        });
        dst
    }
}

pub struct InstrBuilder {
    instrs: MappedInstrs,
}

impl InstrBuilder {
    pub fn new() -> Self {
        Self {
            instrs: MappedInstrs::None,
        }
    }

    pub fn as_vec(self) -> Vec<Box<Instr>> {
        match self.instrs {
            MappedInstrs::None => Vec::new(),
            MappedInstrs::One(i) => vec![i],
            MappedInstrs::Many(v) => v,
        }
    }

    pub fn as_mapped_instrs(self) -> MappedInstrs {
        self.instrs
    }
}

impl Builder for InstrBuilder {
    fn push_instr(&mut self, instr: Box<Instr>) -> &mut Instr {
        self.instrs.push(instr);
        self.instrs.last_mut().unwrap().as_mut()
    }
}

pub struct SSAInstrBuilder<'a> {
    b: InstrBuilder,
    alloc: &'a mut SSAValueAllocator,
}

impl<'a> SSAInstrBuilder<'a> {
    pub fn new(alloc: &'a mut SSAValueAllocator) -> Self {
        Self {
            b: InstrBuilder::new(),
            alloc: alloc,
        }
    }

    pub fn as_vec(self) -> Vec<Box<Instr>> {
        self.b.as_vec()
    }

    #[allow(dead_code)]
    pub fn as_mapped_instrs(self) -> MappedInstrs {
        self.b.as_mapped_instrs()
    }
}

impl<'a> Builder for SSAInstrBuilder<'a> {
    fn push_instr(&mut self, instr: Box<Instr>) -> &mut Instr {
        self.b.push_instr(instr)
    }
}

impl<'a> SSABuilder for SSAInstrBuilder<'a> {
    fn alloc_ssa(&mut self, file: RegFile, comps: u8) -> SSARef {
        self.alloc.alloc_vec(file, comps)
    }
}

pub struct PredicatedBuilder<'a, T: Builder> {
    b: &'a mut T,
    pred: Pred,
}

impl<'a, T: Builder> Builder for PredicatedBuilder<'a, T> {
    fn push_instr(&mut self, instr: Box<Instr>) -> &mut Instr {
        let mut instr = instr;
        assert!(instr.pred.is_true());
        instr.pred = self.pred;
        self.b.push_instr(instr)
    }
}

impl<'a, T: SSABuilder> SSABuilder for PredicatedBuilder<'a, T> {
    fn alloc_ssa(&mut self, file: RegFile, comps: u8) -> SSARef {
        self.b.alloc_ssa(file, comps)
    }
}
