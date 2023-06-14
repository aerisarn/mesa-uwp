/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashMap;

struct CFGBlock {
    pred: Vec<u32>,
    num_succ: u8,
    succ: [u32; 2],
}

pub struct CFG {
    block_map: HashMap<u32, CFGBlock>,
}

impl CFG {
    fn block_mut(&mut self, id: u32) -> &mut CFGBlock {
        self.block_map.entry(id).or_insert_with(|| CFGBlock {
            pred: Vec::new(),
            num_succ: 0,
            succ: [0_u32; 2],
        })
    }

    fn block(&self, id: u32) -> &CFGBlock {
        self.block_map.get(&id).unwrap()
    }

    pub fn block_predecessors(&self, id: u32) -> &[u32] {
        &self.block(id).pred
    }

    pub fn block_successors(&self, id: u32) -> &[u32] {
        let b = self.block(id);
        let num_succ = usize::try_from(b.num_succ).unwrap();
        &b.succ[0..num_succ]
    }

    pub fn for_function(f: &Function) -> CFG {
        let mut cfg = CFG {
            block_map: HashMap::new(),
        };

        for (i, bb) in f.blocks.iter().enumerate() {
            let mut succ = [0_u32; 2];
            let mut num_succ = 0_usize;

            if bb.falls_through() {
                succ[num_succ] = f.blocks[i + 1].id;
                num_succ += 1;
            }

            if let Some(br) = bb.branch() {
                match &br.op {
                    Op::Bra(bra) => {
                        succ[num_succ] = bra.target;
                        num_succ += 1;
                    }
                    Op::Exit(_) => (),
                    _ => panic!("Unhandled branch op"),
                }
            }

            for si in 0..num_succ {
                cfg.block_mut(succ[si]).pred.push(bb.id);
            }

            let cb = cfg.block_mut(bb.id);
            assert!(cb.num_succ == 0);
            cb.num_succ = num_succ.try_into().unwrap();
            cb.succ = succ;
        }

        cfg
    }
}
