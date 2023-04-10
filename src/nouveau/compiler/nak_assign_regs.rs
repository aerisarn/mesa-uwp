/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(unstable_name_collisions)]

use crate::bitset::BitSet;
use crate::nak_ir::*;
use crate::nak_liveness::{BlockLiveness, Liveness};
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

    pub fn contains(&self, ssa: &SSAValue) -> bool {
        self.set.contains(ssa)
    }

    pub fn iter(&self) -> std::slice::Iter<'_, SSAValue> {
        self.vec.iter()
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
struct PhiComp {
    v: SSAValue,
}

impl PhiComp {
    pub fn new(idx: u32, comp: u8) -> PhiComp {
        PhiComp {
            v: SSAValue::new(RegFile::GPR, idx, comp + 1),
        }
    }

    pub fn idx(&self) -> u32 {
        self.v.idx()
    }

    pub fn comp(&self) -> u8 {
        self.v.comps() - 1
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
enum LiveRef {
    SSA(SSAComp),
    Phi(PhiComp),
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
    reg_ssa: Vec<SSAComp>,
    ssa_reg: HashMap<SSAComp, u8>,
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

    pub fn get_reg_comp(&self, ssa: SSAComp) -> u8 {
        *self.ssa_reg.get(&ssa).unwrap()
    }

    pub fn get_ssa_comp(&self, reg: u8) -> Option<SSAComp> {
        if self.used.get(reg.into()) {
            Some(self.reg_ssa[usize::from(reg)])
        } else {
            None
        }
    }

    pub fn try_get_reg(&self, ssa: SSAValue) -> Option<u8> {
        let align = ssa.comps().next_power_of_two();
        let reg = self.get_reg_comp(ssa.comp(0));
        if reg % align == 0 {
            for i in 1..ssa.comps() {
                if self.get_reg_comp(ssa.comp(i)) != reg + i {
                    return None;
                }
            }
            Some(reg)
        } else {
            None
        }
    }

    pub fn free_ssa_comp(&mut self, ssa: SSAComp) -> u8 {
        assert!(ssa.file() == self.file);
        let reg = self.ssa_reg.remove(&ssa).unwrap();
        assert!(self.used.get(reg.into()));
        self.used.remove(reg.into());
        reg
    }

    pub fn free_ssa(&mut self, ssa: SSAValue) {
        for i in 0..ssa.comps() {
            self.free_ssa_comp(ssa.comp(i));
        }
    }

    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            if ssa.file() == self.file {
                self.free_ssa(*ssa);
            }
        }
    }

    pub fn assign_reg_comp(&mut self, ssa: SSAComp, reg: u8) -> RegRef {
        assert!(ssa.file() == self.file);
        assert!(reg <= self.max_reg);
        assert!(!self.used.get(reg.into()));

        if usize::from(reg) >= self.reg_ssa.len() {
            self.reg_ssa
                .resize(usize::from(reg) + 1, SSAComp::new(RegFile::GPR, 0, 0));
        }
        self.reg_ssa[usize::from(reg)] = ssa;
        self.ssa_reg.insert(ssa, reg);
        self.used.insert(reg.into());
        self.pinned.insert(reg.into());

        RegRef::new(self.file, reg, 1)
    }

    pub fn assign_reg(&mut self, ssa: SSAValue, reg: u8) -> RegRef {
        for i in 0..ssa.comps() {
            self.assign_reg_comp(ssa.comp(i), reg + i);
        }
        RegRef::new(self.file, reg, ssa.comps())
    }

    pub fn try_assign_reg(&mut self, ssa: SSAValue, reg: u8) -> Option<RegRef> {
        if ssa.file() != self.file() {
            return None;
        }
        if !self.is_reg_in_bounds(reg, ssa.comps()) {
            return None;
        }
        for c in 0..ssa.comps() {
            if self.used.get((reg + c).into()) {
                return None;
            }
        }
        Some(self.assign_reg(ssa, reg))
    }

    pub fn try_find_unused_reg(&self, start_reg: u8, comps: u8) -> Option<u8> {
        assert!(comps > 0);
        let comps_mask = u32::MAX >> (32 - comps);
        let align = comps.next_power_of_two();

        let start_word = usize::from(start_reg / 32);
        for w in start_word..self.used.words().len() {
            let word = self.used.words()[w];

            let mut avail = !word;
            if w == start_word {
                avail &= u32::MAX << (start_reg % 32);
            }

            if w < self.pinned.words().len() {
                avail &= !self.pinned.words()[w];
            }

            while avail != 0 {
                let bit = u8::try_from(avail.trailing_zeros()).unwrap();

                /* Ensure we're properly aligned */
                if bit & (align - 1) != 0 {
                    avail &= !(1 << bit);
                    continue;
                }

                let mask = comps_mask << bit;
                if avail & mask == mask {
                    let reg = u8::try_from(w * 32).unwrap() + bit;
                    if self.is_reg_in_bounds(reg, comps) {
                        return Some(reg);
                    } else {
                        return None;
                    }
                }

                avail &= !mask;
            }
        }

        if let Ok(reg) = u8::try_from(self.used.words().len() * 32) {
            if self.is_reg_in_bounds(reg, comps) {
                Some(reg)
            } else {
                None
            }
        } else {
            None
        }
    }

    fn get_reg_near_reg(&self, reg: u8, comps: u8) -> u8 {
        let align = comps.next_power_of_two();

        /* Pick something properly aligned near component 0 */
        let mut reg = reg & (align - 1);
        if !self.is_reg_in_bounds(reg, comps) {
            reg -= align;
        }
        reg
    }

    pub fn get_reg_near_ssa(&self, ssa: SSAValue) -> u8 {
        /* Get something near component 0 */
        self.get_reg_near_reg(self.get_reg_comp(ssa.comp(0)), ssa.comps())
    }

    pub fn get_any_reg(&self, comps: u8) -> u8 {
        let mut pick_comps = comps;
        while pick_comps > 0 {
            if let Some(reg) = self.try_find_unused_reg(0, pick_comps) {
                return self.get_reg_near_reg(reg, comps);
            }
            pick_comps = pick_comps >> 1;
        }
        panic!("Failed to find any free registers");
    }

    pub fn get_scalar(&mut self, ssa: SSAComp) -> RegRef {
        assert!(ssa.file() == self.file);
        let reg = self.get_reg_comp(ssa);
        self.pinned.insert(reg.into());
        RegRef::new(self.file, reg, 1)
    }

    pub fn move_to_reg(
        &mut self,
        pcopy: &mut OpParCopy,
        ssa: SSAValue,
        reg: u8,
    ) -> RegRef {
        for c in 0..ssa.comps() {
            let old_reg = self.get_reg_comp(ssa.comp(c));
            if old_reg == reg + c {
                continue;
            }

            self.free_ssa_comp(ssa.comp(c));

            /* If something already exists in the destination, swap it to the
             * source.
             */
            if let Some(evicted) = self.get_ssa_comp(reg + c) {
                self.free_ssa_comp(evicted);
                pcopy.srcs.push(RegRef::new(self.file, reg + c, 1).into());
                pcopy.dsts.push(RegRef::new(self.file, old_reg, 1).into());
                self.assign_reg_comp(evicted, old_reg);
            }

            pcopy.srcs.push(RegRef::new(self.file, old_reg, 1).into());
            pcopy.dsts.push(RegRef::new(self.file, reg + c, 1).into());
            self.assign_reg_comp(ssa.comp(c), reg + c);
        }

        RegRef::new(self.file, reg, ssa.comps())
    }

    pub fn get_vector(
        &mut self,
        pcopy: &mut OpParCopy,
        ssa: SSAValue,
    ) -> RegRef {
        let reg = if let Some(reg) = self.try_get_reg(ssa) {
            reg
        } else if let Some(reg) = self.try_find_unused_reg(0, ssa.comps()) {
            reg
        } else {
            self.get_reg_near_ssa(ssa)
        };

        self.move_to_reg(pcopy, ssa, reg)
    }

    pub fn alloc_scalar(&mut self, ssa: SSAComp) -> RegRef {
        let reg = self.try_find_unused_reg(0, 1).unwrap();
        self.assign_reg_comp(ssa, reg)
    }
}

fn instr_remap_srcs_file(
    instr: &mut Instr,
    pcopy: &mut OpParCopy,
    ra: &mut RegFileAllocation,
) {
    if let Pred::SSA(pred) = instr.pred {
        if pred.file() == ra.file() {
            instr.pred = ra.get_scalar(pred.as_comp()).into();
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

fn instr_alloc_scalar_dsts_file(instr: &mut Instr, ra: &mut RegFileAllocation) {
    for dst in instr.dsts_mut() {
        if let Dst::SSA(ssa) = dst {
            if ssa.file() == ra.file() {
                *dst = ra.alloc_scalar(ssa.as_comp()).into();
            }
        }
    }
}

fn instr_assign_regs_file(
    instr: &mut Instr,
    killed: &KillSet,
    pcopy: &mut OpParCopy,
    ra: &mut RegFileAllocation,
) {
    struct VecDst {
        dst_idx: usize,
        comps: u8,
        killed: Option<SSAValue>,
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
        instr_alloc_scalar_dsts_file(instr, ra);
        ra.end_alloc();
        return;
    }

    /* Predicates can't be vectors.  This lets us ignore instr.pred in our
     * analysis for the cases below. Only the easy case above needs to care
     * about them.
     */
    assert!(!ra.file().is_predicate());

    let mut killed_vecs = Vec::new();
    let mut killed_vec_comps = 0;
    for ssa in killed.iter() {
        if ssa.file() == ra.file() && ssa.comps() > 1 {
            killed_vecs.push(ssa);
            killed_vec_comps += ssa.comps();
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
                vec_dst.killed = Some(*src);
                break;
            }
        }
        if vec_dst.killed.is_none() {
            vec_dsts_map_to_killed_srcs = false;
        }

        if let Some(reg) = ra.try_find_unused_reg(next_dst_reg, vec_dst.comps) {
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
            vec_dst.reg = ra.try_get_reg(vec_dst.killed.unwrap()).unwrap();
        }

        ra.free_killed(killed);

        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = ra.assign_reg(*dst.as_ssa().unwrap(), vec_dst.reg).into();
        }

        instr_alloc_scalar_dsts_file(instr, ra);
    } else if could_trivially_allocate {
        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = ra.assign_reg(*dst.as_ssa().unwrap(), vec_dst.reg).into();
        }

        instr_remap_srcs_file(instr, pcopy, ra);
        ra.free_killed(killed);
        instr_alloc_scalar_dsts_file(instr, ra);
    } else {
        /* We're all out of tricks.  We need to allocate enough space for all
         * the vector destinations and all the killed SSA values and shuffle the
         * killed values into the new space.
         */
        let vec_comps = max(killed_vec_comps, vec_dst_comps);
        let vec_reg = ra.get_any_reg(vec_comps);

        let mut ssa_reg = HashMap::new();
        let mut src_vec_reg = vec_reg;
        for src in instr.srcs_mut() {
            if let SrcRef::SSA(ssa) = src.src_ref {
                if ssa.file() == ra.file() {
                    if killed.contains(&ssa) && ssa.comps() > 1 {
                        let reg = *ssa_reg.entry(ssa).or_insert_with(|| {
                            let align = ssa.comps().next_power_of_two();
                            let reg = src_vec_reg;
                            src_vec_reg += ssa.comps();
                            /* We assume vector sources are in order of
                             * decreasing alignment.  This is true for texture
                             * opcodes which should be the only interesting
                             * case.
                             */
                            assert!(reg % align == 0);
                            ra.move_to_reg(pcopy, ssa, reg)
                        });
                        src.src_ref = reg.into();
                    }
                }
            }
        }

        /* Handle the scalar and not killed sources */
        instr_remap_srcs_file(instr, pcopy, ra);

        ra.free_killed(killed);

        let mut dst_vec_reg = vec_reg;
        for dst in instr.dsts_mut() {
            if let Dst::SSA(ssa) = dst {
                if ssa.comps() > 1 {
                    let align = ssa.comps().next_power_of_two();
                    let reg = dst_vec_reg;
                    dst_vec_reg += ssa.comps();
                    /* We assume vector destinations are in order of decreasing
                     * alignment.  This is true for texture opcodes which should
                     * be the only interesting case.
                     */
                    assert!(reg % align == 0);
                    *dst = ra.assign_reg(*ssa, reg).into();
                }
            }
        }

        instr_alloc_scalar_dsts_file(instr, ra);
    }

    ra.end_alloc();
}

#[derive(Clone)]
struct RegAllocation {
    files: [RegFileAllocation; 4],
    phi_ssa: HashMap<u32, SSAValue>,
}

impl RegAllocation {
    pub fn new(sm: u8) -> Self {
        Self {
            files: [
                RegFileAllocation::new(RegFile::GPR, sm),
                RegFileAllocation::new(RegFile::UGPR, sm),
                RegFileAllocation::new(RegFile::Pred, sm),
                RegFileAllocation::new(RegFile::UPred, sm),
            ],
            phi_ssa: HashMap::new(),
        }
    }

    pub fn file(&self, file: RegFile) -> &RegFileAllocation {
        for f in &self.files {
            if f.file() == file {
                return f;
            }
        }
        panic!("Unknown register file");
    }

    pub fn file_mut(&mut self, file: RegFile) -> &mut RegFileAllocation {
        for f in &mut self.files {
            if f.file() == file {
                return f;
            }
        }
        panic!("Unknown register file");
    }

    pub fn free_ssa(&mut self, ssa: SSAValue) {
        self.file_mut(ssa.file()).free_ssa(ssa);
    }

    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            self.free_ssa(*ssa);
        }
    }

    pub fn get_scalar(&mut self, ssa: SSAComp) -> RegRef {
        self.file_mut(ssa.file()).get_scalar(ssa)
    }

    pub fn alloc_scalar(&mut self, ssa: SSAComp) -> RegRef {
        self.file_mut(ssa.file()).alloc_scalar(ssa)
    }
}

struct AssignRegsBlock {
    ra: RegAllocation,
    live_in: Vec<LiveValue>,
    phi_out: HashMap<PhiComp, SrcRef>,
}

impl AssignRegsBlock {
    fn new(ra: RegAllocation) -> AssignRegsBlock {
        AssignRegsBlock {
            ra: ra,
            live_in: Vec::new(),
            phi_out: HashMap::new(),
        }
    }

    fn assign_regs_split(
        &mut self,
        split: &OpSplit,
        killed: &KillSet,
        pcopy: &mut OpParCopy,
    ) {
        let src = split.src.src_ref.as_ssa().unwrap();
        let comps = src.comps();
        assert!(usize::from(comps) == split.dsts.len());

        let mut coalesced = BitSet::new();
        if killed.contains(src) {
            for c in 0..comps {
                /* Feee the component regardless of any dest checks */
                let src_ra = self.ra.file_mut(src.file());
                let reg = src_ra.free_ssa_comp(src.comp(c));
                let src_ref = RegRef::new(src.file(), reg, 1);

                /* If we have an OpSplit which kills its source, we can coalesce
                 * on the spot into the destinations.
                 */
                if let Dst::SSA(dst) = &split.dsts[usize::from(c)] {
                    if dst.file() == src.file() {
                        /* Assign destinations to source components when the
                         * register files match.
                         */
                        let dst_ra = src_ra;
                        dst_ra.assign_reg_comp(dst.as_comp(), reg);
                        coalesced.insert(c.into());
                    } else {
                        /* Otherwise, they come from different files so
                         * allocating a destination register won't affect the
                         * source and it's okay to alloc before we've finished
                         * freeing the source.
                         */
                        let dst_ra = self.ra.file_mut(dst.file());
                        let dst_ref = dst_ra.alloc_scalar(dst.as_comp());
                        pcopy.srcs.push(src_ref.into());
                        pcopy.dsts.push(dst_ref.into());
                    }
                }
            }
        } else {
            for c in 0..comps {
                if let Dst::SSA(dst) = &split.dsts[usize::from(c)] {
                    pcopy.srcs.push(self.ra.get_scalar(src.comp(c)).into());
                    pcopy.dsts.push(self.ra.alloc_scalar(dst.as_comp()).into());
                }
            }
        }
    }

    fn assign_regs_instr(
        &mut self,
        mut instr: Instr,
        killed: &KillSet,
        pcopy: &mut OpParCopy,
    ) -> Option<Instr> {
        match &instr.op {
            Op::Split(split) => {
                assert!(instr.pred.is_none());
                assert!(split.src.src_mod.is_none());
                self.assign_regs_split(split, killed, pcopy);
                None
            }
            Op::PhiSrcs(phi) => {
                for (id, src) in phi.iter() {
                    assert!(src.src_mod.is_none());
                    if let SrcRef::SSA(ssa) = src.src_ref {
                        for c in 0..ssa.comps() {
                            let src = self.ra.get_scalar(ssa.comp(c)).into();
                            self.phi_out.insert(PhiComp::new(*id, 0), src);
                        }
                    } else {
                        self.phi_out.insert(PhiComp::new(*id, 0), src.src_ref);
                    }
                }
                self.ra.free_killed(killed);
                None
            }
            Op::PhiDsts(phi) => {
                assert!(instr.pred.is_none());

                for (id, dst) in phi.iter() {
                    if let Dst::SSA(ssa) = dst {
                        for c in 0..ssa.comps() {
                            self.live_in.push(LiveValue {
                                live_ref: LiveRef::Phi(PhiComp::new(*id, c)),
                                reg_ref: self.ra.alloc_scalar(ssa.as_comp()),
                            });
                        }
                    }
                }

                None
            }
            _ => {
                for file in &mut self.ra.files {
                    instr_assign_regs_file(&mut instr, killed, pcopy, file);
                }
                Some(instr)
            }
        }
    }

    fn first_pass(&mut self, b: &mut BasicBlock, bl: &BlockLiveness) {
        /* Populate live in from the register file we're handed.  We'll add more
         * live in when we process the OpPhiDst, if any.
         */
        for raf in &self.ra.files {
            for (comp, reg) in &raf.ssa_reg {
                self.live_in.push(LiveValue {
                    live_ref: LiveRef::SSA(*comp),
                    reg_ref: RegRef::new(raf.file(), *reg, 1),
                });
            }
        }

        let mut instrs = Vec::new();
        let mut killed = KillSet::new();

        for (ip, instr) in b.instrs.drain(..).enumerate() {
            /* Build up the kill set */
            killed.clear();
            if let Pred::SSA(ssa) = &instr.pred {
                if !bl.is_live_after(ssa, ip) {
                    killed.insert(*ssa);
                }
            }
            for src in instr.srcs() {
                if let SrcRef::SSA(ssa) = &src.src_ref {
                    if !bl.is_live_after(ssa, ip) {
                        killed.insert(*ssa);
                    }
                }
            }

            let mut pcopy = OpParCopy::new();

            let instr = self.assign_regs_instr(instr, &killed, &mut pcopy);

            if !pcopy.is_empty() {
                instrs.push(Instr::new(Op::ParCopy(pcopy)));
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
                    let reg = self.ra.file(ssa.file()).get_reg_comp(ssa);
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

        let pcopy = Instr::new(Op::ParCopy(pcopy));
        if b.branch().is_some() {
            b.instrs.insert(b.instrs.len() - 1, pcopy);
        } else {
            b.instrs.push(pcopy);
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
                RegAllocation::new(self.sm)
            } else {
                /* Start with the previous block's. */
                self.blocks.get(&bl.predecessors[0]).unwrap().ra.clone()
            };

            let mut arb = AssignRegsBlock::new(ra);
            arb.first_pass(b, bl);
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

struct TrivialRegAlloc {
    next_reg: u8,
    next_ureg: u8,
    next_pred: u8,
    next_upred: u8,
    reg_map: HashMap<SSAValue, RegRef>,
    phi_map: HashMap<u32, RegRef>,
}

impl TrivialRegAlloc {
    pub fn new() -> TrivialRegAlloc {
        TrivialRegAlloc {
            next_reg: 16, /* Leave some space for FS outputs */
            next_ureg: 0,
            next_pred: 0,
            next_upred: 0,
            reg_map: HashMap::new(),
            phi_map: HashMap::new(),
        }
    }

    fn alloc_reg(&mut self, file: RegFile, comps: u8) -> RegRef {
        let align = comps.next_power_of_two();
        let idx = match file {
            RegFile::GPR => {
                let idx = self.next_reg.next_multiple_of(align);
                self.next_reg = idx + comps;
                idx
            }
            RegFile::UGPR => {
                let idx = self.next_ureg.next_multiple_of(align);
                self.next_ureg = idx + comps;
                idx
            }
            RegFile::Pred => {
                let idx = self.next_pred.next_multiple_of(align);
                self.next_pred = idx + comps;
                idx
            }
            RegFile::UPred => {
                let idx = self.next_upred.next_multiple_of(align);
                self.next_upred = idx + comps;
                idx
            }
        };
        RegRef::new(file, idx, comps)
    }

    fn alloc_ssa(&mut self, ssa: SSAValue) -> RegRef {
        let reg = self.alloc_reg(ssa.file(), ssa.comps());
        let old = self.reg_map.insert(ssa, reg);
        assert!(old.is_none());
        reg
    }

    fn get_ssa_reg(&self, ssa: SSAValue) -> RegRef {
        *self.reg_map.get(&ssa).unwrap()
    }

    fn map_src(&self, mut src: Src) -> Src {
        if let SrcRef::SSA(ssa) = src.src_ref {
            src.src_ref = self.get_ssa_reg(ssa).into();
        }
        src
    }

    pub fn do_alloc(&mut self, s: &mut Shader) {
        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    match &instr.op {
                        Op::PhiDsts(phi) => {
                            let mut pcopy = OpParCopy::new();

                            assert!(phi.ids.len() == phi.dsts.len());
                            for (id, dst) in phi.iter() {
                                let dst_ssa = dst.as_ssa().unwrap();
                                let dst_reg = self.alloc_ssa(*dst_ssa);
                                let src_reg = self
                                    .alloc_reg(dst_ssa.file(), dst_ssa.comps());
                                self.phi_map.insert(*id, src_reg);
                                pcopy.srcs.push(src_reg.into());
                                pcopy.dsts.push(dst_reg.into());
                            }

                            instr.op = Op::ParCopy(pcopy);
                        }
                        _ => (),
                    }
                }
            }
        }

        for f in &mut s.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    match &instr.op {
                        Op::PhiSrcs(phi) => {
                            assert!(phi.ids.len() == phi.srcs.len());
                            instr.op = Op::ParCopy(OpParCopy {
                                srcs: phi
                                    .srcs
                                    .iter()
                                    .map(|src| self.map_src(*src))
                                    .collect(),
                                dsts: phi
                                    .ids
                                    .iter()
                                    .map(|id| {
                                        (*self.phi_map.get(id).unwrap()).into()
                                    })
                                    .collect(),
                            });
                        }
                        _ => {
                            if let Pred::SSA(ssa) = instr.pred {
                                instr.pred = self.get_ssa_reg(ssa).into();
                            }
                            for dst in instr.dsts_mut() {
                                if let Dst::SSA(ssa) = dst {
                                    *dst = self.alloc_ssa(*ssa).into();
                                }
                            }
                            for src in instr.srcs_mut() {
                                *src = self.map_src(*src);
                            }
                        }
                    }
                }
            }
        }
    }
}

impl Shader {
    pub fn assign_regs_trivial(&mut self) {
        TrivialRegAlloc::new().do_alloc(self);
    }
}
