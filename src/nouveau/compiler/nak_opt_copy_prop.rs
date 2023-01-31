/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashMap;
use std::slice;

#[derive(Clone, Copy, Eq, PartialEq)]
enum CopyType {
    Raw,
    Bits,
    F32,
    I32,
}

#[derive(Clone)]
struct CopyEntry {
    typ: CopyType,
    src: Src,
}

impl CopyEntry {
    pub fn supports_type(&self, typ: CopyType) -> bool {
        match self.typ {
            CopyType::Raw => true,
            entry_typ => entry_typ == typ,
        }
    }
}

struct CopyPropPass {
    ssa_map: HashMap<SSAValue, Vec<CopyEntry>>,
}

impl CopyPropPass {
    pub fn new() -> CopyPropPass {
        CopyPropPass {
            ssa_map: HashMap::new(),
        }
    }

    fn add_copy(&mut self, dst: &SSAValue, typ: CopyType, src_vec: &[Src]) {
        let entries = src_vec
            .iter()
            .map(|src| {
                match typ {
                    CopyType::Raw => assert!(src.src_mod.is_none()),
                    CopyType::Bits => assert!(src.src_mod.is_bitwise()),
                    CopyType::F32 | CopyType::I32 => {
                        assert!(src.src_mod.is_alu())
                    }
                }
                CopyEntry {
                    typ: match src.src_mod {
                        SrcMod::None => CopyType::Raw,
                        SrcMod::Abs | SrcMod::Neg | SrcMod::NegAbs => {
                            assert!(
                                typ != CopyType::Raw && typ != CopyType::Bits
                            );
                            typ
                        }
                        SrcMod::Not => {
                            assert!(typ == CopyType::Bits);
                            typ
                        }
                    },
                    src: *src,
                }
            })
            .collect();
        self.ssa_map.insert(*dst, entries);
    }

    fn add_copy_entry(&mut self, dst: &SSAValue, entry: CopyEntry) {
        self.ssa_map.insert(*dst, vec![entry]);
    }

    fn get_copy(&mut self, dst: &SSAValue) -> Option<&Vec<CopyEntry>> {
        self.ssa_map.get(dst)
    }

    fn prop_to_pred(&mut self, pred: &mut Pred, pred_inv: &mut bool) {
        if let Pred::SSA(src_ssa) = pred {
            if let Some(src_vec) = self.get_copy(&src_ssa) {
                let entry = &src_vec[0];
                if !entry.supports_type(CopyType::Bits) {
                    return;
                }

                *pred = Pred::SSA(*entry.src.src_ref.as_ssa().unwrap());
                if entry.src.src_mod.has_not() {
                    *pred_inv = !*pred_inv;
                }
            }
        }
    }

    fn prop_to_src(&mut self, src: &mut Src, src_typ: CopyType) -> bool {
        if let SrcRef::SSA(src_ssa) = src.src_ref {
            if src_ssa.comps() != 1 {
                return false; /* TODO */
            }

            if let Some(src_vec) = self.get_copy(&src_ssa) {
                let entry = &src_vec[0];
                if !entry.supports_type(src_typ) {
                    return false;
                }

                let mut new_src = entry.src;
                match src_typ {
                    CopyType::Raw => {
                        assert!(src.src_mod.is_none());
                    }
                    CopyType::Bits => {
                        if src.src_mod.has_neg() {
                            new_src.src_mod = new_src.src_mod.neg();
                        }
                    }
                    CopyType::F32 | CopyType::I32 => {
                        if src.src_mod.has_abs() {
                            new_src.src_mod = new_src.src_mod.abs();
                        }
                        if src.src_mod.has_neg() {
                            new_src.src_mod = new_src.src_mod.neg();
                        }
                    }
                }
                *src = new_src;
                true
            } else {
                false
            }
        } else {
            false
        }
    }

    fn prop_to_srcs(&mut self, srcs: &mut [Src], src_typ: CopyType) -> bool {
        let mut progress = false;
        for src in srcs {
            progress |= self.prop_to_src(src, src_typ);
        }
        progress
    }

    pub fn run(&mut self, f: &mut Function) {
        for b in &mut f.blocks {
            for instr in &mut b.instrs {
                match &instr.op {
                    Op::FMov(mov) => {
                        if !mov.saturate {
                            self.add_copy(
                                mov.dst.as_ssa().unwrap(),
                                CopyType::F32,
                                slice::from_ref(&mov.src),
                            );
                        }
                    }
                    Op::IMov(mov) => {
                        self.add_copy(
                            mov.dst.as_ssa().unwrap(),
                            CopyType::I32,
                            slice::from_ref(&mov.src),
                        );
                    }
                    Op::Vec(vec) => {
                        self.add_copy(
                            vec.dst.as_ssa().unwrap(),
                            CopyType::Raw,
                            &vec.srcs,
                        );
                    }
                    Op::Split(split) => {
                        assert!(split.src.src_mod.is_none());
                        let src_ssa = split.src.src_ref.as_ssa().unwrap();
                        if let Some(src_vec) = self.get_copy(src_ssa) {
                            let mut src_vec = src_vec.clone();
                            assert!(src_vec.len() == split.dsts.len());
                            for (i, entry) in src_vec.drain(..).enumerate() {
                                if let Dst::SSA(ssa) = &split.dsts[i] {
                                    self.add_copy_entry(ssa, entry);
                                }
                            }
                        }
                    }
                    _ => (),
                }

                self.prop_to_pred(&mut instr.pred, &mut instr.pred_inv);

                match &mut instr.op {
                    Op::FAdd(_) | Op::FSet(_) => {
                        self.prop_to_srcs(instr.srcs_mut(), CopyType::F32);
                    }
                    Op::FSetP(op) => {
                        self.prop_to_srcs(&mut op.srcs, CopyType::F32);
                        /* TODO Other predicates */
                    }
                    Op::IAdd3(op) => {
                        self.prop_to_srcs(&mut op.srcs, CopyType::I32);
                        /* Carry doesn't have a negate modifier */
                        self.prop_to_src(&mut op.carry, CopyType::Raw);
                    }
                    Op::ISetP(op) => {
                        self.prop_to_srcs(&mut op.srcs, CopyType::I32);
                        /* TODO Other predicates */
                    }
                    Op::Lop3(op) => {
                        if self.prop_to_srcs(&mut op.srcs, CopyType::Bits) {
                            op.op = LogicOp::new_lut(&|mut x, mut y, mut z| {
                                if op.srcs[0].src_mod.has_not() {
                                    x = !x;
                                }
                                if op.srcs[1].src_mod.has_not() {
                                    y = !y;
                                }
                                if op.srcs[2].src_mod.has_not() {
                                    z = !z;
                                }
                                op.op.eval(x, y, z)
                            });
                            op.srcs[0].src_mod = SrcMod::None;
                            op.srcs[1].src_mod = SrcMod::None;
                            op.srcs[2].src_mod = SrcMod::None;
                        }
                    }
                    Op::PLop3(op) => {
                        self.prop_to_srcs(&mut op.srcs, CopyType::Bits);
                    }
                    _ => {
                        self.prop_to_srcs(instr.srcs_mut(), CopyType::Raw);
                    }
                }
            }
        }
    }
}

impl Shader {
    pub fn opt_copy_prop(&mut self) {
        for f in &mut self.functions {
            CopyPropPass::new().run(f);
        }
    }
}
