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

    pub fn contains_all(&self, slice: &[SSAValue]) -> bool {
        for ssa in slice {
            if !self.contains(ssa) {
                return false;
            }
        }
        true
    }

    pub fn iter(&self) -> std::slice::Iter<'_, SSAValue> {
        self.vec.iter()
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

    pub fn get_reg(&self, ssa: SSAValue) -> u8 {
        *self.ssa_reg.get(&ssa).unwrap()
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

    pub fn free_ssa_ref(&mut self, ssa_ref: SSARef) {
        for ssa in ssa_ref.iter() {
            self.free_ssa(*ssa);
        }
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

    pub fn try_assign_vec_reg(
        &mut self,
        ssa: SSARef,
        reg: u8,
    ) -> Option<RegRef> {
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
        Some(self.assign_vec_reg(ssa, reg))
    }

    pub fn try_find_unused_reg_range(
        &self,
        start_reg: u8,
        comps: u8,
    ) -> Option<u8> {
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
            let reg = max(start_reg, reg);
            if self.is_reg_in_bounds(reg, comps) {
                Some(reg)
            } else {
                None
            }
        } else {
            None
        }
    }

    fn try_find_unpinned_reg_range(
        &self,
        start_reg: u8,
        comps: u8,
    ) -> Option<u8> {
        let align = comps.next_power_of_two();

        let mut reg = start_reg.next_multiple_of(align);
        while self.is_reg_in_bounds(reg, comps) {
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
            reg += align;
        }

        None
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

    pub fn alloc_scalar(&mut self, ssa: SSAValue) -> RegRef {
        let reg = self.try_find_unused_reg_range(0, 1).unwrap();
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
    if let Pred::SSA(pred) = instr.pred {
        if pred.file() == ra.file() {
            instr.pred = ra.get_scalar(pred).into();
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
            assert!(ssa.comps() == 1);
            if ssa.file() == ra.file() {
                *dst = ra.alloc_scalar(ssa[0]).into();
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
        instr_alloc_scalar_dsts_file(instr, ra);
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

        instr_alloc_scalar_dsts_file(instr, ra);
    } else if could_trivially_allocate {
        for vec_dst in vec_dsts {
            let dst = &mut instr.dsts_mut()[vec_dst.dst_idx];
            *dst = ra
                .assign_vec_reg(*dst.as_ssa().unwrap(), vec_dst.reg)
                .into();
        }

        instr_remap_srcs_file(instr, pcopy, ra);
        ra.free_killed(killed);
        instr_alloc_scalar_dsts_file(instr, ra);
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

        instr_alloc_scalar_dsts_file(instr, ra);
    }

    ra.end_alloc();
}

#[derive(Clone)]
struct RegAllocation {
    files: [RegFileAllocation; 4],
    phi_ssa: HashMap<u32, SSARef>,
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

    pub fn free_ssa_ref(&mut self, ssa: SSARef) {
        self.file_mut(ssa.file()).free_ssa_ref(ssa);
    }

    pub fn free_killed(&mut self, killed: &KillSet) {
        for ssa in killed.iter() {
            self.free_ssa(*ssa);
        }
    }

    pub fn get_scalar(&mut self, ssa: SSAValue) -> RegRef {
        self.file_mut(ssa.file()).get_scalar(ssa)
    }

    pub fn alloc_scalar(&mut self, ssa: SSAValue) -> RegRef {
        self.file_mut(ssa.file()).alloc_scalar(ssa)
    }
}

struct AssignRegsBlock {
    ra: RegAllocation,
    live_in: Vec<LiveValue>,
    phi_out: HashMap<u32, SrcRef>,
}

impl AssignRegsBlock {
    fn new(ra: RegAllocation) -> AssignRegsBlock {
        AssignRegsBlock {
            ra: ra,
            live_in: Vec::new(),
            phi_out: HashMap::new(),
        }
    }

    fn assign_regs_instr(
        &mut self,
        mut instr: Instr,
        killed: &KillSet,
        pcopy: &mut OpParCopy,
    ) -> Option<Instr> {
        match &instr.op {
            Op::PhiSrcs(phi) => {
                for (id, src) in phi.iter() {
                    assert!(src.src_mod.is_none());
                    if let SrcRef::SSA(ssa) = src.src_ref {
                        assert!(ssa.comps() == 1);
                        let src = self.ra.get_scalar(ssa[0]).into();
                        self.phi_out.insert(*id, src);
                    } else {
                        self.phi_out.insert(*id, src.src_ref);
                    }
                }
                self.ra.free_killed(killed);
                None
            }
            Op::PhiDsts(phi) => {
                assert!(instr.pred.is_none());

                for (id, dst) in phi.iter() {
                    if let Dst::SSA(ssa) = dst {
                        assert!(ssa.comps() == 1);
                        self.live_in.push(LiveValue {
                            live_ref: LiveRef::Phi(*id),
                            reg_ref: self.ra.alloc_scalar(ssa[0]),
                        });
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
            for (ssa, reg) in &raf.ssa_reg {
                self.live_in.push(LiveValue {
                    live_ref: LiveRef::SSA(*ssa),
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
                for ssa in src.iter_ssa() {
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
                    let reg = self.ra.file(ssa.file()).get_reg(ssa);
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
