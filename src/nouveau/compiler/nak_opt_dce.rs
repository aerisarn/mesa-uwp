/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashSet;

struct DeadCodePass {
    live_ssa: HashSet<SSAValue>,
}

impl DeadCodePass {
    pub fn new() -> DeadCodePass {
        DeadCodePass {
            live_ssa: HashSet::new(),
        }
    }

    fn mark_ssa_live(&mut self, ssa: &SSAValue) {
        self.live_ssa.insert(*ssa);
    }

    fn mark_instr_live(&mut self, instr: &Instr) {
        if let Pred::SSA(ssa) = &instr.pred {
            self.mark_ssa_live(ssa);
        }

        for src in instr.srcs() {
            if let Ref::SSA(ssa) = &src.src_ref {
                self.mark_ssa_live(ssa);
            }
        }
    }

    fn is_dst_live(&self, dst: &Dst) -> bool {
        match dst {
            Ref::SSA(ssa) => self.live_ssa.get(ssa).is_some(),
            Ref::Zero => false,
            _ => panic!("Invalid SSA destination"),
        }
    }

    fn is_instr_live(&self, instr: &Instr) -> bool {
        if !instr.can_eliminate() {
            return true;
        }

        for dst in instr.dsts() {
            if self.is_dst_live(dst) {
                return true;
            }
        }

        false
    }

    pub fn run(&mut self, f: &mut Function) {
        let mut has_any_dead = false;

        for b in f.blocks.iter().rev() {
            for instr in b.instrs.iter().rev() {
                if self.is_instr_live(instr) {
                    self.mark_instr_live(instr);
                } else {
                    has_any_dead = true;
                }
            }
        }

        if has_any_dead {
            f.map_instrs(&|instr: Instr| -> Vec<Instr> {
                if self.is_instr_live(&instr) {
                    vec![instr]
                } else {
                    Vec::new()
                }
            })
        }
    }
}

impl Shader {
    pub fn opt_dce(&mut self) {
        for f in &mut self.functions {
            DeadCodePass::new().run(f);
        }
    }
}
