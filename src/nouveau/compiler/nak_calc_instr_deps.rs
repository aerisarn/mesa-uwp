/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;
use crate::{GetDebugFlags, DEBUG};

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
            start: usize::try_from(range.start).unwrap(),
            end: usize::try_from(range.end).unwrap(),
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
            start: usize::try_from(range.start).unwrap(),
            end: usize::try_from(range.end).unwrap(),
        };

        match reg.file() {
            RegFile::GPR => &mut self.reg[range],
            RegFile::UGPR => &mut self.ureg[range],
            RegFile::Pred => &mut self.pred[range],
            RegFile::UPred => &mut self.upred[range],
        }
    }
}

struct BarDep {
    dep: usize,
    bar: u8,
}

struct BarAlloc {
    bars: u8,
    bar_dep: [usize; 6],
    dep_bar: Vec<u8>,
}

impl BarAlloc {
    pub fn new() -> BarAlloc {
        BarAlloc {
            bars: 6,
            bar_dep: [usize::MAX; 6],
            dep_bar: Vec::new(),
        }
    }

    pub fn get_dep(&self, bar: u8) -> Option<usize> {
        assert!(bar < self.bars);
        let dep = self.bar_dep[usize::from(bar)];
        if dep == usize::MAX {
            None
        } else {
            assert!(self.dep_bar[dep] == bar);
            Some(dep)
        }
    }

    pub fn get_bar(&self, dep: usize) -> Option<u8> {
        let bar = self.dep_bar[dep];
        if bar == u8::MAX {
            None
        } else {
            assert!(self.bar_dep[usize::from(bar)] == dep);
            Some(bar)
        }
    }

    fn alloc_dep(&mut self, bar: u8) -> BarDep {
        let dep = self.dep_bar.len();
        self.bar_dep[usize::from(bar)] = dep;
        self.dep_bar.push(bar);
        BarDep { dep: dep, bar: bar }
    }

    pub fn try_alloc(&mut self) -> Option<BarDep> {
        for bar in 0..self.bars {
            if self.bar_dep[usize::from(bar)] == usize::MAX {
                return Some(self.alloc_dep(bar));
            }
        }
        None
    }

    pub fn alloc(&mut self) -> (BarDep, bool) {
        if let Some(bd) = self.try_alloc() {
            (bd, false)
        } else {
            /* Get the oldest by looking for the one with the smallest dep */
            let mut old_bar = 0;
            let mut old_dep = self.bar_dep[0];
            for bar in 1..self.bars {
                let dep = self.bar_dep[usize::from(bar)];
                if dep < old_dep {
                    old_bar = bar;
                    old_dep = dep;
                }
            }
            self.free_bar(old_bar);
            let bd = self.alloc_dep(old_bar);
            (bd, true)
        }
    }

    pub fn free_bar(&mut self, bar: u8) {
        let dep = self.get_dep(bar).unwrap();
        self.bar_dep[usize::from(bar)] = usize::MAX;
        self.dep_bar[dep] = u8::MAX;
    }
}

struct AllocBarriers {
    deps: RegTracker<usize>,
    bars: BarAlloc,
}

impl AllocBarriers {
    pub fn new() -> AllocBarriers {
        AllocBarriers {
            deps: RegTracker::new(usize::MAX),
            bars: BarAlloc::new(),
        }
    }

    fn take_reg_barrier_mask(&mut self, reg: &RegRef) -> u8 {
        let mut mask = 0_u8;
        for d in self.deps.get_mut(reg) {
            if *d != usize::MAX {
                if let Some(bar) = self.bars.get_bar(*d) {
                    self.bars.free_bar(bar);
                    mask |= 1_u8 << bar;
                }
            }
        }
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
                        let (bd, stall) = self.bars.alloc();
                        if stall {
                            instr.deps.add_wt_bar(bd.bar);
                        }
                        instr.deps.set_rd_bar(bd.bar);
                        self.set_instr_read_dep(instr, bd.dep);
                    }
                    if !instr.dsts().is_empty() {
                        let (bd, stall) = self.bars.alloc();
                        if stall {
                            instr.deps.add_wt_bar(bd.bar);
                        }
                        instr.deps.set_wr_bar(bd.bar);
                        self.set_instr_write_dep(instr, bd.dep);
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
    pub fn assign_deps_serial(&mut self) {
        for f in &mut self.functions {
            for b in &mut f.blocks.iter_mut().rev() {
                let mut wt = 0_u8;
                for instr in &mut b.instrs {
                    if instr.is_barrier() {
                        instr.deps.set_yield(true);
                    } else if instr.is_branch() {
                        instr.deps.add_wt_bar_mask(0x3f);
                    } else {
                        instr.deps.add_wt_bar_mask(wt);
                        if instr.dsts().len() > 0 {
                            instr.deps.set_wr_bar(0);
                            wt |= 1 << 0;
                        }
                        if !instr.pred.pred_ref.is_none()
                            || instr.srcs().len() > 0
                        {
                            instr.deps.set_rd_bar(1);
                            wt |= 1 << 1;
                        }
                    }
                }
            }
        }
    }

    pub fn calc_instr_deps(&mut self) {
        if DEBUG.serial() {
            self.assign_deps_serial();
        } else {
            AllocBarriers::new().alloc_barriers(self);
            CalcDelay::new().calc_delay(self);
        }
    }
}
