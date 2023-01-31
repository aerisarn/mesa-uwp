/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashMap;

struct CopyPropPass {
    ssa_map: HashMap<SSAValue, Vec<Src>>,
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
                match &instr.op {
                    Op::Vec(vec) => {
                        self.add_copy(
                            vec.dst.as_ssa().unwrap(),
                            vec.srcs.to_vec(),
                        );
                    }
                    Op::Split(split) => {
                        assert!(split.src.src_mod.is_none());
                        let src_ssa = split.src.src_ref.as_ssa().unwrap();
                        if let Some(src_vec) = self.get_copy(src_ssa).cloned() {
                            assert!(src_vec.len() == split.dsts.len());
                            for (i, dst) in split.dsts.iter().enumerate() {
                                if let Dst::SSA(ssa) = dst {
                                    self.add_copy(ssa, vec![src_vec[i]]);
                                }
                            }
                        }
                    }
                    _ => (),
                }

                if let Pred::SSA(src_ssa) = &instr.pred {
                    if let Some(src_vec) = self.get_copy(&src_ssa) {
                        assert!(src_vec[0].src_mod.is_none());
                        if let Ref::SSA(ssa) = src_vec[0].src_ref {
                            instr.pred = Pred::SSA(ssa);
                        }
                    }
                }

                for src in instr.srcs_mut() {
                    if let Ref::SSA(src_ssa) = src.src_ref {
                        if src_ssa.comps() == 1 {
                            if let Some(src_vec) = self.get_copy(&src_ssa) {
                                assert!(src_vec[0].src_mod.is_none());
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
