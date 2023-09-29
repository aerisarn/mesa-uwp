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
            srcs: [x, y],
            accum: SrcRef::True.into(),
        });
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
