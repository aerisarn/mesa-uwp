// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::nak_ir::*;
use crate::{GetDebugFlags, DEBUG};

use std::cmp::max;
use std::collections::HashSet;
use std::ops::{Index, IndexMut, Range};

struct RegTracker<T> {
    reg: [T; 255],
    ureg: [T; 63],
    pred: [T; 7],
    upred: [T; 7],
}

impl<T: Copy> RegTracker<T> {
    pub fn new(v: T) -> Self {
        Self {
            reg: [v; 255],
            ureg: [v; 63],
            pred: [v; 7],
            upred: [v; 7],
        }
    }

    pub fn for_each_instr_src_mut(
        &mut self,
        instr: &Instr,
        mut f: impl FnMut(&mut T),
    ) {
        if let PredRef::Reg(reg) = &instr.pred.pred_ref {
            for i in &mut self[*reg] {
                f(i);
            }
        }
        for src in instr.srcs() {
            if let SrcRef::Reg(reg) = &src.src_ref {
                for i in &mut self[*reg] {
                    f(i);
                }
            }
        }
    }

    pub fn for_each_instr_dst_mut(
        &mut self,
        instr: &Instr,
        mut f: impl FnMut(&mut T),
    ) {
        for dst in instr.dsts() {
            if let Dst::Reg(reg) = dst {
                for i in &mut self[*reg] {
                    f(i);
                }
            }
        }
    }
}

impl<T> Index<RegRef> for RegTracker<T> {
    type Output = [T];

    fn index(&self, reg: RegRef) -> &[T] {
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
            RegFile::Mem => panic!("Not a register"),
        }
    }
}

impl<T> IndexMut<RegRef> for RegTracker<T> {
    fn index_mut(&mut self, reg: RegRef) -> &mut [T] {
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
            RegFile::Mem => panic!("Not a register"),
        }
    }
}

struct BarDep {
    dep: usize,
    bar: u8,
}

struct BarAlloc {
    num_bars: u8,
    wr_bars: u8,
    bar_dep: [usize; 6],
    dep_bar: Vec<u8>,
}

impl BarAlloc {
    pub fn new() -> BarAlloc {
        BarAlloc {
            num_bars: 6,
            wr_bars: 0,
            bar_dep: [usize::MAX; 6],
            dep_bar: Vec::new(),
        }
    }

    pub fn bar_is_free(&self, bar: u8) -> bool {
        assert!(bar < self.num_bars);
        self.bar_dep[usize::from(bar)] == usize::MAX
    }

    pub fn is_wr_bar(&self, bar: u8) -> bool {
        assert!(!self.bar_is_free(bar));
        self.wr_bars & (1 << bar) != 0
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

    pub fn free_bar(&mut self, bar: u8) {
        assert!(bar < self.num_bars);
        let dep = self.bar_dep[usize::from(bar)];
        assert!(dep != usize::MAX);

        self.wr_bars &= !(1 << bar);
        self.bar_dep[usize::from(bar)] = usize::MAX;
        self.dep_bar[dep] = u8::MAX;
    }

    pub fn free_some_bar(&mut self) -> u8 {
        // Get the oldest by looking for the one with the smallest dep
        let mut bar = 0;
        for b in 1..self.num_bars {
            if self.bar_dep[usize::from(b)] < self.bar_dep[usize::from(bar)] {
                bar = b;
            }
        }
        self.free_bar(bar);
        bar
    }

    pub fn alloc_dep_for_bar(&mut self, bar: u8, is_write: bool) -> BarDep {
        assert!(self.bar_is_free(bar));
        assert!(self.wr_bars & (1 << bar) == 0);

        let dep = self.dep_bar.len();
        self.bar_dep[usize::from(bar)] = dep;
        self.dep_bar.push(bar);
        if is_write {
            self.wr_bars |= 1 << bar;
        }
        BarDep { dep: dep, bar: bar }
    }

    pub fn try_alloc_bar_dep(&mut self, is_write: bool) -> Option<BarDep> {
        for bar in 0..self.num_bars {
            if self.bar_is_free(bar) {
                return Some(self.alloc_dep_for_bar(bar, is_write));
            }
        }
        None
    }
}

fn assign_barriers(f: &mut Function) {
    let mut deps = RegTracker::new(usize::MAX);
    let mut bars = BarAlloc::new();

    for b in f.blocks.iter_mut() {
        for instr in b.instrs.iter_mut() {
            let mut wait_mask = 0_u8;
            if instr.is_branch() {
                // For branch instructions, we grab everything
                for bar in 0..bars.num_bars {
                    if !bars.bar_is_free(bar) {
                        wait_mask |= 1 << bar;
                    }
                }
            } else {
                deps.for_each_instr_src_mut(instr, |dep| {
                    if *dep != usize::MAX {
                        if let Some(bar) = bars.get_bar(*dep) {
                            // We don't care about RaR deps
                            if bars.is_wr_bar(bar) {
                                wait_mask |= 1 << bar;
                            }
                        }
                    }
                });
                deps.for_each_instr_dst_mut(instr, |dep| {
                    if *dep != usize::MAX {
                        if let Some(bar) = bars.get_bar(*dep) {
                            wait_mask |= 1 << bar;
                        }
                    }
                });
            }

            instr.deps.add_wt_bar_mask(wait_mask);

            // Free any barriers we just waited on
            while wait_mask != 0 {
                let bar: u8 = wait_mask.trailing_zeros().try_into().unwrap();
                bars.free_bar(bar);
                wait_mask &= !(1 << bar);
            }

            if instr.has_fixed_latency() {
                continue;
            }

            // Gather sources and destinations into a hash sets
            let mut srcs = HashSet::new();
            if let PredRef::Reg(reg) = &instr.pred.pred_ref {
                for c in 0..reg.comps() {
                    srcs.insert(reg.comp(c));
                }
            }
            for src in instr.srcs() {
                if let SrcRef::Reg(reg) = &src.src_ref {
                    for c in 0..reg.comps() {
                        srcs.insert(reg.comp(c));
                    }
                }
            }

            let mut dsts = HashSet::new();
            for dst in instr.dsts() {
                if let Dst::Reg(reg) = dst {
                    for c in 0..reg.comps() {
                        dsts.insert(reg.comp(c));
                        // Remove any sources which this instruction overwrites.
                        // We are going to set a write barrier for those so
                        // there's no point in also setting a read barrier for
                        // them.
                        srcs.remove(&reg.comp(c));
                    }
                }
            }

            // Note: It's okay to use hash sets for sources and destinations
            // because we allocate a single barrier for the entire set of
            // sources or destinations so the order of the set doesn't matter.

            if !srcs.is_empty() {
                let bd = bars.try_alloc_bar_dep(false).unwrap_or_else(|| {
                    let bar = bars.free_some_bar();
                    instr.deps.add_wt_bar(bar);
                    bars.alloc_dep_for_bar(bar, false)
                });
                instr.deps.set_rd_bar(bd.bar);
                for src in srcs {
                    deps[src][0] = bd.dep;
                }
            }

            if !dsts.is_empty() {
                let bd = bars.try_alloc_bar_dep(true).unwrap_or_else(|| {
                    let bar = bars.free_some_bar();
                    instr.deps.add_wt_bar(bar);
                    bars.alloc_dep_for_bar(bar, true)
                });
                instr.deps.set_wr_bar(bd.bar);
                for dst in dsts {
                    deps[dst][0] = bd.dep;
                }
            }
        }
    }
}

fn calc_delays(f: &mut Function) {
    for b in f.blocks.iter_mut().rev() {
        let mut cycle = 0_u32;
        let mut ready = RegTracker::new(0_u32);
        for instr in b.instrs.iter_mut().rev() {
            let mut min_start = cycle + 1; /* TODO: co-issue */
            if instr.has_fixed_latency() {
                for (idx, dst) in instr.dsts().iter().enumerate() {
                    if let Dst::Reg(reg) = dst {
                        let latency = instr.get_dst_latency(idx);
                        for c in &ready[*reg] {
                            min_start = max(min_start, *c + latency);
                        }
                    }
                }
            }

            let delay = min_start - cycle;
            let delay = delay
                .clamp(MIN_INSTR_DELAY.into(), MAX_INSTR_DELAY.into())
                .try_into()
                .unwrap();
            instr.deps.set_delay(delay);

            ready.for_each_instr_src_mut(instr, |c| *c = min_start);
            cycle = min_start;
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
            for f in &mut self.functions {
                assign_barriers(f);
                calc_delays(f);
            }
        }
    }
}
