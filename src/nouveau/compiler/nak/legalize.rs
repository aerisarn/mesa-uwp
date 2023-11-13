/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::ir::*;
use crate::liveness::{BlockLiveness, Liveness, SimpleLiveness};

use std::collections::{HashMap, HashSet};

fn src_is_reg(src: &Src) -> bool {
    match src.src_ref {
        SrcRef::Zero | SrcRef::True | SrcRef::False | SrcRef::SSA(_) => true,
        SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
        SrcRef::Reg(_) => panic!("Not in SSA form"),
    }
}

fn src_as_lop_imm(src: &Src) -> Option<bool> {
    let x = match src.src_ref {
        SrcRef::Zero => false,
        SrcRef::True => true,
        SrcRef::False => false,
        SrcRef::Imm32(i) => {
            if i == 0 {
                false
            } else if i == !0 {
                true
            } else {
                return None;
            }
        }
        _ => return None,
    };
    Some(x ^ src.src_mod.is_bnot())
}

fn fold_lop_src(src: &Src, x: &mut u8) {
    if let Some(i) = src_as_lop_imm(src) {
        *x = if i { !0 } else { 0 };
    }
    if src.src_mod.is_bnot() {
        *x = !*x;
    }
}

fn copy_src(b: &mut impl SSABuilder, src: &mut Src, file: RegFile) {
    let val = b.alloc_ssa(file, 1);
    b.copy_to(val.into(), src.src_ref.into());
    src.src_ref = val.into();
}

fn copy_src_if_cbuf(b: &mut impl SSABuilder, src: &mut Src, file: RegFile) {
    match src.src_ref {
        SrcRef::CBuf(_) => copy_src(b, src, file),
        _ => (),
    }
}

fn copy_src_if_not_reg(b: &mut impl SSABuilder, src: &mut Src, file: RegFile) {
    if !src_is_reg(&src) {
        copy_src(b, src, file);
    }
}

fn swap_srcs_if_not_reg(x: &mut Src, y: &mut Src) -> bool {
    if !src_is_reg(x) && src_is_reg(y) {
        std::mem::swap(x, y);
        true
    } else {
        false
    }
}

fn legalize_sm50_instr(
    b: &mut impl SSABuilder,
    _bl: &impl BlockLiveness,
    _ip: usize,
    instr: &mut Instr,
) {
    match &mut instr.op {
        Op::Shf(op) => {
            copy_src_if_not_reg(b, &mut op.shift, RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.high, RegFile::GPR);
        }
        Op::FAdd(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src1, RegFile::GPR);
        }
        Op::FMul(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::FSet(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::FSetP(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::ISetP(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::Lop2(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::PSetP(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::Pred);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::Pred);
            copy_src_if_not_reg(b, &mut op.srcs[2], RegFile::Pred);
        }
        Op::MuFu(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::IAbs(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::Sel(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::IAdd2(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::I2F(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::F2F(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::IMad(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[2], RegFile::GPR);
        }
        Op::F2I(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::IMnMx(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::Ipa(op) => {
            copy_src_if_not_reg(b, &mut op.offset, RegFile::GPR);
        }
        Op::PopC(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::Brev(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
        }
        Op::FMnMx(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::Prmt(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.sel, RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::FFma(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[2], RegFile::GPR);
        }
        Op::Copy(_) => (), // Nothing to do
        _ => {
            let src_types = instr.src_types();
            for (i, src) in instr.srcs_mut().iter_mut().enumerate() {
                match src_types[i] {
                    SrcType::SSA => {
                        if src.as_ssa().is_none() {
                            copy_src(b, src, RegFile::GPR);
                        }
                    }
                    SrcType::GPR => {
                        copy_src_if_not_reg(b, src, RegFile::GPR);
                    }
                    SrcType::ALU
                    | SrcType::F32
                    | SrcType::F64
                    | SrcType::I32
                    | SrcType::B32 => {
                        panic!("ALU srcs must be legalized explicitly");
                    }
                    SrcType::Pred => {
                        panic!("Predicates must be legalized explicitly");
                    }
                    SrcType::Bar => panic!("Barrier regs are Volta+"),
                }
            }
        }
    }
}

fn legalize_sm70_instr(
    b: &mut impl SSABuilder,
    bl: &impl BlockLiveness,
    ip: usize,
    instr: &mut Instr,
) {
    match &mut instr.op {
        Op::FAdd(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::FFma(op) => {
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::FMnMx(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::FMul(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::FSet(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                op.cmp_op = op.cmp_op.flip();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::FSetP(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                op.cmp_op = op.cmp_op.flip();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::MuFu(_) => (), /* Nothing to do */
        Op::DAdd(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::Brev(_) | Op::Flo(_) | Op::IAbs(_) | Op::INeg(_) => (),
        Op::IAdd3(op) => {
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            swap_srcs_if_not_reg(src2, src1);
            if !src0.src_mod.is_none() && !src1.src_mod.is_none() {
                let val = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIAdd3 {
                    srcs: [Src::new_zero(), *src0, Src::new_zero()],
                    overflow: [Dst::None; 2],
                    dst: val.into(),
                });
                *src0 = val.into();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::IAdd3X(op) => {
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            swap_srcs_if_not_reg(src2, src1);
            if !src0.src_mod.is_none() && !src1.src_mod.is_none() {
                let val = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIAdd3X {
                    srcs: [Src::new_zero(), *src0, Src::new_zero()],
                    overflow: [Dst::None; 2],
                    dst: val.into(),
                    carry: [false.into(); 2],
                });
                *src0 = val.into();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::IDp4(op) => {
            let [ref mut src_type0, ref mut src_type1] = op.src_types;
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            if swap_srcs_if_not_reg(src0, src1) {
                std::mem::swap(src_type0, src_type1);
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::IMad(op) => {
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::IMad64(op) => {
            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::IMnMx(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            swap_srcs_if_not_reg(src0, src1);
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::ISetP(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                op.cmp_op = op.cmp_op.flip();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::Lop3(op) => {
            /* Fold constants and modifiers if we can */
            op.op = LogicOp3::new_lut(&|mut x, mut y, mut z| {
                fold_lop_src(&op.srcs[0], &mut x);
                fold_lop_src(&op.srcs[1], &mut y);
                fold_lop_src(&op.srcs[2], &mut z);
                op.op.eval(x, y, z)
            });
            for src in &mut op.srcs {
                src.src_mod = SrcMod::None;
                if src_as_lop_imm(src).is_some() {
                    src.src_ref = SrcRef::Zero;
                }
            }

            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                op.op = LogicOp3::new_lut(&|x, y, z| op.op.eval(y, x, z))
            }
            if !src_is_reg(src2) && src_is_reg(src1) {
                std::mem::swap(src2, src1);
                op.op = LogicOp3::new_lut(&|x, y, z| op.op.eval(x, z, y))
            }

            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::PopC(_) => (),
        Op::Shf(op) => {
            copy_src_if_not_reg(b, &mut op.low, RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.high, RegFile::GPR);
        }
        Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::Mov(_) | Op::FRnd(_) => (),
        Op::Prmt(op) => {
            copy_src_if_not_reg(b, &mut op.srcs[0], RegFile::GPR);
            copy_src_if_not_reg(b, &mut op.srcs[1], RegFile::GPR);
        }
        Op::Sel(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                op.cond.src_mod = op.cond.src_mod.bnot();
            }
            copy_src_if_not_reg(b, src0, RegFile::GPR);
        }
        Op::PLop3(op) => {
            /* Fold constants and modifiers if we can */
            for lop in &mut op.ops {
                *lop = LogicOp3::new_lut(&|mut x, mut y, mut z| {
                    fold_lop_src(&op.srcs[0], &mut x);
                    fold_lop_src(&op.srcs[1], &mut y);
                    fold_lop_src(&op.srcs[2], &mut z);
                    lop.eval(x, y, z)
                });
            }
            for src in &mut op.srcs {
                src.src_mod = SrcMod::None;
                if src_as_lop_imm(src).is_some() {
                    src.src_ref = SrcRef::True;
                }
            }

            let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
            if !src_is_reg(src0) && src_is_reg(src1) {
                std::mem::swap(src0, src1);
                for lop in &mut op.ops {
                    *lop = LogicOp3::new_lut(&|x, y, z| lop.eval(y, x, z));
                }
            }
            if !src_is_reg(src2) && src_is_reg(src1) {
                std::mem::swap(src2, src1);
                for lop in &mut op.ops {
                    *lop = LogicOp3::new_lut(&|x, y, z| lop.eval(x, z, y));
                }
            }

            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src2, RegFile::GPR);
        }
        Op::FSwzAdd(op) => {
            let [ref mut src0, ref mut src1] = op.srcs;
            copy_src_if_not_reg(b, src0, RegFile::GPR);
            copy_src_if_not_reg(b, src1, RegFile::GPR);
        }
        Op::Shfl(op) => {
            copy_src_if_not_reg(b, &mut op.src, RegFile::GPR);
            copy_src_if_cbuf(b, &mut op.lane, RegFile::GPR);
            copy_src_if_cbuf(b, &mut op.c, RegFile::GPR);
        }
        Op::Out(op) => {
            copy_src_if_not_reg(b, &mut op.handle, RegFile::GPR);
            copy_src_if_cbuf(b, &mut op.stream, RegFile::GPR);
        }
        Op::Break(op) => {
            let bar_in = op.bar_in.src_ref.as_ssa().unwrap();
            if !op.bar_out.is_none() && bl.is_live_after_ip(&bar_in[0], ip) {
                let gpr = b.bmov_to_gpr(op.bar_in);
                let tmp = b.bmov_to_bar(gpr.into());
                op.bar_in = tmp.into();
            }
        }
        Op::BSSy(op) => {
            let bar_in = op.bar_in.src_ref.as_ssa().unwrap();
            if !op.bar_out.is_none() && bl.is_live_after_ip(&bar_in[0], ip) {
                let gpr = b.bmov_to_gpr(op.bar_in);
                let tmp = b.bmov_to_bar(gpr.into());
                op.bar_in = tmp.into();
            }
        }
        Op::OutFinal(op) => {
            copy_src_if_not_reg(b, &mut op.handle, RegFile::GPR);
        }
        Op::Ldc(_) => (), // Nothing to do
        Op::BSync(_) => (),
        Op::Vote(_) => (), // Nothing to do
        Op::Copy(_) => (), // Nothing to do
        _ => {
            let src_types = instr.src_types();
            for (i, src) in instr.srcs_mut().iter_mut().enumerate() {
                match src_types[i] {
                    SrcType::SSA => {
                        if src.as_ssa().is_none() {
                            copy_src(b, src, RegFile::GPR);
                        }
                    }
                    SrcType::GPR => {
                        copy_src_if_not_reg(b, src, RegFile::GPR);
                    }
                    SrcType::ALU
                    | SrcType::F32
                    | SrcType::F64
                    | SrcType::I32
                    | SrcType::B32 => {
                        panic!("ALU srcs must be legalized explicitly");
                    }
                    SrcType::Pred => {
                        panic!("Predicates must be legalized explicitly");
                    }
                    SrcType::Bar => (),
                }
            }
        }
    }
}

fn legalize_instr(
    b: &mut impl SSABuilder,
    bl: &impl BlockLiveness,
    ip: usize,
    instr: &mut Instr,
) {
    if b.sm() >= 70 {
        legalize_sm70_instr(b, bl, ip, instr);
    } else if b.sm() >= 50 {
        legalize_sm50_instr(b, bl, ip, instr);
    } else {
        panic!("Unknown shader model SM{}", b.sm());
    }

    let src_types = instr.src_types();
    for (i, src) in instr.srcs_mut().iter_mut().enumerate() {
        if let SrcRef::Imm32(u) = &mut src.src_ref {
            *u = match src_types[i] {
                SrcType::F32 | SrcType::F64 => match src.src_mod {
                    SrcMod::None => *u,
                    SrcMod::FAbs => *u & !(1_u32 << 31),
                    SrcMod::FNeg => *u ^ !(1_u32 << 31),
                    SrcMod::FNegAbs => *u | !(1_u32 << 31),
                    _ => panic!("Not a float source modifier"),
                },
                SrcType::I32 => match src.src_mod {
                    SrcMod::None => *u,
                    SrcMod::INeg => -(*u as i32) as u32,
                    _ => panic!("Not an integer source modifier"),
                },
                SrcType::B32 => match src.src_mod {
                    SrcMod::None => *u,
                    SrcMod::BNot => !*u,
                    _ => panic!("Not a bitwise source modifier"),
                },
                _ => {
                    assert!(src.src_mod.is_none());
                    *u
                }
            };
            src.src_mod = SrcMod::None;
        }
    }

    let mut vec_src_map: HashMap<SSARef, SSARef> = HashMap::new();
    let mut vec_comps = HashSet::new();
    for src in instr.srcs_mut() {
        if let SrcRef::SSA(vec) = &src.src_ref {
            if vec.comps() == 1 {
                continue;
            }

            /* If the same vector shows up twice in one instruction, that's
             * okay. Just make it look the same as the previous source we
             * fixed up.
             */
            if let Some(new_vec) = vec_src_map.get(&vec) {
                src.src_ref = (*new_vec).into();
                continue;
            }

            let mut new_vec = *vec;
            for c in 0..vec.comps() {
                let ssa = vec[usize::from(c)];
                /* If the same SSA value shows up in multiple non-identical
                 * vector sources or as multiple components in the same
                 * source, we need to make a copy so it can get assigned to
                 * multiple different registers.
                 */
                if vec_comps.get(&ssa).is_some() {
                    let copy = b.alloc_ssa(ssa.file(), 1)[0];
                    b.copy_to(copy.into(), ssa.into());
                    new_vec[usize::from(c)] = copy;
                } else {
                    vec_comps.insert(ssa);
                }
            }

            vec_src_map.insert(*vec, new_vec);
            src.src_ref = new_vec.into();
        }
    }
}

impl Shader {
    pub fn legalize(&mut self) {
        let sm = self.info.sm;
        for f in &mut self.functions {
            let live = SimpleLiveness::for_function(f);

            for (bi, b) in f.blocks.iter_mut().enumerate() {
                let bl = live.block_live(bi);

                let mut instrs = Vec::new();
                for (ip, mut instr) in b.instrs.drain(..).enumerate() {
                    let mut b = SSAInstrBuilder::new(sm, &mut f.ssa_alloc);
                    legalize_instr(&mut b, bl, ip, &mut instr);
                    b.push_instr(instr);
                    instrs.append(&mut b.as_vec());
                }
                b.instrs = instrs;
            }
        }
    }
}
