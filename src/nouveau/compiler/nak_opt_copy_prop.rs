/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashMap;

struct CopyPropPass {
    ssa_map: HashMap<SSAValue, Vec<Ref>>,
}

impl CopyPropPass {
    pub fn new() -> CopyPropPass {
        CopyPropPass {
            ssa_map: HashMap::new(),
        }
    }

    fn add_copy(&mut self, dst: &SSAValue, src_vec: Vec<Src>) {
        self.ssa_map.insert(*dst, src_vec);
    }

    fn get_copy(&mut self, dst: &SSAValue) -> Option<&Vec<Src>> {
        self.ssa_map.get(dst)
    }

    pub fn run(&mut self, f: &mut Function) {
        for b in &mut f.blocks {
            for instr in &mut b.instrs {
                match instr.op {
                    Opcode::VEC => {
                        self.add_copy(
                            instr.dst(0).as_ssa().unwrap(),
                            instr.srcs().to_vec(),
                        );
                    }
                    Opcode::SPLIT => {
                        let src_ssa = instr.src(0).as_ssa().unwrap();
                        if let Some(src_vec) = self.get_copy(src_ssa).cloned() {
                            assert!(src_vec.len() == instr.num_dsts());
                            for i in 0..instr.num_dsts() {
                                if let Dst::SSA(ssa) = instr.dst(i) {
                                    self.add_copy(ssa, vec![src_vec[i]]);
                                }
                            }
                        }
                    }
                    _ => (),
                }

                if let Pred::SSA(src_ssa) = &instr.pred {
                    if let Some(src_vec) = self.get_copy(src_ssa) {
                        if let Src::SSA(ssa) = src_vec[0] {
                            instr.pred = Pred::SSA(ssa);
                        }
                    }
                }

                for src in instr.srcs_mut() {
                    if let Ref::SSA(src_ssa) = src {
                        if src_ssa.comps() == 1 {
                            if let Some(src_vec) = self.get_copy(src_ssa) {
                                *src = src_vec[0];
                            }
                        }
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
