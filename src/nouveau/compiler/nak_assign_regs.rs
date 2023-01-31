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
}

impl TrivialRegAlloc {
    pub fn new() -> TrivialRegAlloc {
        TrivialRegAlloc {
            next_reg: 16, /* Leave some space for FS outputs */
            next_ureg: 0,
            next_pred: 0,
            next_upred: 0,
            reg_map: HashMap::new(),
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

    pub fn rewrite_ssa(&mut self, ssa: SSAValue) -> RegRef {
        if let Some(reg) = self.reg_map.get(&ssa) {
            *reg
        } else {
            let reg = self.alloc_reg(ssa.file(), ssa.comps());
            self.reg_map.insert(ssa, reg);
            reg
        }
    }

    pub fn do_alloc(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    if let Pred::SSA(ssa) = instr.pred {
                        instr.pred = Pred::Reg(self.rewrite_ssa(ssa));
                    }
                    for dst in instr.dsts_mut() {
                        if let Dst::SSA(ssa) = dst {
                            *dst = self.rewrite_ssa(*ssa).into();
                        }
                    }
                    for src in instr.srcs_mut() {
                        if let SrcRef::SSA(ssa) = src.src_ref {
                            src.src_ref = self.rewrite_ssa(ssa).into();
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
