/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(unstable_name_collisions)]

use crate::bitset::BitSet;
use crate::nak_cfg::CFG;
use crate::nak_ir::*;
use crate::nak_liveness::{BlockLiveness, Liveness, SimpleLiveness};
use crate::util::NextMultipleOf;

use std::cmp::{max, Ordering};
use std::collections::{HashMap, HashSet};

struct KillSet {
    set: HashSet<SSAValue>,
    vec: Vec<SSAValue>,
}

impl KillSet {
    pub fn new() -> KillSet {
        KillSet {
            set: HashSet::new(),
            vec: Vec::new(),
        }
    }

    pub fn clear(&mut self) {
        self.set.clear();
        self.vec.clear();
    }

    pub fn insert(&mut self, ssa: SSAValue) {
        if self.set.insert(ssa) {
            self.vec.push(ssa);
        }
    }

    pub fn iter(&self) -> std::slice::Iter<'_, SSAValue> {
        self.vec.iter()
    }

    pub fn is_empty(&self) -> bool {
        self.vec.is_empty()
    }
}

enum SSAUse {
    FixedReg(u8),
    Vec(SSARef),
}

struct SSAUseMap {
    ssa_map: HashMap<SSAValue, Vec<(usize, SSAUse)>>,
}

impl SSAUseMap {
    fn add_fixed_reg_use(&mut self, ip: usize, ssa: SSAValue, reg: u8) {
        let v = self.ssa_map.entry(ssa).or_insert_with(|| Vec::new());
        v.push((ip, SSAUse::FixedReg(reg)));
    }

    fn add_vec_use(&mut self, ip: usize, vec: SSARef) {
        if vec.comps() == 1 {
            return;
        }

        for ssa in vec.iter() {
            let v = self.ssa_map.entry(*ssa).or_insert_with(|| Vec::new());
            v.push((ip, SSAUse::Vec(vec)));
        }
    }

    fn find_vec_use_after(&self, ssa: SSAValue, ip: usize) -> Option<&SSAUse> {
        if let Some(v) = self.ssa_map.get(&ssa) {
            let p = v.partition_point(|(uip, _)| *uip <= ip);
            if p == v.len() {
                None
            } else {
                let (_, u) = &v[p];
                Some(u)
            }
        } else {
            None
        }
    }

    pub fn add_block(&mut self, b: &BasicBlock) {
        for (ip, instr) in b.instrs.iter().enumerate() {
            match &instr.op {
                Op::FSOut(op) => {
                    for (i, src) in op.srcs.iter().enumerate() {
                        let out_reg = u8::try_from(i).unwrap();
                        if let SrcRef::SSA(ssa) = src.src_ref {
                            assert!(ssa.comps() == 1);
                            self.add_fixed_reg_use(ip, ssa[0], out_reg);
                        }
                    }
                }
                _ => {
                    /* We don't care about predicates because they're scalar */
                    for src in instr.srcs() {
                        if let SrcRef::SSA(ssa) = src.src_ref {
                            self.add_vec_use(ip, ssa);
                        }
                    }
                }
            }
        }
    }

    pub fn for_block(b: &BasicBlock) -> SSAUseMap {
        let mut am = SSAUseMap {
            ssa_map: HashMap::new(),
        };
        am.add_block(b);
        am
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
enum LiveRef {
    SSA(SSAValue),
    Phi(u32),
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
struct LiveValue {
    pub live_ref: LiveRef,
    pub reg_ref: RegRef,
}

/* We need a stable ordering of live values so that RA is deterministic */
impl Ord for LiveValue {
    fn cmp(&self, other: &Self) -> Ordering {
        let s_file = u8::from(self.reg_ref.file());
        let o_file = u8::from(other.reg_ref.file());
        match s_file.cmp(&o_file) {
            Ordering::Equal => {
                let s_idx = self.reg_ref.base_idx();
                let o_idx = other.reg_ref.base_idx();
                s_idx.cmp(&o_idx)
            }
            ord => ord,
        }
    }
}

impl PartialOrd for LiveValue {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[derive(Clone)]
struct RegAllocator {
    file: RegFile,
    num_regs: u8,
    used: BitSet,
    reg_ssa: Vec<SSAValue>,
    ssa_reg: HashMap<SSAValue, u8>,
}

impl RegAllocator {
    pub fn new(file: RegFile, num_regs: u8) -> Self {
        Self {
            file: file,
            num_regs: num_regs,
            used: BitSet::new(),
            reg_ssa: Vec::new(),
            ssa_reg: HashMap::new(),
        }
    }

    fn file(&self) -> RegFile {
        self.file
    }

    fn reg_is_used(&self, reg: u8) -> bool {
        self.used.get(reg.into())
    }

    fn reg_range_is_unused(&self, reg: u8, comps: u8) -> bool {
        for i in 0..comps {
            if self.reg_is_used(reg + i) {
                return false;
            }
        }
        true
    }

    pub fn try_get_reg(&self, ssa: SSAValue) -> Option<u8> {
        self.ssa_reg.get(&ssa).cloned()
    }

    pub fn try_get_ssa(&self, reg: u8) -> Option<SSAValue> {
        if self.reg_is_used(reg) {
            Some(self.reg_ssa[usize::from(reg)])
        } else {
            None
        }
    }

    pub fn try_get_vec_reg(&self, vec: &SSARef) -> Option<u8> {
        let Some(reg) = self.try_get_reg(vec[0]) else {
            return None;
        };

        let align = vec.comps().next_power_of_two();
        if reg % align != 0 {
            return None;
        }

        for i in 1..vec.comps() {
            if self.try_get_reg(vec[usize::from(i)]) != Some(reg + i) {
                return None;
            }
        }
        Some(reg)
    }

    pub fn free_ssa(&mut self, ssa: SSAValue) -> u8 {
        assert!(ssa.file() == self.file);
        let reg = self.ssa_reg.remove(&ssa).unwrap();
        assert!(self.reg_is_used(reg));
        assert!(self.reg_ssa[usize::from(reg)] == ssa);
        self.used.remove(reg.into());
        reg
    }

    pub fn assign_reg(&mut self, ssa: SSAValue, reg: u8) -> RegRef {
        assert!(ssa.file() == self.file);
        assert!(reg < self.num_regs);
        assert!(!self.reg_is_used(reg));

        if usize::from(reg) >= self.reg_ssa.len() {
            self.reg_ssa.resize(usize::from(reg) + 1, SSAValue::NONE);
        }
        self.reg_ssa[usize::from(reg)] = ssa;
        let old = self.ssa_reg.insert(ssa, reg);
        assert!(old.is_none());
        self.used.insert(reg.into());

        RegRef::new(self.file, reg, 1)
    }

    pub fn try_find_unused_reg_range(
        &self,
        start_reg: u8,
        align: u8,
        comps: u8,
    ) -> Option<u8> {
        assert!(comps > 0);
        let align = usize::from(align);

        let mut next_reg = usize::from(start_reg);
        loop {
            let reg = self.used.next_unset(next_reg.into());

            /* Ensure we're properly aligned */
            let reg = reg.next_multiple_of(align);

            /* Ensure we're in-bounds. This also serves as a check to ensure
             * that u8::try_from(reg + i) will succeed.
             */
            if reg > usize::from(self.num_regs - comps) {
                return None;
            }

            let reg_u8 = u8::try_from(reg).unwrap();
            if self.reg_range_is_unused(reg_u8, comps) {
                return Some(reg_u8);
            }

            next_reg = reg + align;
        }
    }

    pub fn get_scalar(&self, ssa: SSAValue) -> RegRef {
        let reg = self.try_get_reg(ssa).expect("Unknown SSA value");
        RegRef::new(self.file, reg, 1)
    }

    pub fn alloc_scalar(
        &mut self,
        ip: usize,
        sum: &SSAUseMap,
        ssa: SSAValue,
    ) -> RegRef {
        if let Some(u) = sum.find_vec_use_after(ssa, ip) {
            match u {
                SSAUse::FixedReg(reg) => {
                    if !self.used.get((*reg).into()) {
                        return self.assign_reg(ssa, *reg);
                    }
                }
                SSAUse::Vec(vec) => {
                    let mut comp = u8::MAX;
                    for c in 0..vec.comps() {
                        if vec[usize::from(c)] == ssa {
                            comp = c;
                            break;
                        }
                    }
                    assert!(comp < vec.comps());

                    let align = vec.comps().next_power_of_two();
                    for c in 0..vec.comps() {
                        if c == comp {
                            continue;
                        }

                        let other = vec[usize::from(c)];
                        if let Some(other_reg) = self.try_get_reg(other) {
                            let vec_reg = other_reg & !(align - 1);
                            if other_reg != vec_reg + c {
                                continue;
                            }

                            if vec_reg + comp >= self.num_regs {
                                continue;
                            }

                            if !self.used.get((vec_reg + comp).into()) {
                                return self.assign_reg(ssa, vec_reg + comp);
                            }
                        }
                    }

                    /* We weren't able to pair it with an already allocated
                     * register but maybe we can at least find an aligned one.
                     */
                    if let Some(reg) =
                        self.try_find_unused_reg_range(0, align, 1)
                    {
                        return self.assign_reg(ssa, reg);
                    }
                }
            }
        }

        let reg = self
            .try_find_unused_reg_range(0, 1, 1)
            .expect("Failed to find free register");
        self.assign_reg(ssa, reg)
    }
}

struct PinnedRegAllocator<'a> {
    ra: &'a mut RegAllocator,
    pcopy: OpParCopy,
    pinned: BitSet,
    evicted: HashMap<SSAValue, u8>,
}

impl<'a> PinnedRegAllocator<'a> {
    fn new(ra: &'a mut RegAllocator) -> Self {
        PinnedRegAllocator {
            ra: ra,
            pcopy: OpParCopy::new(),
            pinned: Default::default(),
            evicted: HashMap::new(),
        }
    }

    fn file(&self) -> RegFile {
        self.ra.file()
    }

    fn pin_reg(&mut self, reg: u8) {
        self.pinned.insert(reg.into());
    }

    fn pin_reg_range(&mut self, reg: u8, comps: u8) {
        for i in 0..comps {
            self.pin_reg(reg + i);
        }
    }

    fn reg_is_pinned(&self, reg: u8) -> bool {
        self.pinned.get(reg.into())
    }

    fn reg_range_is_unpinned(&self, reg: u8, comps: u8) -> bool {
        for i in 0..comps {
            if self.pinned.get((reg + i).into()) {
                return false;
            }
        }
        true
    }

    fn assign_pin_reg(&mut self, ssa: SSAValue, reg: u8) -> RegRef {
        self.pin_reg(reg);
        self.ra.assign_reg(ssa, reg)
    }

    pub fn assign_pin_vec_reg(&mut self, ssa: SSARef, reg: u8) -> RegRef {
        for c in 0..ssa.comps() {
            self.assign_pin_reg(ssa[usize::from(c)], reg + c);
        }
        RegRef::new(self.file(), reg, ssa.comps())
    }

    fn try_find_unpinned_reg_range(
        &self,
        start_reg: u8,
        align: u8,
        comps: u8,
    ) -> Option<u8> {
        let align = usize::from(align);

        let mut next_reg = usize::from(start_reg);
        loop {
            let reg = self.pinned.next_unset(next_reg);

            /* Ensure we're properly aligned */
            let reg = reg.next_multiple_of(align);

            /* Ensure we're in-bounds. This also serves as a check to ensure
             * that u8::try_from(reg + i) will succeed.
             */
            if reg > usize::from(self.ra.num_regs - comps) {
                return None;
            }

            let reg_u8 = u8::try_from(reg).unwrap();
            if self.reg_range_is_unpinned(reg_u8, comps) {
                return Some(reg_u8);
            }

            next_reg = reg + align;
        }
    }

    pub fn evict_ssa(&mut self, ssa: SSAValue, old_reg: u8) {
        assert!(ssa.file() == self.file());
        assert!(!self.reg_is_pinned(old_reg));
        self.evicted.insert(ssa, old_reg);
    }

    pub fn evict_reg_if_used(&mut self, reg: u8) {
        assert!(!self.reg_is_pinned(reg));

        if let Some(ssa) = self.ra.try_get_ssa(reg) {
            self.ra.free_ssa(ssa);
            self.evict_ssa(ssa, reg);
        }
    }

    fn move_ssa_to_reg(&mut self, ssa: SSAValue, new_reg: u8) {
        if let Some(old_reg) = self.ra.try_get_reg(ssa) {
            assert!(self.evicted.get(&ssa).is_none());
            assert!(!self.reg_is_pinned(old_reg));

            if new_reg == old_reg {
                self.pin_reg(new_reg);
            } else {
                self.ra.free_ssa(ssa);
                self.evict_reg_if_used(new_reg);

                self.pcopy.push(
                    RegRef::new(self.file(), new_reg, 1).into(),
                    RegRef::new(self.file(), old_reg, 1).into(),
                );

                self.assign_pin_reg(ssa, new_reg);
            }
        } else if let Some(old_reg) = self.evicted.remove(&ssa) {
            self.evict_reg_if_used(new_reg);

            self.pcopy.push(
                RegRef::new(self.file(), new_reg, 1).into(),
                RegRef::new(self.file(), old_reg, 1).into(),
            );

            self.assign_pin_reg(ssa, new_reg);
        } else {
            panic!("Unknown SSA value");
        }
    }

    fn finish(mut self, pcopy: &mut OpParCopy) {
        pcopy.srcs.append(&mut self.pcopy.srcs);
        pcopy.dsts.append(&mut self.pcopy.dsts);

        if !self.evicted.is_empty() {
            /* Sort so we get determinism, even if the hash map order changes
             * from one run to another or due to rust compiler updates.
             */
            let mut evicted: Vec<_> = self.evicted.drain().collect();
            evicted.sort_by_key(|(_, reg)| *reg);

            for (ssa, old_reg) in evicted {
                let mut next_reg = 0;
                let new_reg = loop {
                    let reg = self
                        .ra
                        .try_find_unused_reg_range(next_reg, 1, 1)
                        .expect("Failed to find free register");
                    if !self.reg_is_pinned(reg) {
                        break reg;
                    }
                    next_reg = reg + 1;
                };

                pcopy.push(
                    RegRef::new(self.file(), new_reg, 1).into(),
                    RegRef::new(self.file(), old_reg, 1).into(),
                );
                self.assign_pin_reg(ssa, new_reg);
            }
        }
    }

    pub fn try_get_vec_reg(&self, vec: &SSARef) -> Option<u8> {
        self.ra.try_get_vec_reg(vec)
    }

    pub fn collect_vector(&mut self, vec: &SSARef) -> RegRef {
        if let Some(reg) = self.try_get_vec_reg(vec) {
            self.pin_reg_range(reg, vec.comps());
            return RegRef::new(self.file(), reg, vec.comps());
        }

        let comps = vec.comps();
        let align = comps.next_power_of_two();

        let reg = self
            .ra
            .try_find_unused_reg_range(0, align, comps)
            .or_else(|| {
                for c in 0..vec.comps() {
                    let ssa = vec[usize::from(c)];
                    let Some(comp_reg) = self.ra.try_get_reg(ssa) else {
                        continue;
                    };

                    let Some(reg) = comp_reg.checked_sub(c) else {
                        continue;
                    };
                    if reg % align != 0 {
                        continue;
                    }

                    if let Some(end) = reg.checked_add(comps) {
                        if end > self.ra.num_regs {
                            continue;
                        }
                    } else {
                        continue;
                    }

                    if self.reg_range_is_unpinned(reg, comps) {
                        return Some(reg);
                    }
                }
                None
            })
            .or_else(|| self.try_find_unpinned_reg_range(0, align, comps))
            .expect("Failed to find an unpinned register range");

        for c in 0..vec.comps() {
            self.move_ssa_to_reg(vec[usize::from(c)], reg + c);
        }

        RegRef::new(self.file(), reg, comps)
    }

    pub fn alloc_vector(&mut self, vec: SSARef) -> RegRef {
        let comps = vec.comps();
        let align = comps.next_power_of_two();

        if let Some(reg) = self.ra.try_find_unused_reg_range(0, align, comps) {
            return self.assign_pin_vec_reg(vec, reg);
        }

        let reg = self
            .try_find_unpinned_reg_range(0, align, comps)
            .expect("Failed to find an unpinned register range");

        for c in 0..vec.comps() {
            self.evict_reg_if_used(reg + c);
        }
        self.assign_pin_vec_reg(vec, reg)
    }

    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            if ssa.file() == self.file() {
                self.ra.free_ssa(*ssa);
            }
        }
    }
}

impl Drop for PinnedRegAllocator<'_> {
    fn drop(&mut self) {
        assert!(self.evicted.is_empty());
    }
}

fn instr_remap_srcs_file(instr: &mut Instr, ra: &mut PinnedRegAllocator) {
    /* Collect vector sources first since those may silently pin some of our
     * scalar sources.
     */
    for src in instr.srcs_mut() {
        if let SrcRef::SSA(ssa) = &src.src_ref {
            if ssa.file() == ra.file() && ssa.comps() > 1 {
                src.src_ref = ra.collect_vector(ssa).into();
            }
        }
    }

    if let PredRef::SSA(pred) = instr.pred.pred_ref {
        if pred.file() == ra.file() {
            instr.pred.pred_ref = ra.collect_vector(&pred.into()).into();
        }
    }

    for src in instr.srcs_mut() {
        if let SrcRef::SSA(ssa) = &src.src_ref {
            if ssa.file() == ra.file() && ssa.comps() == 1 {
                src.src_ref = ra.collect_vector(ssa).into();
            }
        }
    }
}

fn instr_alloc_scalar_dsts_file(
    instr: &mut Instr,
    ip: usize,
    sum: &SSAUseMap,
    ra: &mut RegAllocator,
) {
    for dst in instr.dsts_mut() {
        if let Dst::SSA(ssa) = dst {
            assert!(ssa.comps() == 1);
            if ssa.file() == ra.file() {
                *dst = ra.alloc_scalar(ip, sum, ssa[0]).into();
            }
        }
    }
}

fn instr_assign_regs_file(
    instr: &mut Instr,
    ip: usize,
    sum: &SSAUseMap,
    killed: &KillSet,
    pcopy: &mut OpParCopy,
    ra: &mut RegAllocator,
) {
    struct VecDst {
        dst_idx: usize,
        comps: u8,
        killed: Option<SSARef>,
        reg: u8,
    }

    let mut vec_dsts = Vec::new();
    let mut vec_dst_comps = 0;
    for (i, dst) in instr.dsts().iter().enumerate() {
        if let Dst::SSA(ssa) = dst {
            if ssa.file() == ra.file() && ssa.comps() > 1 {
                vec_dsts.push(VecDst {
                    dst_idx: i,
                    comps: ssa.comps(),
                    killed: None,
                    reg: u8::MAX,
                });
                vec_dst_comps += ssa.comps();
            }
        }
    }

    /* No vector destinations is the easy case */
    if vec_dst_comps == 0 {
        let mut pra = PinnedRegAllocator::new(ra);
        instr_remap_srcs_file(instr, &mut pra);
        pra.free_killed(killed);
        pra.finish(pcopy);
        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
        return;
    }

    /* Predicates can't be vectors.  This lets us ignore instr.pred in our
     * analysis for the cases below. Only the easy case above needs to care
     * about them.
     */
    assert!(!ra.file().is_predicate());

    let mut avail = killed.set.clone();
    let mut killed_vecs = Vec::new();
    for src in instr.srcs() {
        if let SrcRef::SSA(vec) = src.src_ref {
            if vec.comps() > 1 {
                let mut vec_killed = true;
                for ssa in vec.iter() {
                    if ssa.file() != ra.file() || !avail.contains(ssa) {
                        vec_killed = false;
                        break;
                    }
                }
                if vec_killed {
                    for ssa in vec.iter() {
                        avail.remove(ssa);
                    }
                    killed_vecs.push(vec);
                }
            }
        }
    }

    vec_dsts.sort_by_key(|v| v.comps);
    killed_vecs.sort_by_key(|v| v.comps());

    let mut next_dst_reg = 0;
    let mut vec_dsts_map_to_killed_srcs = true;
    let mut could_trivially_allocate = true;
    for vec_dst in vec_dsts.iter_mut().rev() {
        while !killed_vecs.is_empty() {
            let src = killed_vecs.pop().unwrap();
            if src.comps() >= vec_dst.comps {
                vec_dst.killed = Some(src);
                break;
            }
        }
        if vec_dst.killed.is_none() {
            vec_dsts_map_to_killed_srcs = false;
        }

        let align = vec_dst.comps.next_power_of_two();
        if let Some(reg) =
            ra.try_find_unused_reg_range(next_dst_reg, align, vec_dst.comps)
        {
            vec_dst.reg = reg;
            next_dst_reg = reg + vec_dst.comps;
        } else {
            could_trivially_allocate = false;
        }
    }

    if vec_dsts_map_to_killed_srcs {
        let mut pra = PinnedRegAllocator::new(ra);
        instr_remap_srcs_file(instr, &mut pra);

        for vec_dst in &mut vec_dsts {
            let src_vec = vec_dst.killed.as_ref().unwrap();
            vec_dst.reg = pra.try_get_vec_reg(src_vec).unwrap();
        }

        pra.free_killed(killed);

        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = pra
                .assign_pin_vec_reg(*dst.as_ssa().unwrap(), vec_dst.reg)
                .into();
        }

        pra.finish(pcopy);

        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    } else if could_trivially_allocate {
        let mut pra = PinnedRegAllocator::new(ra);
        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = pra
                .assign_pin_vec_reg(*dst.as_ssa().unwrap(), vec_dst.reg)
                .into();
        }

        instr_remap_srcs_file(instr, &mut pra);
        pra.free_killed(killed);
        pra.finish(pcopy);
        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    } else {
        let mut pra = PinnedRegAllocator::new(ra);
        instr_remap_srcs_file(instr, &mut pra);

        /* Allocate vector destinations first so we have the most freedom.
         * Scalar destinations can fill in holes.
         */
        for dst in instr.dsts_mut() {
            if let Dst::SSA(ssa) = dst {
                if ssa.file() == pra.file() && ssa.comps() > 1 {
                    *dst = pra.alloc_vector(*ssa).into();
                }
            }
        }

        pra.free_killed(killed);
        pra.finish(pcopy);

        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    }
}

impl PerRegFile<RegAllocator> {
    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            self[ssa.file()].free_ssa(*ssa);
        }
    }
}

struct AssignRegsBlock {
    ra: PerRegFile<RegAllocator>,
    live_in: Vec<LiveValue>,
    phi_out: HashMap<u32, SrcRef>,
}

impl AssignRegsBlock {
    fn new(num_regs: &PerRegFile<u8>) -> AssignRegsBlock {
        AssignRegsBlock {
            ra: PerRegFile::new_with(|file| {
                RegAllocator::new(file, num_regs[file])
            }),
            live_in: Vec::new(),
            phi_out: HashMap::new(),
        }
    }

    fn assign_regs_instr(
        &mut self,
        mut instr: Box<Instr>,
        ip: usize,
        sum: &SSAUseMap,
        srcs_killed: &KillSet,
        dsts_killed: &KillSet,
        pcopy: &mut OpParCopy,
    ) -> Option<Box<Instr>> {
        match &instr.op {
            Op::Undef(undef) => {
                if let Dst::SSA(ssa) = undef.dst {
                    assert!(ssa.comps() == 1);
                    let ra = &mut self.ra[ssa.file()];
                    ra.alloc_scalar(ip, sum, ssa[0]);
                }
                assert!(srcs_killed.is_empty());
                self.ra.free_killed(dsts_killed);
                None
            }
            Op::PhiSrcs(phi) => {
                for (id, src) in phi.iter() {
                    assert!(src.src_mod.is_none());
                    if let SrcRef::SSA(ssa) = src.src_ref {
                        assert!(ssa.comps() == 1);
                        let reg = self.ra[ssa.file()].get_scalar(ssa[0]);
                        self.phi_out.insert(*id, reg.into());
                    } else {
                        self.phi_out.insert(*id, src.src_ref);
                    }
                }
                self.ra.free_killed(srcs_killed);
                assert!(dsts_killed.is_empty());
                None
            }
            Op::PhiDsts(phi) => {
                assert!(instr.pred.is_true());

                for (id, dst) in phi.iter() {
                    if let Dst::SSA(ssa) = dst {
                        assert!(ssa.comps() == 1);
                        let ra = &mut self.ra[ssa.file()];
                        self.live_in.push(LiveValue {
                            live_ref: LiveRef::Phi(*id),
                            reg_ref: ra.alloc_scalar(ip, sum, ssa[0]),
                        });
                    }
                }
                assert!(srcs_killed.is_empty());
                self.ra.free_killed(dsts_killed);

                None
            }
            _ => {
                for file in self.ra.values_mut() {
                    instr_assign_regs_file(
                        &mut instr,
                        ip,
                        sum,
                        srcs_killed,
                        pcopy,
                        file,
                    );
                }
                self.ra.free_killed(dsts_killed);
                Some(instr)
            }
        }
    }

    fn first_pass<BL: BlockLiveness>(
        &mut self,
        b: &mut BasicBlock,
        bl: &BL,
        pred_ra: Option<&PerRegFile<RegAllocator>>,
    ) {
        /* Populate live in from the register file we're handed.  We'll add more
         * live in when we process the OpPhiDst, if any.
         */
        if let Some(pred_ra) = pred_ra {
            for (raf, pred_raf) in self.ra.values_mut().zip(pred_ra.values()) {
                for (ssa, reg) in &pred_raf.ssa_reg {
                    if bl.is_live_in(ssa) {
                        raf.assign_reg(*ssa, *reg);
                        self.live_in.push(LiveValue {
                            live_ref: LiveRef::SSA(*ssa),
                            reg_ref: RegRef::new(raf.file(), *reg, 1),
                        });
                    }
                }
            }
        }

        let sum = SSAUseMap::for_block(b);

        let mut instrs = Vec::new();
        let mut srcs_killed = KillSet::new();
        let mut dsts_killed = KillSet::new();

        for (ip, instr) in b.instrs.drain(..).enumerate() {
            /* Build up the kill set */
            srcs_killed.clear();
            if let PredRef::SSA(ssa) = &instr.pred.pred_ref {
                if !bl.is_live_after_ip(ssa, ip) {
                    srcs_killed.insert(*ssa);
                }
            }
            for src in instr.srcs() {
                for ssa in src.iter_ssa() {
                    if !bl.is_live_after_ip(ssa, ip) {
                        srcs_killed.insert(*ssa);
                    }
                }
            }

            dsts_killed.clear();
            for dst in instr.dsts() {
                if let Dst::SSA(vec) = dst {
                    for ssa in vec.iter() {
                        if !bl.is_live_after_ip(ssa, ip) {
                            dsts_killed.insert(*ssa);
                        }
                    }
                }
            }

            let mut pcopy = OpParCopy::new();

            let instr = self.assign_regs_instr(
                instr,
                ip,
                &sum,
                &srcs_killed,
                &dsts_killed,
                &mut pcopy,
            );

            if !pcopy.is_empty() {
                instrs.push(Instr::new_boxed(pcopy));
            }

            if let Some(instr) = instr {
                instrs.push(instr);
            }
        }

        /* Sort live-in to maintain determinism */
        self.live_in.sort();

        b.instrs = instrs;
    }

    fn second_pass(&self, target: &AssignRegsBlock, b: &mut BasicBlock) {
        let mut pcopy = OpParCopy::new();

        for lv in &target.live_in {
            let src = match lv.live_ref {
                LiveRef::SSA(ssa) => {
                    SrcRef::from(self.ra[ssa.file()].get_scalar(ssa))
                }
                LiveRef::Phi(phi) => *self.phi_out.get(&phi).unwrap(),
            };
            let dst = lv.reg_ref;
            if let SrcRef::Reg(src_reg) = src {
                if dst == src_reg {
                    continue;
                }
            }
            pcopy.push(dst.into(), src.into());
        }

        if b.branch().is_some() {
            b.instrs.insert(b.instrs.len() - 1, Instr::new_boxed(pcopy));
        } else {
            b.instrs.push(Instr::new_boxed(pcopy));
        };
    }
}

struct AssignRegs {
    sm: u8,
    blocks: HashMap<u32, AssignRegsBlock>,
}

impl AssignRegs {
    pub fn new(sm: u8) -> Self {
        Self {
            sm: sm,
            blocks: HashMap::new(),
        }
    }

    pub fn run(&mut self, s: &mut Shader) {
        assert!(s.functions.len() == 1);
        let f = &mut s.functions[0];

        let cfg = CFG::for_function(f);
        let live = SimpleLiveness::for_function(f, &cfg);
        let max_live = live.calc_max_live(f, &cfg);

        let num_regs = PerRegFile::new_with(|file| {
            let num_regs = file.num_regs(self.sm);
            let max_live = max_live[file];
            if max_live > u32::from(num_regs) {
                panic!("Not enough registers. Needs {}", max_live);
            }

            if file == RegFile::GPR {
                // Shrink the number of GPRs to fit.  We need at least 16
                // registers for vectors to work.
                max(max_live, 16).try_into().unwrap()
            } else {
                num_regs
            }
        });

        s.num_gprs = num_regs[RegFile::GPR];

        for b in &mut f.blocks {
            let bl = live.block_live(b.id);

            let pred = cfg.block_predecessors(b.id);
            let pred_ra = if pred.is_empty() {
                None
            } else {
                /* Start with the previous block's. */
                Some(&self.blocks.get(&pred[0]).unwrap().ra)
            };

            let mut arb = AssignRegsBlock::new(&num_regs);
            arb.first_pass(b, bl, pred_ra);
            self.blocks.insert(b.id, arb);
        }

        for b in &mut f.blocks {
            let arb = self.blocks.get(&b.id).unwrap();
            for sb_id in cfg.block_successors(b.id) {
                let target = self.blocks.get(&sb_id).unwrap();
                arb.second_pass(target, b);
            }
        }
    }
}

impl Shader {
    pub fn assign_regs(&mut self) {
        AssignRegs::new(self.sm).run(self);
    }
}
