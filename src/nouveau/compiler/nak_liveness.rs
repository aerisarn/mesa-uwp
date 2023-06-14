/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_cfg::CFG;
use crate::nak_ir::*;

use std::cell::RefCell;
use std::cmp::max;
use std::collections::{HashMap, HashSet};

pub trait BlockLiveness {
    fn is_live_after_ip(&self, val: &SSAValue, ip: usize) -> bool;
    fn is_live_in(&self, val: &SSAValue) -> bool;
    fn is_live_out(&self, val: &SSAValue) -> bool;
}

pub trait Liveness {
    type PerBlock: BlockLiveness;

    fn block_live(&self, id: u32) -> &Self::PerBlock;
}

struct SSAUseDef {
    defined: bool,
    uses: Vec<usize>,
}

impl SSAUseDef {
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

pub struct NextUseBlockLiveness {
    num_instrs: usize,
    ssa_map: HashMap<SSAValue, SSAUseDef>,
    max_live: PerRegFile<u32>,
}

impl NextUseBlockLiveness {
    fn entry_mut(&mut self, ssa: SSAValue) -> &mut SSAUseDef {
        self.ssa_map.entry(ssa).or_insert_with(|| SSAUseDef {
            defined: false,
            uses: Vec::new(),
        })
    }

    fn for_block(block: &BasicBlock) -> Self {
        let mut bl = Self {
            num_instrs: block.instrs.len(),
            ssa_map: HashMap::new(),
            max_live: Default::default(),
        };

        for (ip, instr) in block.instrs.iter().enumerate() {
            if let PredRef::SSA(val) = &instr.pred.pred_ref {
                bl.entry_mut(*val).add_in_block_use(ip);
            }

            for src in instr.srcs() {
                for sv in src.iter_ssa() {
                    bl.entry_mut(*sv).add_in_block_use(ip);
                }
            }

            for dst in instr.dsts() {
                if let Dst::SSA(sr) = dst {
                    for sv in sr.iter() {
                        bl.entry_mut(*sv).add_def();
                    }
                }
            }
        }

        bl
    }

    #[allow(dead_code)]
    pub fn max_live(&self, file: RegFile) -> u32 {
        self.max_live[file]
    }
}

impl BlockLiveness for NextUseBlockLiveness {
    fn is_live_after_ip(&self, val: &SSAValue, ip: usize) -> bool {
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

    fn is_live_in(&self, val: &SSAValue) -> bool {
        if let Some(entry) = self.ssa_map.get(val) {
            !entry.defined && !entry.uses.is_empty()
        } else {
            false
        }
    }

    fn is_live_out(&self, val: &SSAValue) -> bool {
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

pub struct NextUseLiveness {
    blocks: HashMap<u32, NextUseBlockLiveness>,
    max_live: PerRegFile<u32>,
}

impl NextUseLiveness {
    pub fn max_live(&self, file: RegFile) -> u32 {
        self.max_live[file]
    }

    pub fn for_function(func: &Function, cfg: &CFG) -> NextUseLiveness {
        let mut blocks = HashMap::new();
        for b in &func.blocks {
            let bl = NextUseBlockLiveness::for_block(b);
            blocks.insert(b.id, RefCell::new(bl));
        }

        let mut to_do = true;
        while to_do {
            to_do = false;
            for b in func.blocks.iter().rev() {
                let num_instrs = b.instrs.len();
                let mut bl = blocks.get(&b.id).unwrap().borrow_mut();

                /* Compute live-out */
                for sb_id in cfg.block_successors(b.id) {
                    if *sb_id == b.id {
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
                        let sbl = blocks.get(&sb_id).unwrap().borrow();
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

        let mut l = NextUseLiveness {
            blocks: HashMap::from_iter(
                blocks.drain().map(|(k, v)| (k, v.into_inner())),
            ),
            max_live: Default::default(),
        };

        for b in func.blocks.iter().rev() {
            let bl = l.blocks.get_mut(&b.id).unwrap();

            /* Populate a live set based on live out */
            let mut dead = HashSet::new();
            let mut live: PerRegFile<u32> = Default::default();
            for (ssa, entry) in bl.ssa_map.iter() {
                if !entry.defined {
                    assert!(!entry.uses.is_empty());
                    live[ssa.file()] += 1;
                }
            }

            for (ip, instr) in b.instrs.iter().enumerate() {
                /* Vector destinations go live before sources are killed */
                for dst in instr.dsts() {
                    if let Dst::SSA(vec) = dst {
                        if vec.comps() > 1 {
                            live[vec.file()] += u32::from(vec.comps());
                        }
                    }
                }

                for (bml, l) in bl.max_live.values_mut().zip(live.values()) {
                    *bml = max(*bml, *l);
                }

                /* Because any given SSA value may appear multiple times in
                 * sources, we have to de-duplicate in order to get an accurate
                 * count.
                 */
                dead.clear();

                if let PredRef::SSA(ssa) = &instr.pred.pred_ref {
                    if !bl.is_live_after_ip(ssa, ip) {
                        dead.insert(*ssa);
                    }
                }

                for src in instr.srcs() {
                    if let SrcRef::SSA(vec) = &src.src_ref {
                        for ssa in vec.iter() {
                            if !bl.is_live_after_ip(ssa, ip) {
                                dead.insert(*ssa);
                            }
                        }
                    }
                }

                for ssa in dead.iter() {
                    live[ssa.file()] -= 1;
                }

                /* Scalar destinations go live after we've killed sources */
                for dst in instr.dsts() {
                    if let Dst::SSA(vec) = dst {
                        if vec.comps() == 1 {
                            live[vec.file()] += 1;
                        }
                    }
                }
            }

            for (ml, bml) in l.max_live.values_mut().zip(bl.max_live.values()) {
                *ml = max(*ml, *bml);
            }
        }

        l
    }
}

impl Liveness for NextUseLiveness {
    type PerBlock = NextUseBlockLiveness;

    fn block_live(&self, id: u32) -> &NextUseBlockLiveness {
        self.blocks.get(&id).unwrap()
    }
}
