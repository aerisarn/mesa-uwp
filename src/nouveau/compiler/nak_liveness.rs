/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::cell::{Ref, RefCell};
use std::collections::HashMap;

struct SSAEntry {
    defined: bool,
    uses: Vec<usize>,
}

impl SSAEntry {
    fn add_def(&mut self) {
        self.defined = true;
    }

    fn add_in_block_use(&mut self, use_ip: usize) {
        self.uses.push(use_ip);
    }

    fn add_successor_use(
        &mut self,
        num_block_instrs: usize,
        use_ip: usize,
    ) -> bool {
        /* IPs are relative to the start of their block */
        let use_ip = num_block_instrs + use_ip;

        if let Some(last_use_ip) = self.uses.last_mut() {
            if *last_use_ip < num_block_instrs {
                /* We've never seen a successor use before */
                self.uses.push(use_ip);
                true
            } else if *last_use_ip > use_ip {
                /* Otherwise, we want the minimum next use */
                *last_use_ip = use_ip;
                true
            } else {
                false
            }
        } else {
            self.uses.push(use_ip);
            true
        }
    }
}

pub struct BlockLiveness {
    num_instrs: usize,
    ssa_map: HashMap<SSAValue, SSAEntry>,
    pub predecessors: Vec<u32>,
    pub successors: [Option<u32>; 2],
}

impl BlockLiveness {
    fn new(num_instrs: usize) -> Self {
        Self {
            num_instrs: num_instrs,
            ssa_map: HashMap::new(),
            predecessors: Vec::new(),
            successors: [None; 2],
        }
    }

    fn entry_mut(&mut self, ssa: SSAValue) -> &mut SSAEntry {
        self.ssa_map.entry(ssa).or_insert_with(|| SSAEntry {
            defined: false,
            uses: Vec::new(),
        })
    }

    fn add_live_block(&mut self, block: &BasicBlock) {
        for (ip, instr) in block.instrs.iter().enumerate() {
            if let PredRef::SSA(val) = &instr.pred.pred_ref {
                self.entry_mut(*val).add_in_block_use(ip);
            }

            for src in instr.srcs() {
                for sv in src.iter_ssa() {
                    self.entry_mut(*sv).add_in_block_use(ip);
                }
            }

            for dst in instr.dsts() {
                if let Dst::SSA(sr) = dst {
                    for sv in sr.iter() {
                        self.entry_mut(*sv).add_def();
                    }
                }
            }
        }
    }

    pub fn is_live_after(&self, val: &SSAValue, ip: usize) -> bool {
        if let Some(entry) = self.ssa_map.get(val) {
            if let Some(last_use_ip) = entry.uses.last() {
                *last_use_ip > ip
            } else {
                false
            }
        } else {
            false
        }
    }

    #[allow(dead_code)]
    pub fn is_live_in(&self, val: &SSAValue) -> bool {
        if let Some(entry) = self.ssa_map.get(val) {
            !entry.defined && !entry.uses.is_empty()
        } else {
            false
        }
    }

    #[allow(dead_code)]
    pub fn is_live_out(&self, val: &SSAValue) -> bool {
        if let Some(entry) = self.ssa_map.get(val) {
            if let Some(last_use_ip) = entry.uses.last() {
                *last_use_ip >= self.num_instrs
            } else {
                false
            }
        } else {
            false
        }
    }
}

pub struct Liveness {
    blocks: HashMap<u32, RefCell<BlockLiveness>>,
}

impl Liveness {
    pub fn block(&self, block: &BasicBlock) -> Ref<BlockLiveness> {
        self.blocks.get(&block.id).unwrap().borrow()
    }

    fn link_blocks(&mut self, p_id: u32, s_id: u32) {
        let s = self.blocks.get_mut(&s_id).unwrap().get_mut();
        s.predecessors.push(p_id);

        let p = self.blocks.get_mut(&p_id).unwrap().get_mut();
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

        for b in &func.blocks {
            let mut bl = BlockLiveness::new(b.instrs.len());
            bl.add_live_block(&b);
            l.blocks.insert(b.id, RefCell::new(bl));
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
                let num_instrs = b.instrs.len();
                let mut bl = l.blocks.get(&b.id).unwrap().borrow_mut();

                /* Compute live-out */
                for s in bl.successors {
                    let Some(sb_id) = s else {
                        continue;
                    };

                    if sb_id == b.id {
                        for entry in bl.ssa_map.values_mut() {
                            if entry.defined {
                                continue;
                            }

                            let Some(first_use_ip) = entry.uses.first() else {
                                continue;
                            };

                            to_do |= entry
                                .add_successor_use(num_instrs, *first_use_ip);
                        }
                    } else {
                        let sbl = l.blocks.get(&sb_id).unwrap().borrow();
                        for (ssa, entry) in sbl.ssa_map.iter() {
                            if entry.defined {
                                continue;
                            }

                            let Some(first_use_ip) = entry.uses.first() else {
                                continue;
                            };

                            to_do |= bl
                                .entry_mut(*ssa)
                                .add_successor_use(num_instrs, *first_use_ip);
                        }
                    }
                }
            }
        }

        l
    }
}
