/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(unstable_name_collisions)]

use crate::bitset::BitSet;
use crate::nak_ir::*;
use crate::nak_liveness::{BlockLiveness, Liveness};
use crate::util::NextMultipleOf;

use std::cmp::Ordering;
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

    fn find_vec_use_after(&self, ssa: &SSAValue, ip: usize) -> Option<&SSAUse> {
        if let Some(v) = self.ssa_map.get(ssa) {
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
struct RegFileAllocation {
    file: RegFile,
    max_reg: u8,
    used: BitSet,
    pinned: BitSet,
    reg_ssa: Vec<SSAValue>,
    ssa_reg: HashMap<SSAValue, u8>,
}

impl RegFileAllocation {
    pub fn new(file: RegFile, sm: u8) -> Self {
        Self {
            file: file,
            max_reg: file.num_regs(sm) - 1,
            used: BitSet::new(),
            pinned: BitSet::new(),
            reg_ssa: Vec::new(),
            ssa_reg: HashMap::new(),
        }
    }

    fn file(&self) -> RegFile {
        self.file
    }

    pub fn begin_alloc(&mut self) {
        self.pinned.clear();
    }

    pub fn end_alloc(&mut self) {}

    fn is_reg_in_bounds(&self, reg: u8, comps: u8) -> bool {
        if let Some(max_reg) = reg.checked_add(comps - 1) {
            max_reg <= self.max_reg
        } else {
            false
        }
    }

    pub fn try_get_reg(&self, ssa: SSAValue) -> Option<u8> {
        self.ssa_reg.get(&ssa).cloned()
    }

    pub fn get_reg(&self, ssa: SSAValue) -> u8 {
        self.try_get_reg(ssa).expect("Undefined SSA value")
    }

    pub fn get_ssa(&self, reg: u8) -> Option<SSAValue> {
        if self.used.get(reg.into()) {
            Some(self.reg_ssa[usize::from(reg)])
        } else {
            None
        }
    }

    pub fn try_get_vec_reg(&self, vec: SSARef) -> Option<u8> {
        let align = vec.comps().next_power_of_two();
        let reg = self.get_reg(vec[0]);
        if reg % align == 0 {
            for i in 1..vec.comps() {
                if self.get_reg(vec[usize::from(i)]) != reg + i {
                    return None;
                }
            }
            Some(reg)
        } else {
            None
        }
    }

    pub fn free_ssa(&mut self, ssa: SSAValue) -> u8 {
        assert!(ssa.file() == self.file);
        let reg = self.ssa_reg.remove(&ssa).unwrap();
        assert!(self.used.get(reg.into()));
        self.used.remove(reg.into());
        reg
    }

    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            if ssa.file() == self.file {
                self.free_ssa(*ssa);
            }
        }
    }

    pub fn assign_reg(&mut self, ssa: SSAValue, reg: u8) -> RegRef {
        assert!(ssa.file() == self.file);
        assert!(reg <= self.max_reg);
        assert!(!self.used.get(reg.into()));

        if usize::from(reg) >= self.reg_ssa.len() {
            self.reg_ssa.resize(usize::from(reg) + 1, SSAValue::NONE);
        }
        self.reg_ssa[usize::from(reg)] = ssa;
        self.ssa_reg.insert(ssa, reg);
        self.used.insert(reg.into());
        self.pinned.insert(reg.into());

        RegRef::new(self.file, reg, 1)
    }

    pub fn assign_vec_reg(&mut self, ssa: SSARef, reg: u8) -> RegRef {
        for i in 0..ssa.comps() {
            self.assign_reg(ssa[usize::from(i)], reg + i);
        }
        RegRef::new(self.file, reg, ssa.comps())
    }

    pub fn try_find_unused_reg_range(
        &self,
        start_reg: u8,
        comps: u8,
    ) -> Option<u8> {
        assert!(comps > 0);
        let align = comps.next_power_of_two();

        let mut next_reg = start_reg;
        loop {
            let reg = self.used.next_unset(next_reg.into());

            /* Ensure we're properly aligned */
            let Ok(reg) = u8::try_from(reg.next_multiple_of(align.into())) else {
                return None;
            };

            if !self.is_reg_in_bounds(reg, comps) {
                return None;
            }

            let mut avail = true;
            for c in 0..comps {
                let reg_c = usize::from(reg + c);
                if self.used.get(reg_c) || self.pinned.get(reg_c) {
                    avail = false;
                    break;
                }
            }
            if avail {
                return Some(reg);
            }

            next_reg = match reg.checked_add(align) {
                Some(r) => r,
                None => return None,
            }
        }
    }

    fn try_find_unused_reg(
        &self,
        start_reg: u8,
        align: u8,
        comp: u8,
    ) -> Option<u8> {
        let mut reg = start_reg;
        loop {
            reg = match self.try_find_unused_reg_range(reg, 1) {
                Some(r) => r,
                None => break None,
            };

            if reg % align == comp {
                return Some(reg);
            }
            reg += 1;
        }
    }

    fn try_find_unpinned_reg_range(
        &self,
        start_reg: u8,
        comps: u8,
    ) -> Option<u8> {
        let align = comps.next_power_of_two();

        let mut next_reg = start_reg;
        loop {
            let reg = self.pinned.next_unset(next_reg.into());

            /* Ensure we're properly aligned */
            let reg = match u8::try_from(reg.next_multiple_of(align.into())) {
                Ok(r) => r,
                Err(_) => return None,
            };

            if !self.is_reg_in_bounds(reg, comps) {
                return None;
            }

            let mut is_pinned = false;
            for i in 0..comps {
                if self.pinned.get((reg + i).into()) {
                    is_pinned = true;
                    break;
                }
            }
            if !is_pinned {
                return Some(reg);
            }

            next_reg = match reg.checked_add(align) {
                Some(r) => r,
                None => return None,
            }
        }
    }

    pub fn try_find_unpinned_reg_near_ssa(&self, ssa: SSARef) -> Option<u8> {
        /* Get something near component 0 */
        self.try_find_unpinned_reg_range(self.get_reg(ssa[0]), ssa.comps())
    }

    pub fn get_scalar(&mut self, ssa: SSAValue) -> RegRef {
        assert!(ssa.file() == self.file);
        let reg = self.get_reg(ssa);
        self.pinned.insert(reg.into());
        RegRef::new(self.file, reg, 1)
    }

    pub fn move_to_reg(
        &mut self,
        pcopy: &mut OpParCopy,
        ssa: SSARef,
        reg: u8,
    ) -> RegRef {
        for c in 0..ssa.comps() {
            let old_reg = self.get_reg(ssa[usize::from(c)]);
            if old_reg == reg + c {
                continue;
            }

            self.free_ssa(ssa[usize::from(c)]);

            /* If something already exists in the destination, swap it to the
             * source.
             */
            if let Some(evicted) = self.get_ssa(reg + c) {
                self.free_ssa(evicted);
                pcopy.srcs.push(RegRef::new(self.file, reg + c, 1).into());
                pcopy.dsts.push(RegRef::new(self.file, old_reg, 1).into());
                self.assign_reg(evicted, old_reg);
            }

            pcopy.srcs.push(RegRef::new(self.file, old_reg, 1).into());
            pcopy.dsts.push(RegRef::new(self.file, reg + c, 1).into());
            self.assign_reg(ssa[usize::from(c)], reg + c);
        }

        RegRef::new(self.file, reg, ssa.comps())
    }

    pub fn get_vector(&mut self, pcopy: &mut OpParCopy, ssa: SSARef) -> RegRef {
        let reg = self
            .try_get_vec_reg(ssa)
            .or_else(|| self.try_find_unused_reg_range(0, ssa.comps()))
            .or_else(|| self.try_find_unpinned_reg_near_ssa(ssa))
            .or_else(|| self.try_find_unpinned_reg_range(0, ssa.comps()))
            .expect("Failed to find an unpinned register range");

        for c in 0..ssa.comps() {
            self.pinned.insert((reg + c).into());
        }
        self.move_to_reg(pcopy, ssa, reg)
    }

    pub fn alloc_scalar(
        &mut self,
        ip: usize,
        sum: &SSAUseMap,
        ssa: SSAValue,
    ) -> RegRef {
        if let Some(u) = sum.find_vec_use_after(&ssa, ip) {
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

                            if !self.used.get((vec_reg + comp).into()) {
                                return self.assign_reg(ssa, vec_reg + comp);
                            }
                        }
                    }

                    /* We weren't able to pair it with an already allocated
                     * register but maybe we can at least find an aligned one.
                     */
                    if let Some(reg) = self.try_find_unused_reg(0, align, comp)
                    {
                        return self.assign_reg(ssa, reg);
                    }
                }
            }
        }

        let reg = self
            .try_find_unused_reg_range(0, 1)
            .expect("Failed to find free register");
        self.assign_reg(ssa, reg)
    }

    pub fn alloc_vector(
        &mut self,
        pcopy: &mut OpParCopy,
        ssa: SSARef,
    ) -> RegRef {
        let reg = self
            .try_find_unused_reg_range(0, ssa.comps())
            .or_else(|| self.try_find_unpinned_reg_range(0, ssa.comps()))
            .expect("Failed to find an unpinned register range");

        for c in 0..ssa.comps() {
            self.pinned.insert((reg + c).into());
        }

        for c in 0..ssa.comps() {
            if let Some(evicted) = self.get_ssa(reg + c) {
                self.free_ssa(evicted);
                let new_reg = self.try_find_unused_reg_range(0, 1).unwrap();
                pcopy.srcs.push(RegRef::new(self.file, reg + c, 1).into());
                pcopy.dsts.push(RegRef::new(self.file, new_reg, 1).into());
                self.assign_reg(evicted, new_reg);
            }
        }

        self.assign_vec_reg(ssa, reg)
    }
}

fn instr_remap_srcs_file(
    instr: &mut Instr,
    pcopy: &mut OpParCopy,
    ra: &mut RegFileAllocation,
) {
    if let PredRef::SSA(pred) = instr.pred.pred_ref {
        if pred.file() == ra.file() {
            instr.pred.pred_ref = ra.get_scalar(pred).into();
        }
    }

    for src in instr.srcs_mut() {
        if let SrcRef::SSA(ssa) = src.src_ref {
            if ssa.file() == ra.file() {
                src.src_ref = ra.get_vector(pcopy, ssa).into();
            }
        }
    }
}

fn instr_alloc_scalar_dsts_file(
    instr: &mut Instr,
    ip: usize,
    sum: &SSAUseMap,
    ra: &mut RegFileAllocation,
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
    ra: &mut RegFileAllocation,
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
        ra.begin_alloc();
        instr_remap_srcs_file(instr, pcopy, ra);
        ra.end_alloc();
        ra.free_killed(killed);
        ra.begin_alloc();
        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
        ra.end_alloc();
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

        if let Some(reg) =
            ra.try_find_unused_reg_range(next_dst_reg, vec_dst.comps)
        {
            vec_dst.reg = reg;
            next_dst_reg = reg + vec_dst.comps;
        } else {
            could_trivially_allocate = false;
        }
    }

    ra.begin_alloc();

    if vec_dsts_map_to_killed_srcs {
        instr_remap_srcs_file(instr, pcopy, ra);

        for vec_dst in &mut vec_dsts {
            vec_dst.reg = ra.try_get_vec_reg(vec_dst.killed.unwrap()).unwrap();
        }

        ra.free_killed(killed);

        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = ra
                .assign_vec_reg(*dst.as_ssa().unwrap(), vec_dst.reg)
                .into();
        }

        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    } else if could_trivially_allocate {
        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = ra
                .assign_vec_reg(*dst.as_ssa().unwrap(), vec_dst.reg)
                .into();
        }

        instr_remap_srcs_file(instr, pcopy, ra);
        ra.free_killed(killed);
        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    } else {
        instr_remap_srcs_file(instr, pcopy, ra);
        ra.free_killed(killed);

        /* Allocate vector destinations first so we have the most freedom.
         * Scalar destinations can fill in holes.
         */
        for dst in instr.dsts_mut() {
            if let Dst::SSA(ssa) = dst {
                if ssa.file() == ra.file() && ssa.comps() > 1 {
                    *dst = ra.alloc_vector(pcopy, *ssa).into();
                }
            }
        }

        instr_alloc_scalar_dsts_file(instr, ip, sum, ra);
    }

    ra.end_alloc();
}

impl PerRegFile<RegFileAllocation> {
    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            self[ssa.file()].free_ssa(*ssa);
        }
    }
}

struct AssignRegsBlock {
    ra: PerRegFile<RegFileAllocation>,
    live_in: Vec<LiveValue>,
    phi_out: HashMap<u32, SrcRef>,
}

impl AssignRegsBlock {
    fn new(ra: PerRegFile<RegFileAllocation>) -> AssignRegsBlock {
        AssignRegsBlock {
            ra: ra,
            live_in: Vec::new(),
            phi_out: HashMap::new(),
        }
    }

    fn assign_regs_instr(
        &mut self,
        mut instr: Box<Instr>,
        ip: usize,
        sum: &SSAUseMap,
        killed: &KillSet,
        pcopy: &mut OpParCopy,
    ) -> Option<Box<Instr>> {
        match &instr.op {
            Op::Undef(undef) => {
                if let Dst::SSA(ssa) = undef.dst {
                    assert!(ssa.comps() == 1);
                    let ra = &mut self.ra[ssa.file()];
                    ra.alloc_scalar(ip, sum, ssa[0]);
                }
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
                self.ra.free_killed(killed);
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

                None
            }
            _ => {
                for file in self.ra.values_mut() {
                    instr_assign_regs_file(
                        &mut instr, ip, sum, killed, pcopy, file,
                    );
                }
                Some(instr)
            }
        }
    }

    fn first_pass(&mut self, b: &mut BasicBlock, bl: &BlockLiveness) {
        /* Populate live in from the register file we're handed.  We'll add more
         * live in when we process the OpPhiDst, if any.
         */
        for raf in self.ra.values_mut() {
            for (ssa, reg) in &raf.ssa_reg {
                if bl.is_live_in(ssa) {
                    self.live_in.push(LiveValue {
                        live_ref: LiveRef::SSA(*ssa),
                        reg_ref: RegRef::new(raf.file(), *reg, 1),
                    });
                }
            }
        }

        let sum = SSAUseMap::for_block(b);

        let mut instrs = Vec::new();
        let mut killed = KillSet::new();

        for (ip, instr) in b.instrs.drain(..).enumerate() {
            /* Build up the kill set */
            killed.clear();
            if let PredRef::SSA(ssa) = &instr.pred.pred_ref {
                if !bl.is_live_after(ssa, ip) {
                    killed.insert(*ssa);
                }
            }
            for src in instr.srcs() {
                for ssa in src.iter_ssa() {
                    if !bl.is_live_after(ssa, ip) {
                        killed.insert(*ssa);
                    }
                }
            }

            let mut pcopy = OpParCopy::new();

            let instr =
                self.assign_regs_instr(instr, ip, &sum, &killed, &mut pcopy);

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
                    let reg = self.ra[ssa.file()].get_reg(ssa);
                    SrcRef::from(RegRef::new(ssa.file(), reg, 1))
                }
                LiveRef::Phi(phi) => *self.phi_out.get(&phi).unwrap(),
            };
            let dst = lv.reg_ref;
            if let SrcRef::Reg(src_reg) = src {
                if dst == src_reg {
                    continue;
                }
            }
            pcopy.srcs.push(src.into());
            pcopy.dsts.push(dst.into());
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

    pub fn run(&mut self, f: &mut Function) {
        let live = Liveness::for_function(f);
        for b in &mut f.blocks {
            let bl = live.block(&b);

            let ra = if bl.predecessors.is_empty() {
                PerRegFile::new_with(&|file| {
                    RegFileAllocation::new(file, self.sm)
                })
            } else {
                /* Start with the previous block's. */
                self.blocks.get(&bl.predecessors[0]).unwrap().ra.clone()
            };

            let mut arb = AssignRegsBlock::new(ra);
            arb.first_pass(b, &bl);
            self.blocks.insert(b.id, arb);
        }

        for b in &mut f.blocks {
            let bl = live.block(&b);
            let arb = self.blocks.get(&b.id).unwrap();
            for succ in bl.successors {
                if let Some(succ) = succ {
                    let target = self.blocks.get(&succ).unwrap();
                    arb.second_pass(target, b);
                }
            }
        }
    }
}

impl Shader {
    pub fn assign_regs(&mut self) {
        for f in &mut self.functions {
            AssignRegs::new(self.sm).run(f);
        }
    }
}
