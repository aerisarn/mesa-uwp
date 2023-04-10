/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(unstable_name_collisions)]

use crate::nak_ir::*;
use crate::util::NextMultipleOf;

use std::cmp::max;
use std::ops::Range;

struct RegTracker<T: Clone> {
    reg: [T; 255],
    ureg: [T; 63],
    pred: [T; 6],
    upred: [T; 6],
}

impl<T: Copy> RegTracker<T> {
    pub fn new(v: T) -> Self {
        Self {
            reg: [v; 255],
            ureg: [v; 63],
            pred: [v; 6],
            upred: [v; 6],
        }
    }

    fn get(&self, reg: &RegRef) -> &[T] {
        let range = reg.idx_range();
        let range = Range {
            start: usize::from(range.start),
            end: usize::from(range.end),
        };

        match reg.file() {
            RegFile::GPR => &self.reg[range],
            RegFile::UGPR => &self.ureg[range],
            RegFile::Pred => &self.pred[range],
            RegFile::UPred => &self.upred[range],
        }
    }

    fn get_mut(&mut self, reg: &RegRef) -> &mut [T] {
        let range = reg.idx_range();
        let range = Range {
            start: usize::from(range.start),
            end: usize::from(range.end),
        };

        match reg.file() {
            RegFile::GPR => &mut self.reg[range],
            RegFile::UGPR => &mut self.ureg[range],
            RegFile::Pred => &mut self.pred[range],
            RegFile::UPred => &mut self.upred[range],
        }
    }
}

struct AllocBarriers {
    deps: RegTracker<usize>,
    barriers: Vec<i8>,
    active: u8,
}

impl AllocBarriers {
    pub fn new() -> AllocBarriers {
        AllocBarriers {
            deps: RegTracker::new(usize::MAX),
            barriers: Vec::new(),
            active: 0,
        }
    }

    fn alloc_bar(&mut self) -> i8 {
        let bar = i8::try_from(self.active.trailing_ones()).unwrap();
        assert!(bar < 6);
        self.active |= 1 << bar;
        bar
    }

    fn free_bar_mask(&mut self, bar_mask: u8) {
        assert!(bar_mask < (1 << 7));
        assert!((bar_mask & !self.active) == 0);
        self.active &= !bar_mask;
    }

    fn alloc_dep(&mut self) -> (usize, i8) {
        let bar = self.alloc_bar();
        let dep = self.barriers.len();
        self.barriers.push(bar);
        (dep, bar)
    }

    fn take_reg_barrier_mask(&mut self, reg: &RegRef) -> u8 {
        let mut mask = 0_u8;
        for d in self.deps.get_mut(reg) {
            if *d != usize::MAX && self.barriers[*d] >= 0 {
                let bar = self.barriers[*d];
                self.barriers[*d] = -1;
                mask |= 1_u8 << bar;
            }
        }
        self.free_bar_mask(mask);
        mask
    }

    fn set_reg_dep(&mut self, reg: &RegRef, dep: usize) {
        for d in self.deps.get_mut(reg) {
            *d = dep;
        }
    }

    fn take_instr_read_barrier_mask(&mut self, instr: &Instr) -> u8 {
        let mut bar_mask = 0_u8;
        for src in instr.srcs() {
            if let Some(reg) = src.get_reg() {
                bar_mask |= self.take_reg_barrier_mask(reg);
            }
        }
        bar_mask
    }

    fn set_instr_read_dep(&mut self, instr: &Instr, dep: usize) {
        for src in instr.srcs() {
            if let Some(reg) = src.get_reg() {
                self.set_reg_dep(reg, dep);
            }
        }
    }

    fn take_instr_write_barrier_mask(&mut self, instr: &Instr) -> u8 {
        let mut bar_mask = 0_u8;
        for dst in instr.dsts() {
            if let Some(reg) = dst.as_reg() {
                bar_mask |= self.take_reg_barrier_mask(reg);
            }
        }
        bar_mask
    }

    fn set_instr_write_dep(&mut self, instr: &Instr, dep: usize) {
        for dst in instr.dsts() {
            if let Some(reg) = dst.as_reg() {
                self.set_reg_dep(reg, dep);
            }
        }
    }

    pub fn alloc_barriers(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks.iter_mut() {
                for instr in &mut b.instrs.iter_mut() {
                    /* TODO: Don't barrier read-after-read */
                    let wait = self.take_instr_read_barrier_mask(instr)
                        | self.take_instr_write_barrier_mask(instr);
                    instr.deps.add_wt_bar_mask(wait);

                    if instr.get_latency().is_some() {
                        continue;
                    }

                    if !instr.srcs().is_empty() {
                        let (dep, bar) = self.alloc_dep();
                        instr.deps.set_rd_bar(bar.try_into().unwrap());
                        self.set_instr_read_dep(instr, dep);
                    }
                    if !instr.dsts().is_empty() {
                        let (dep, bar) = self.alloc_dep();
                        instr.deps.set_wr_bar(bar.try_into().unwrap());
                        self.set_instr_write_dep(instr, dep);
                    }
                }
            }
        }
    }
}

struct CalcDelay {
    cycle: u32,
    ready: RegTracker<u32>,
}

impl CalcDelay {
    pub fn new() -> CalcDelay {
        CalcDelay {
            cycle: 0,
            ready: RegTracker::new(0),
        }
    }

    fn set_reg_ready(&mut self, reg: &RegRef, ready: u32) {
        for r in self.ready.get_mut(reg) {
            assert!(*r <= ready);
            *r = ready;
        }
    }

    fn reg_ready(&self, reg: &RegRef) -> u32 {
        *self.ready.get(reg).iter().max().unwrap_or(&0_u32)
    }

    fn instr_dsts_ready(&self, instr: &Instr) -> u32 {
        instr
            .dsts()
            .iter()
            .map(|dst| match dst {
                Dst::None => 0,
                Dst::Reg(reg) => self.reg_ready(reg),
                _ => panic!("Should be run after RA"),
            })
            .max()
            .unwrap_or(0)
    }

    fn set_instr_ready(&mut self, instr: &Instr, ready: u32) {
        for src in instr.srcs() {
            if let Some(reg) = src.get_reg() {
                self.set_reg_ready(reg, ready);
            }
        }
    }

    fn calc_instr_delay(&mut self, instr: &mut Instr) {
        let mut ready = self.cycle + 1; /* TODO: co-issue */
        if let Some(latency) = instr.get_latency() {
            ready = max(ready, self.instr_dsts_ready(instr) + latency);
        }

        self.set_instr_ready(instr, ready);

        let delay = ready - self.cycle;
        let delay = delay.clamp(MIN_INSTR_DELAY.into(), MAX_INSTR_DELAY.into());
        instr.deps.set_delay(u8::try_from(delay).unwrap());

        self.cycle = ready;
    }

    pub fn calc_delay(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks.iter_mut().rev() {
                for instr in &mut b.instrs.iter_mut().rev() {
                    self.calc_instr_delay(instr);
                }
            }
        }
    }
}

impl Shader {
    pub fn calc_instr_deps(&mut self) {
        AllocBarriers::new().alloc_barriers(self);
        CalcDelay::new().calc_delay(self);
    }
}
