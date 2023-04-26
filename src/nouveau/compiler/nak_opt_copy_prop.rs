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
    F64H,
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
    ssa_map: HashMap<SSAValue, CopyEntry>,
}

impl CopyPropPass {
    pub fn new() -> CopyPropPass {
        CopyPropPass {
            ssa_map: HashMap::new(),
        }
    }

    fn add_copy(&mut self, dst: SSAValue, typ: CopyType, src: Src) {
        match typ {
            CopyType::Raw => assert!(src.src_mod.is_none()),
            CopyType::Bits => assert!(src.src_mod.is_bitwise()),
            CopyType::F32 | CopyType::F64H | CopyType::I32 => {
                assert!(src.src_mod.is_alu())
            }
        }
        let typ = match src.src_mod {
            SrcMod::None => CopyType::Raw,
            SrcMod::FAbs | SrcMod::FNeg | SrcMod::FNegAbs => {
                assert!(typ == CopyType::F32 || typ == CopyType::F64H);
                typ
            }
            SrcMod::INeg => {
                assert!(typ == CopyType::I32);
                typ
            }
            SrcMod::BNot => {
                assert!(typ == CopyType::Bits);
                typ
            }
        };
        if let Some(ssa) = src.src_ref.as_ssa() {
            assert!(ssa.comps() == 1);
        }

        self.ssa_map.insert(dst, CopyEntry { typ: typ, src: src });
    }

    fn get_copy(&mut self, dst: &SSAValue) -> Option<&CopyEntry> {
        self.ssa_map.get(dst)
    }

    fn prop_to_pred(&mut self, pred: &mut Pred, pred_inv: &mut bool) {
        if let Pred::SSA(src_ssa) = pred {
            if let Some(entry) = self.get_copy(&src_ssa) {
                if !entry.supports_type(CopyType::Bits) {
                    return;
                }

                let copy_ssa = entry.src.src_ref.as_ssa().unwrap();
                assert!(copy_ssa.comps() == 1 && copy_ssa.is_predicate());
                *pred = Pred::SSA(copy_ssa[0]);
                match entry.src.src_mod {
                    SrcMod::None => (),
                    SrcMod::BNot => {
                        *pred_inv = !*pred_inv;
                    }
                    _ => panic!("Invalid predicate modifier"),
                }
            }
        }
    }

    fn prop_to_src(&mut self, src: &mut Src, src_typ: CopyType) -> bool {
        if let SrcRef::SSA(src_ssa) = src.src_ref {
            let mut found_copy = false;
            let mut copy_mod = src.src_mod;
            let mut copy_vals = [SSAValue::NONE; 4];
            for c in 0..src_ssa.comps() {
                let c_val = &src_ssa[usize::from(c)];
                if let Some(entry) = self.get_copy(c_val) {
                    let c_typ = match src_typ {
                        CopyType::Raw => CopyType::Raw,
                        CopyType::Bits | CopyType::F32 | CopyType::I32 => {
                            assert!(src_ssa.comps() == 1);
                            src_typ
                        }
                        CopyType::F64H => {
                            assert!(src_ssa.comps() == 2);
                            /* The low bits of a 64-bit value are read raw */
                            if c == 0 {
                                CopyType::Raw
                            } else {
                                CopyType::F64H
                            }
                        }
                    };

                    if !entry.supports_type(c_typ) {
                        return false;
                    }

                    if c_typ != CopyType::Raw {
                        assert!(c == src_ssa.comps() - 1);
                        copy_mod = entry.src.src_mod.modify(src.src_mod);
                    }

                    if let Some(e_ssa) = entry.src.as_ssa() {
                        assert!(e_ssa.comps() == 1);
                        found_copy = true;
                        copy_vals[usize::from(c)] = e_ssa[0];
                    } else if src_ssa.comps() == 1 {
                        src.src_mod = copy_mod;
                        src.src_ref = entry.src.src_ref;
                        return true;
                    } else {
                        return false;
                    }
                } else {
                    copy_vals[usize::from(c)] = *c_val;
                }
            }

            if found_copy {
                let comps = usize::from(src_ssa.comps());
                let copy_ssa = SSARef::try_from(&copy_vals[..comps]).unwrap();
                src.src_mod = copy_mod;
                src.src_ref = copy_ssa.into();
                return true;
            }
        }
        false
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
                    Op::Mov(mov) => {
                        let dst = mov.dst.as_ssa().unwrap();
                        assert!(dst.comps() == 1);
                        if mov.quad_lanes == 0xf {
                            self.add_copy(dst[0], CopyType::Raw, mov.src);
                        }
                    }
                    Op::FMov(mov) => {
                        let dst = mov.dst.as_ssa().unwrap();
                        assert!(dst.comps() == 1);
                        if !mov.saturate {
                            self.add_copy(dst[0], CopyType::F32, mov.src);
                        }
                    }
                    Op::DMov(mov) => {
                        let dst = mov.dst.as_ssa().unwrap();
                        assert!(dst.comps() == 2);
                        if !mov.saturate {
                            if let Some(src) = mov.src.src_ref.as_ssa() {
                                self.add_copy(
                                    dst[0],
                                    CopyType::Bits,
                                    src[0].into(),
                                );
                                self.add_copy(
                                    dst[1],
                                    CopyType::F64H,
                                    Src {
                                        src_ref: src[1].into(),
                                        src_mod: mov.src.src_mod,
                                    },
                                );
                            }
                        }
                    }
                    Op::IMov(mov) => {
                        let dst = mov.dst.as_ssa().unwrap();
                        assert!(dst.comps() == 1);
                        self.add_copy(dst[0], CopyType::I32, mov.src);
                    }
                    Op::ParCopy(pcopy) => {
                        for (src, dst) in pcopy.iter() {
                            let dst = dst.as_ssa().unwrap();
                            assert!(dst.comps() == 1);
                            self.add_copy(dst[0], CopyType::Raw, *src);
                        }
                    }
                    _ => (),
                }

                self.prop_to_pred(&mut instr.pred, &mut instr.pred_inv);

                match &mut instr.op {
                    Op::FAdd(_)
                    | Op::FFma(_)
                    | Op::FMnMx(_)
                    | Op::FMul(_)
                    | Op::FSet(_) => {
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
                                if op.srcs[0].src_mod.is_bnot() {
                                    x = !x;
                                }
                                if op.srcs[1].src_mod.is_bnot() {
                                    y = !y;
                                }
                                if op.srcs[2].src_mod.is_bnot() {
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
