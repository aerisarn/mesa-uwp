/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::{HashMap, HashSet};

struct LegalizeInstr<'a> {
    ssa_alloc: &'a mut SSAValueAllocator,
    instrs: Vec<Box<Instr>>,
}

fn src_is_reg(src: &Src) -> bool {
    match src.src_ref {
        SrcRef::Zero | SrcRef::True | SrcRef::False | SrcRef::SSA(_) => true,
        SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
        SrcRef::Reg(_) => panic!("Not in SSA form"),
    }
}

fn src_as_lop_imm(src: &Src) -> Option<bool> {
    let x = match src.src_ref {
        SrcRef::Zero => false,
        SrcRef::True => true,
        SrcRef::False => false,
        SrcRef::Imm32(i) => {
            if i == 0 {
                false
            } else if i == !0 {
                true
            } else {
                return None;
            }
        }
        _ => return None,
    };
    Some(x ^ src.src_mod.is_bnot())
}

fn fold_lop_src(src: &Src, x: &mut u8) {
    if let Some(i) = src_as_lop_imm(src) {
        *x = if i { !0 } else { 0 };
    }
}

impl<'a> LegalizeInstr<'a> {
    pub fn new(ssa_alloc: &'a mut SSAValueAllocator) -> LegalizeInstr {
        LegalizeInstr {
            ssa_alloc: ssa_alloc,
            instrs: Vec::new(),
        }
    }

    pub fn mov_src(&mut self, src: &mut Src, file: RegFile) {
        let val = self.ssa_alloc.alloc(file);
        self.instrs
            .push(Instr::new_mov(val.into(), src.src_ref.into()).into());
        src.src_ref = val.into();
    }

    pub fn mov_src_if_not_reg(&mut self, src: &mut Src, file: RegFile) {
        if !src_is_reg(&src) {
            self.mov_src(src, file);
        }
    }

    pub fn swap_srcs_if_not_reg(&mut self, x: &mut Src, y: &mut Src) {
        if !src_is_reg(x) && src_is_reg(y) {
            std::mem::swap(x, y);
        }
    }

    pub fn map(&mut self, mut instr: Box<Instr>) -> MappedInstrs {
        match &mut instr.op {
            Op::FAdd(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::FFma(op) => {
                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::FMnMx(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::FMul(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::FSet(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    op.cmp_op = op.cmp_op.flip();
                }
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::FSetP(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    op.cmp_op = op.cmp_op.flip();
                }
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::MuFu(_) => (), /* Nothing to do */
            Op::DAdd(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::IAbs(_) | Op::INeg(_) => (), /* Nothing to do */
            Op::IAdd3(op) => {
                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.swap_srcs_if_not_reg(src2, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::IMad(op) => {
                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::IMad64(op) => {
                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::IMnMx(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::ISetP(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    op.cmp_op = op.cmp_op.flip();
                }
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::Lop3(op) => {
                /* Fold constants if we can */
                op.op = LogicOp::new_lut(&|mut x, mut y, mut z| {
                    fold_lop_src(&op.srcs[0], &mut x);
                    fold_lop_src(&op.srcs[1], &mut y);
                    fold_lop_src(&op.srcs[2], &mut z);
                    op.op.eval(x, y, z)
                });
                for src in &mut op.srcs {
                    src.src_mod = SrcMod::None;
                    if src_as_lop_imm(src).is_some() {
                        src.src_ref = SrcRef::Zero;
                    }
                }

                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    op.op = LogicOp::new_lut(&|x, y, z| op.op.eval(y, x, z))
                }
                if !src_is_reg(src2) && src_is_reg(src1) {
                    std::mem::swap(src2, src1);
                    op.op = LogicOp::new_lut(&|x, y, z| op.op.eval(x, z, y))
                }

                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::Shf(op) => {
                self.mov_src_if_not_reg(&mut op.low, RegFile::GPR);
                self.mov_src_if_not_reg(&mut op.high, RegFile::GPR);
            }
            Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::Mov(_) => (),
            Op::Sel(op) => {
                let [ref mut src0, ref mut src1] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    op.cond.src_mod = op.cond.src_mod.bnot();
                }
                self.mov_src_if_not_reg(src0, RegFile::GPR);
            }
            Op::PLop3(op) => {
                /* Fold constants if we can */
                for lop in &mut op.ops {
                    *lop = LogicOp::new_lut(&|mut x, mut y, mut z| {
                        fold_lop_src(&op.srcs[0], &mut x);
                        fold_lop_src(&op.srcs[1], &mut y);
                        fold_lop_src(&op.srcs[2], &mut z);
                        lop.eval(x, y, z)
                    });
                }
                for src in &mut op.srcs {
                    src.src_mod = SrcMod::None;
                    if src_as_lop_imm(src).is_some() {
                        src.src_ref = SrcRef::True;
                    }
                }

                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                if !src_is_reg(src0) && src_is_reg(src1) {
                    std::mem::swap(src0, src1);
                    for lop in &mut op.ops {
                        *lop = LogicOp::new_lut(&|x, y, z| lop.eval(y, x, z));
                    }
                }
                if !src_is_reg(src2) && src_is_reg(src1) {
                    std::mem::swap(src2, src1);
                    for lop in &mut op.ops {
                        *lop = LogicOp::new_lut(&|x, y, z| lop.eval(x, z, y));
                    }
                }

                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::Ldc(_) => (), /* Nothing to do */
            _ => {
                let src_types = instr.src_types();
                for (i, src) in instr.srcs_mut().iter_mut().enumerate() {
                    match src_types[i] {
                        SrcType::SSA => {
                            if src.as_ssa().is_none() {
                                self.mov_src(src, RegFile::GPR);
                            }
                        }
                        SrcType::GPR => {
                            self.mov_src_if_not_reg(src, RegFile::GPR);
                        }
                        SrcType::ALU
                        | SrcType::F32
                        | SrcType::F64
                        | SrcType::I32 => {
                            panic!("ALU srcs must be legalized explicitly");
                        }
                        SrcType::Pred => {
                            panic!("Predicates must be legalized explicitly");
                        }
                    }
                }
            }
        }

        let mut vec_src_map: HashMap<SSARef, SSARef> = HashMap::new();
        let mut vec_comps = HashSet::new();
        let mut pcopy = OpParCopy::new();
        for src in instr.srcs_mut() {
            if let SrcRef::SSA(vec) = &src.src_ref {
                if vec.comps() == 1 {
                    continue;
                }

                /* If the same vector shows up twice in one instruction, that's
                 * okay. Just make it look the same as the previous source we
                 * fixed up.
                 */
                if let Some(new_vec) = vec_src_map.get(&vec) {
                    src.src_ref = (*new_vec).into();
                    continue;
                }

                let mut new_vec = *vec;
                for c in 0..vec.comps() {
                    let ssa = vec[usize::from(c)];
                    /* If the same SSA value shows up in multiple non-identical
                     * vector sources or as multiple components in the same
                     * source, we need to make a copy so it can get assigned to
                     * multiple different registers.
                     */
                    if vec_comps.get(&ssa).is_some() {
                        let copy = self.ssa_alloc.alloc(ssa.file());
                        pcopy.push(copy.into(), ssa.into());
                        new_vec[usize::from(c)] = copy;
                    } else {
                        vec_comps.insert(ssa);
                    }
                }

                vec_src_map.insert(*vec, new_vec);
                src.src_ref = new_vec.into();
            }
        }

        if !pcopy.is_empty() {
            self.instrs.push(Instr::new_boxed(Op::ParCopy(pcopy)));
        }

        self.instrs.push(instr);
        MappedInstrs::Many(std::mem::replace(&mut self.instrs, Vec::new()))
    }
}

impl Shader {
    pub fn legalize(&mut self) {
        self.map_instrs(&|instr, ssa_alloc| -> MappedInstrs {
            LegalizeInstr::new(ssa_alloc).map(instr)
        });
    }
}
