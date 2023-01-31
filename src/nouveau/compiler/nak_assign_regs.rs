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
    reg_map: HashMap<SSAValue, Ref>,
}

impl TrivialRegAlloc {
    pub fn new() -> TrivialRegAlloc {
        TrivialRegAlloc {
            next_reg: 16, /* Leave some space for FS outputs */
            next_ureg: 0,
            reg_map: HashMap::new(),
        }
    }

    fn alloc_reg(&mut self, file: RegFile, comps: u8) -> Ref {
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
            RegFile::Pred | RegFile::UPred => panic!("Not handled"),
        };
        Ref::new_reg(file, idx, comps)
    }

    pub fn rewrite_ref(&mut self, r: &Ref) -> Ref {
        if let Ref::SSA(ssa) = r {
            if let Some(reg) = self.reg_map.get(ssa) {
                *reg
            } else {
                let reg = self.alloc_reg(ssa.file(), ssa.comps());
                self.reg_map.insert(*ssa, reg);
                reg
            }
        } else {
            *r
        }
    }

    pub fn do_alloc(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    for dst in instr.dsts_mut() {
                        let new_dst = self.rewrite_ref(&dst);
                        *dst = new_dst;
                    }
                    for src in instr.srcs_mut() {
                        let new_src = self.rewrite_ref(&src);
                        *src = new_src;
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
