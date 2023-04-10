/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(unstable_name_collisions)]

use crate::nak_ir::*;
use crate::util::NextMultipleOf;

use std::collections::HashMap;

struct TrivialRegAlloc {
    next_reg: u8,
    next_ureg: u8,
    next_pred: u8,
    next_upred: u8,
    reg_map: HashMap<SSAValue, RegRef>,
    phi_map: HashMap<u32, RegRef>,
}

impl TrivialRegAlloc {
    pub fn new() -> TrivialRegAlloc {
        TrivialRegAlloc {
            next_reg: 16, /* Leave some space for FS outputs */
            next_ureg: 0,
            next_pred: 0,
            next_upred: 0,
            reg_map: HashMap::new(),
            phi_map: HashMap::new(),
        }
    }

    fn alloc_reg(&mut self, file: RegFile, comps: u8) -> RegRef {
        let align = comps.next_power_of_two();
        let idx = match file {
            RegFile::GPR => {
                let idx = self.next_reg.next_multiple_of(align);
                self.next_reg = idx + comps;
                idx
            }
            RegFile::UGPR => {
                let idx = self.next_ureg.next_multiple_of(align);
                self.next_ureg = idx + comps;
                idx
            }
            RegFile::Pred => {
                let idx = self.next_pred.next_multiple_of(align);
                self.next_pred = idx + comps;
                idx
            }
            RegFile::UPred => {
                let idx = self.next_upred.next_multiple_of(align);
                self.next_upred = idx + comps;
                idx
            }
        };
        RegRef::new(file, idx, comps)
    }

    fn alloc_ssa(&mut self, ssa: SSAValue) -> RegRef {
        let reg = self.alloc_reg(ssa.file(), ssa.comps());
        let old = self.reg_map.insert(ssa, reg);
        assert!(old.is_none());
        reg
    }

    fn get_ssa_reg(&self, ssa: SSAValue) -> RegRef {
        *self.reg_map.get(&ssa).unwrap()
    }

    fn map_src(&self, mut src: Src) -> Src {
        if let SrcRef::SSA(ssa) = src.src_ref {
            src.src_ref = self.get_ssa_reg(ssa).into();
        }
        src
    }

    pub fn do_alloc(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    match &instr.op {
                        Op::PhiDsts(phi) => {
                            let mut pcopy = OpParCopy::new();

                            assert!(phi.ids.len() == phi.dsts.len());
                            for (id, dst) in phi.iter() {
                                let dst_ssa = dst.as_ssa().unwrap();
                                let dst_reg = self.alloc_ssa(*dst_ssa);
                                let src_reg = self
                                    .alloc_reg(dst_ssa.file(), dst_ssa.comps());
                                self.phi_map.insert(*id, src_reg);
                                pcopy.srcs.push(src_reg.into());
                                pcopy.dsts.push(dst_reg.into());
                            }

                            instr.op = Op::ParCopy(pcopy);
                        }
                        _ => (),
                    }
                }
            }
        }

        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    match &instr.op {
                        Op::PhiSrcs(phi) => {
                            assert!(phi.ids.len() == phi.srcs.len());
                            instr.op = Op::ParCopy(OpParCopy {
                                srcs: phi
                                    .srcs
                                    .iter()
                                    .map(|src| self.map_src(*src))
                                    .collect(),
                                dsts: phi
                                    .ids
                                    .iter()
                                    .map(|id| {
                                        (*self.phi_map.get(id).unwrap()).into()
                                    })
                                    .collect(),
                            });
                        }
                        _ => {
                            if let Pred::SSA(ssa) = instr.pred {
                                instr.pred = self.get_ssa_reg(ssa).into();
                            }
                            for dst in instr.dsts_mut() {
                                if let Dst::SSA(ssa) = dst {
                                    *dst = self.alloc_ssa(*ssa).into();
                                }
                            }
                            for src in instr.srcs_mut() {
                                *src = self.map_src(*src);
                            }
                        }
                    }
                }
            }
        }
    }
}

impl Shader {
    pub fn assign_regs_trivial(&mut self) {
        TrivialRegAlloc::new().do_alloc(self);
    }
}
