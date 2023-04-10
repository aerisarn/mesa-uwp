/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

struct LegalizeInstr<'a> {
    ssa_alloc: &'a mut SSAValueAllocator,
    instrs: Vec<Instr>,
}

fn src_is_reg(src: &Src) -> bool {
    match src.src_ref {
        SrcRef::Zero | SrcRef::True | SrcRef::False | SrcRef::SSA(_) => true,
        SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
        SrcRef::Reg(_) => panic!("Not in SSA form"),
    }
}

impl<'a> LegalizeInstr<'a> {
    pub fn new(ssa_alloc: &'a mut SSAValueAllocator) -> LegalizeInstr {
        LegalizeInstr {
            ssa_alloc: ssa_alloc,
            instrs: Vec::new(),
        }
    }

    pub fn mov_src(&mut self, src: &mut Src, file: RegFile) {
        let val = self.ssa_alloc.alloc(file, 1);
        self.instrs
            .push(Instr::new_mov(val.into(), src.src_ref.into()));
        src.src_ref = val.into();
    }

    pub fn mov_src_if_not_reg(&mut self, src: &mut Src, file: RegFile) {
        if !src_is_reg(&src) {
            self.mov_src(src, file);
        }
    }

    pub fn swap_srcs_if_not_reg(&mut self, x: &mut Src, y: &mut Src) {
        if !src_is_reg(x) && src_is_reg(y) {
            std::mem::swap(x, y);
        }
    }

    pub fn map(&mut self, mut instr: Instr) -> Vec<Instr> {
        match &mut instr.op {
            Op::IAdd3(op) => {
                let [ref mut src0, ref mut src1, ref mut src2] = op.srcs;
                self.swap_srcs_if_not_reg(src0, src1);
                self.swap_srcs_if_not_reg(src2, src1);
                self.mov_src_if_not_reg(src0, RegFile::GPR);
                self.mov_src_if_not_reg(src2, RegFile::GPR);
            }
            Op::St(op) => {
                self.mov_src_if_not_reg(&mut op.data, RegFile::GPR);
            }
            _ => (),
        }
        self.instrs.push(instr);
        std::mem::replace(&mut self.instrs, Vec::new())
    }
}

impl Shader {
    pub fn legalize(&mut self) {
        self.map_instrs(&|instr, ssa_alloc| -> Vec<Instr> {
            LegalizeInstr::new(ssa_alloc).map(instr)
        });
    }
}
