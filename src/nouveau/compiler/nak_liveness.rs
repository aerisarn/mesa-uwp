/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::bitset::BitSet;
use crate::nak_ir::*;

use std::cell::RefCell;
use std::cmp::max;
use std::collections::{hash_set, HashMap, HashSet};

struct LiveSet {
    live: PerRegFile<u32>,
    set: HashSet<SSAValue>,
}

impl LiveSet {
    pub fn new() -> LiveSet {
        LiveSet {
            live: Default::default(),
            set: HashSet::new(),
        }
    }

    pub fn contains(&self, ssa: &SSAValue) -> bool {
        self.set.contains(ssa)
    }

    pub fn count(&self, file: RegFile) -> u32 {
        self.live[file]
    }

    pub fn counts(&self) -> &PerRegFile<u32> {
        &self.live
    }

    pub fn insert(&mut self, ssa: SSAValue) -> bool {
        if self.set.insert(ssa) {
            self.live[ssa.file()] += 1;
            true
        } else {
            false
        }
    }

    pub fn iter(&self) -> hash_set::Iter<SSAValue> {
        self.set.iter()
    }

    pub fn remove(&mut self, ssa: &SSAValue) -> bool {
        if self.set.remove(ssa) {
            self.live[ssa.file()] -= 1;
            true
        } else {
            false
        }
    }
}

pub trait BlockLiveness {
    fn is_live_after_ip(&self, val: &SSAValue, ip: usize) -> bool;
    fn is_live_in(&self, val: &SSAValue) -> bool;
    fn is_live_out(&self, val: &SSAValue) -> bool;
}

pub trait Liveness {
    type PerBlock: BlockLiveness;

    fn block_live(&self, id: u32) -> &Self::PerBlock;

    fn calc_max_live(&self, f: &Function) -> PerRegFile<u32> {
        let mut max_live: PerRegFile<u32> = Default::default();
        let mut block_live_out: HashMap<u32, LiveSet> = HashMap::new();

        for (bb_idx, bb) in f.blocks.iter().enumerate() {
            let bl = self.block_live(bb.id);

            let mut live = LiveSet::new();

            /* Predecessors are added block order so we can just grab the first
             * one (if any) and it will be a block we've processed.
             */
            if let Some(pred_idx) = f.blocks.pred_indices(bb_idx).first() {
                let pred_id = f.blocks[*pred_idx].id;
                let pred_out = block_live_out.get(&pred_id).unwrap();
                for ssa in pred_out.iter() {
                    if bl.is_live_in(ssa) {
                        live.insert(*ssa);
                    }
                }
            }

            for (ip, instr) in bb.instrs.iter().enumerate() {
                /* Vector destinations go live before sources are killed.  Even
                 * in the case where the destination is immediately killed, it
                 * still may contribute to pressure temporarily.
                 */
                for dst in instr.dsts() {
                    if let Dst::SSA(vec) = dst {
                        if vec.comps() > 1 {
                            for ssa in vec.iter() {
                                live.insert(*ssa);
                            }
                        }
                    }
                }

                for (ml, l) in max_live.values_mut().zip(live.counts().values())
                {
                    *ml = max(*ml, *l);
                }

                instr.for_each_ssa_use(|ssa| {
                    if !bl.is_live_after_ip(ssa, ip) {
                        live.remove(ssa);
                    }
                });

                /* Scalar destinations are allocated last */
                for dst in instr.dsts() {
                    if let Dst::SSA(vec) = dst {
                        if vec.comps() == 1 {
                            live.insert(vec[0]);
                        }
                    }
                }

                for (ml, l) in max_live.values_mut().zip(live.counts().values())
                {
                    *ml = max(*ml, *l);
                }

                /* We already added destinations to the live count but haven't
                 * inserted them into the live set yet.  If a destination is
                 * killed immediately, subtract from the count, otherwise add to
                 * the set.
                 */
                instr.for_each_ssa_def(|ssa| {
                    debug_assert!(live.contains(ssa));
                    if !bl.is_live_after_ip(ssa, ip) {
                        live.remove(ssa);
                    }
                });
            }

            block_live_out.insert(bb.id, live);
        }

        max_live
    }
}

pub struct SimpleBlockLiveness {
    defs: BitSet,
    uses: BitSet,
    last_use: HashMap<u32, usize>,
    live_in: BitSet,
    live_out: BitSet,
}

impl SimpleBlockLiveness {
    fn add_def(&mut self, val: &SSAValue) {
        self.defs.insert(val.idx().try_into().unwrap());
    }

    fn add_use(&mut self, val: &SSAValue, ip: usize) {
        self.uses.insert(val.idx().try_into().unwrap());
        self.last_use.insert(val.idx(), ip);
    }

    fn for_block(block: &BasicBlock) -> Self {
        let mut bl = Self {
            defs: BitSet::new(),
            uses: BitSet::new(),
            last_use: HashMap::new(),
            live_in: BitSet::new(),
            live_out: BitSet::new(),
        };

        for (ip, instr) in block.instrs.iter().enumerate() {
            instr.for_each_ssa_use(|ssa| {
                bl.add_use(ssa, ip);
            });
            instr.for_each_ssa_def(|ssa| {
                bl.add_def(ssa);
            });
        }

        bl
    }
}

impl BlockLiveness for SimpleBlockLiveness {
    fn is_live_after_ip(&self, val: &SSAValue, ip: usize) -> bool {
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

    fn is_live_in(&self, val: &SSAValue) -> bool {
        self.live_in.get(val.idx().try_into().unwrap())
    }

    fn is_live_out(&self, val: &SSAValue) -> bool {
        self.live_out.get(val.idx().try_into().unwrap())
    }
}

pub struct SimpleLiveness {
    blocks: HashMap<u32, SimpleBlockLiveness>,
}

impl SimpleLiveness {
    pub fn for_function(func: &Function) -> SimpleLiveness {
        let mut l = SimpleLiveness {
            blocks: HashMap::new(),
        };
        let mut live_in = HashMap::new();

        for b in func.blocks.iter() {
            let bl = SimpleBlockLiveness::for_block(b);
            l.blocks.insert(b.id, bl);
            live_in.insert(b.id, BitSet::new());
        }

        let mut to_do = true;
        while to_do {
            to_do = false;
            for (b_idx, b) in func.blocks.iter().enumerate().rev() {
                let bl = l.blocks.get_mut(&b.id).unwrap();

                /* Compute live-out */
                for sb_idx in func.blocks.succ_indices(b_idx) {
                    let sb_id = func.blocks[*sb_idx].id;
                    let s_live_in = live_in.get(&sb_id).unwrap();
                    to_do |= bl.live_out.union_with(s_live_in);
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

impl Liveness for SimpleLiveness {
    type PerBlock = SimpleBlockLiveness;

    fn block_live(&self, id: u32) -> &SimpleBlockLiveness {
        self.blocks.get(&id).unwrap()
    }
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
        };

        for (ip, instr) in block.instrs.iter().enumerate() {
            instr.for_each_ssa_use(|ssa| {
                bl.entry_mut(*ssa).add_in_block_use(ip);
            });

            instr.for_each_ssa_def(|ssa| {
                bl.entry_mut(*ssa).add_def();
            });
        }

        bl
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
}

impl NextUseLiveness {
    pub fn for_function(func: &Function) -> NextUseLiveness {
        let mut blocks = HashMap::new();
        for b in &func.blocks {
            let bl = NextUseBlockLiveness::for_block(b);
            blocks.insert(b.id, RefCell::new(bl));
        }

        let mut to_do = true;
        while to_do {
            to_do = false;
            for (b_idx, b) in func.blocks.iter().enumerate().rev() {
                let num_instrs = b.instrs.len();
                let mut bl = blocks.get(&b.id).unwrap().borrow_mut();

                /* Compute live-out */
                for sb_idx in func.blocks.succ_indices(b_idx) {
                    let sb_id = func.blocks[*sb_idx].id;
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

        NextUseLiveness {
            blocks: HashMap::from_iter(
                blocks.drain().map(|(k, v)| (k, v.into_inner())),
            ),
        }
    }
}

impl Liveness for NextUseLiveness {
    type PerBlock = NextUseBlockLiveness;

    fn block_live(&self, id: u32) -> &NextUseBlockLiveness {
        self.blocks.get(&id).unwrap()
    }
}
