/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::bitset::*;
use crate::nak_ir::*;

use std::collections::HashMap;

pub struct BlockLiveness {
    defs: BitSet,
    uses: BitSet,
    last_use: HashMap<u32, usize>,
    live_in: BitSet,
    live_out: BitSet,
    pub predecessors: Vec<u32>,
    pub successors: [Option<u32>; 2],
}

impl BlockLiveness {
    fn new() -> Self {
        Self {
            defs: BitSet::new(),
            uses: BitSet::new(),
            last_use: HashMap::new(),
            live_in: BitSet::new(),
            live_out: BitSet::new(),
            predecessors: Vec::new(),
            successors: [None; 2],
        }
    }

    fn add_def(&mut self, val: &SSAValue) {
        self.defs.insert(val.idx().try_into().unwrap());
    }

    fn add_use(&mut self, val: &SSAValue, ip: usize) {
        self.uses.insert(val.idx().try_into().unwrap());
        self.last_use.insert(val.idx(), ip);
    }

    fn add_live_block(&mut self, block: &BasicBlock) {
        for (ip, instr) in block.instrs.iter().enumerate() {
            if let PredRef::SSA(val) = &instr.pred.pred_ref {
                self.add_use(val, ip);
            }

            for src in instr.srcs() {
                for sv in src.iter_ssa() {
                    self.add_use(sv, ip);
                }
            }

            for dst in instr.dsts() {
                if let Dst::SSA(sr) = dst {
                    for sv in sr.iter() {
                        self.add_def(sv);
                    }
                }
            }
        }
    }

    pub fn is_live_after(&self, val: &SSAValue, ip: usize) -> bool {
        if self.live_out.get(val.idx().try_into().unwrap()) {
            true
        } else {
            if let Some(last_use_ip) = self.last_use.get(&val.idx()) {
                *last_use_ip > ip
            } else {
                false
            }
        }
    }

    #[allow(dead_code)]
    pub fn is_live_in(&self, val: &SSAValue) -> bool {
        self.live_in.get(val.idx().try_into().unwrap())
    }

    #[allow(dead_code)]
    pub fn is_live_out(&self, val: &SSAValue) -> bool {
        self.live_out.get(val.idx().try_into().unwrap())
    }
}

pub struct Liveness {
    blocks: HashMap<u32, BlockLiveness>,
}

impl Liveness {
    pub fn block(&self, block: &BasicBlock) -> &BlockLiveness {
        self.blocks.get(&block.id).unwrap()
    }

    fn link_blocks(&mut self, p_id: u32, s_id: u32) {
        let s = self.blocks.get_mut(&s_id).unwrap();
        s.predecessors.push(p_id);

        let p = self.blocks.get_mut(&p_id).unwrap();
        if p.successors[0].is_none() {
            p.successors[0] = Some(s_id);
        } else {
            assert!(p.successors[1].is_none());
            p.successors[1] = Some(s_id);
        }
    }

    pub fn for_function(func: &Function) -> Liveness {
        let mut l = Liveness {
            blocks: HashMap::new(),
        };
        let mut live_in = HashMap::new();

        for b in &func.blocks {
            let mut bl = BlockLiveness::new();
            bl.add_live_block(&b);
            l.blocks.insert(b.id, bl);

            live_in.insert(b.id, BitSet::new());
        }

        for (i, b) in func.blocks.iter().enumerate() {
            if b.falls_through() {
                l.link_blocks(b.id, func.blocks[i + 1].id);
            }

            if let Some(br) = b.branch() {
                match &br.op {
                    Op::Bra(bra) => {
                        l.link_blocks(b.id, bra.target);
                    }
                    Op::Exit(_) => (),
                    _ => panic!("Unhandled branch op"),
                }
            }
        }

        let mut to_do = true;
        while to_do {
            to_do = false;
            for b in func.blocks.iter().rev() {
                let bl = l.blocks.get_mut(&b.id).unwrap();

                /* Compute live-out */
                for s in bl.successors {
                    if let Some(sb_id) = s {
                        let s_live_in = live_in.get(&sb_id).unwrap();
                        to_do |= bl.live_out.union_with(s_live_in);
                    }
                }

                let b_live_in = live_in.get_mut(&b.id).unwrap();

                let new_live_in =
                    (bl.live_out.clone() | bl.uses.clone()) & !bl.defs.clone();
                to_do |= b_live_in.union_with(&new_live_in);
            }
        }

        for b in &func.blocks {
            let bl = l.blocks.get_mut(&b.id).unwrap();
            bl.live_in = live_in.remove(&b.id).unwrap();
        }

        l
    }
}
