/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

extern crate nak_ir_proc;

use nak_ir_proc::*;
use std::fmt;
use std::iter::Zip;
use std::ops::{BitAnd, BitOr, Deref, DerefMut, Index, Not, Range};
use std::slice;

#[repr(u8)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum RegFile {
    GPR = 0,
    UGPR = 1,
    Pred = 2,
    UPred = 3,
}

impl RegFile {
    pub fn is_uniform(&self) -> bool {
        match self {
            RegFile::GPR | RegFile::Pred => false,
            RegFile::UGPR | RegFile::UPred => true,
        }
    }

    pub fn is_gpr(&self) -> bool {
        match self {
            RegFile::GPR | RegFile::UGPR => true,
            RegFile::Pred | RegFile::UPred => false,
        }
    }

    pub fn is_predicate(&self) -> bool {
        match self {
            RegFile::GPR | RegFile::UGPR => false,
            RegFile::Pred | RegFile::UPred => true,
        }
    }

    pub fn num_regs(&self, sm: u8) -> u8 {
        match self {
            RegFile::GPR => 255,
            RegFile::UGPR => {
                if sm >= 75 {
                    63
                } else {
                    0
                }
            }
            RegFile::Pred => 7,
            RegFile::UPred => {
                if sm >= 75 {
                    7
                } else {
                    0
                }
            }
        }
    }
}

impl From<RegFile> for u8 {
    fn from(value: RegFile) -> u8 {
        value as u8
    }
}

impl TryFrom<u32> for RegFile {
    type Error = &'static str;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(RegFile::GPR),
            1 => Ok(RegFile::UGPR),
            2 => Ok(RegFile::Pred),
            3 => Ok(RegFile::UPred),
            _ => Err("Invalid register file number"),
        }
    }
}

impl TryFrom<u16> for RegFile {
    type Error = &'static str;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        RegFile::try_from(u32::from(value))
    }
}

impl TryFrom<u8> for RegFile {
    type Error = &'static str;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        RegFile::try_from(u32::from(value))
    }
}

pub trait HasRegFile {
    fn file(&self) -> RegFile;

    fn is_uniform(&self) -> bool {
        self.file().is_uniform()
    }

    fn is_gpr(&self) -> bool {
        self.file().is_gpr()
    }

    fn is_predicate(&self) -> bool {
        self.file().is_predicate()
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct SSAValue {
    packed: u32,
}

impl SSAValue {
    pub const NONE: Self = SSAValue { packed: 0 };

    pub fn new(file: RegFile, idx: u32) -> SSAValue {
        /* Reserve 2 numbers for use for SSARef::comps() */
        assert!(idx > 0 && idx < (1 << 30) - 2);
        let mut packed = idx;
        assert!(u8::from(file) < 4);
        packed |= u32::from(u8::from(file)) << 30;
        SSAValue { packed: packed }
    }

    pub fn idx(&self) -> u32 {
        self.packed & 0x3fffffff
    }

    #[allow(dead_code)]
    pub fn is_none(&self) -> bool {
        self.packed == 0
    }
}

impl HasRegFile for SSAValue {
    fn file(&self) -> RegFile {
        RegFile::try_from(self.packed >> 30).unwrap()
    }
}

impl fmt::Display for SSAValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.file() {
            RegFile::GPR => write!(f, "S")?,
            RegFile::UGPR => write!(f, "US")?,
            RegFile::Pred => write!(f, "PS")?,
            RegFile::UPred => write!(f, "UPS")?,
        }
        write!(f, "{}", self.idx())
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct SSARef {
    v: [SSAValue; 4],
}

impl SSARef {
    #[inline]
    fn new(comps: &[SSAValue]) -> SSARef {
        assert!(comps.len() > 0 && comps.len() <= 4);
        let mut r = SSARef {
            v: [SSAValue::NONE; 4],
        };
        for i in 0..comps.len() {
            r.v[i] = comps[i];
        }
        if comps.len() < 4 {
            r.v[3].packed = (comps.len() as u32).wrapping_neg();
        }
        r
    }

    pub fn comps(&self) -> u8 {
        if self.v[3].packed >= u32::MAX - 2 {
            self.v[3].packed.wrapping_neg() as u8
        } else {
            4
        }
    }
}

impl HasRegFile for SSARef {
    fn file(&self) -> RegFile {
        let comps = usize::from(self.comps());
        for i in 1..comps {
            assert!(self.v[i].file() == self.v[0].file());
        }
        self.v[0].file()
    }
}

impl Deref for SSARef {
    type Target = [SSAValue];

    fn deref(&self) -> &[SSAValue] {
        let comps = usize::from(self.comps());
        &self.v[..comps]
    }
}

impl DerefMut for SSARef {
    fn deref_mut(&mut self) -> &mut [SSAValue] {
        let comps = usize::from(self.comps());
        &mut self.v[..comps]
    }
}

impl TryFrom<&[SSAValue]> for SSARef {
    type Error = &'static str;

    fn try_from(comps: &[SSAValue]) -> Result<Self, Self::Error> {
        if comps.len() == 0 {
            Err("Empty vector")
        } else if comps.len() > 4 {
            Err("Too many vector components")
        } else {
            Ok(SSARef::new(comps))
        }
    }
}

impl TryFrom<Vec<SSAValue>> for SSARef {
    type Error = &'static str;

    fn try_from(comps: Vec<SSAValue>) -> Result<Self, Self::Error> {
        SSARef::try_from(&comps[..])
    }
}

macro_rules! impl_ssa_ref_from_arr {
    ($n: expr) => {
        impl From<[SSAValue; $n]> for SSARef {
            fn from(comps: [SSAValue; $n]) -> Self {
                SSARef::new(&comps[..])
            }
        }
    };
}
impl_ssa_ref_from_arr!(1);
impl_ssa_ref_from_arr!(2);
impl_ssa_ref_from_arr!(3);
impl_ssa_ref_from_arr!(4);

impl From<SSAValue> for SSARef {
    fn from(val: SSAValue) -> Self {
        [val].into()
    }
}

impl fmt::Display for SSARef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.comps() == 1 {
            write!(f, "{}", self[0])
        } else {
            write!(f, "{{")?;
            for v in self.iter() {
                write!(f, " {}", v)?;
            }
            write!(f, " }}")
        }
    }
}

pub struct SSAValueAllocator {
    count: u32,
}

impl SSAValueAllocator {
    pub fn new() -> SSAValueAllocator {
        SSAValueAllocator { count: 0 }
    }

    pub fn alloc(&mut self, file: RegFile) -> SSAValue {
        self.count += 1;
        SSAValue::new(file, self.count)
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct RegRef {
    packed: u16,
}

impl RegRef {
    fn zero_idx(file: RegFile) -> u8 {
        match file {
            RegFile::GPR => 255,
            RegFile::UGPR => 63,
            RegFile::Pred => 7,
            RegFile::UPred => 7,
        }
    }

    pub fn new(file: RegFile, base_idx: u8, comps: u8) -> RegRef {
        assert!(base_idx + (comps - 1) <= RegRef::zero_idx(file));
        let mut packed = u16::from(base_idx);
        assert!(comps > 0 && comps <= 8);
        packed |= u16::from(comps - 1) << 8;
        assert!(u8::from(file) < 4);
        packed |= u16::from(u8::from(file)) << 11;
        RegRef { packed: packed }
    }

    pub fn zero(file: RegFile, comps: u8) -> RegRef {
        RegRef::new(file, RegRef::zero_idx(file), comps)
    }

    pub fn base_idx(&self) -> u8 {
        self.packed as u8
    }

    pub fn idx_range(&self) -> Range<u16> {
        let start = u16::from(self.base_idx());
        let end = start + u16::from(self.comps());
        start..end
    }

    pub fn comps(&self) -> u8 {
        (((self.packed >> 8) & 0x7) + 1).try_into().unwrap()
    }
}

impl HasRegFile for RegRef {
    fn file(&self) -> RegFile {
        ((self.packed >> 11) & 0xf).try_into().unwrap()
    }
}

impl fmt::Display for RegRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.file() {
            RegFile::GPR => write!(f, "R")?,
            RegFile::UGPR => write!(f, "UR")?,
            RegFile::Pred => write!(f, "P")?,
            RegFile::UPred => write!(f, "UP")?,
        }
        write!(f, "{}", self.base_idx())?;
        if self.comps() > 1 {
            write!(f, "..{}", self.base_idx() + self.comps())?;
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
pub enum Dst {
    None,
    SSA(SSARef),
    Reg(RegRef),
}

impl Dst {
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            Dst::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        match self {
            Dst::SSA(r) => Some(r),
            _ => None,
        }
    }
}

impl From<RegRef> for Dst {
    fn from(reg: RegRef) -> Dst {
        Dst::Reg(reg)
    }
}

impl<T: Into<SSARef>> From<T> for Dst {
    fn from(ssa: T) -> Dst {
        Dst::SSA(ssa.into())
    }
}

impl fmt::Display for Dst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Dst::None => write!(f, "NULL")?,
            Dst::SSA(v) => v.fmt(f)?,
            Dst::Reg(r) => r.fmt(f)?,
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum CBuf {
    Binding(u8),
    BindlessSSA(SSAValue),
    BindlessGPR(RegRef),
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct CBufRef {
    pub buf: CBuf,
    pub offset: u16,
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum SrcRef {
    Zero,
    True,
    False,
    Imm32(u32),
    CBuf(CBufRef),
    SSA(SSARef),
    Reg(RegRef),
}

impl SrcRef {
    pub fn is_alu(&self) -> bool {
        match self {
            SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => true,
            SrcRef::SSA(ssa) => ssa.is_gpr(),
            SrcRef::Reg(reg) => reg.is_gpr(),
            SrcRef::True | SrcRef::False => false,
        }
    }

    pub fn is_predicate(&self) -> bool {
        match self {
            SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
            SrcRef::True | SrcRef::False => true,
            SrcRef::SSA(ssa) => ssa.is_predicate(),
            SrcRef::Reg(reg) => reg.is_predicate(),
        }
    }

    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        match self {
            SrcRef::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::SSA(_) => None,
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessSSA(_) => None,
                CBuf::BindlessGPR(reg) => Some(reg),
            },
            SrcRef::Reg(reg) => Some(reg),
        }
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        match self {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::Reg(_) => &[],
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessGPR(_) => &[],
                CBuf::BindlessSSA(ssa) => slice::from_ref(ssa),
            },
            SrcRef::SSA(ssa) => ssa,
        }
        .iter()
    }
}

impl From<RegRef> for SrcRef {
    fn from(reg: RegRef) -> SrcRef {
        SrcRef::Reg(reg)
    }
}

impl<T: Into<SSARef>> From<T> for SrcRef {
    fn from(ssa: T) -> SrcRef {
        SrcRef::SSA(ssa.into())
    }
}

impl fmt::Display for SrcRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SrcRef::Zero => write!(f, "ZERO")?,
            SrcRef::True => write!(f, "TRUE")?,
            SrcRef::False => write!(f, "FALSE")?,
            SrcRef::Imm32(u) => write!(f, "{:#x}", u)?,
            SrcRef::CBuf(r) => {
                match r.buf {
                    CBuf::Binding(idx) => write!(f, "c[{:#x}]", idx)?,
                    CBuf::BindlessSSA(v) => write!(f, "cx[{}]", v)?,
                    CBuf::BindlessGPR(r) => write!(f, "cx[{}]", r)?,
                }
                write!(f, "[{:#x}]", r.offset)?;
            }
            SrcRef::SSA(v) => v.fmt(f)?,
            SrcRef::Reg(r) => r.fmt(f)?,
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
pub enum SrcMod {
    None,
    FAbs,
    FNeg,
    FNegAbs,
    INeg,
    BNot,
}

impl SrcMod {
    pub fn is_none(&self) -> bool {
        match self {
            SrcMod::None => true,
            _ => false,
        }
    }

    pub fn is_bnot(&self) -> bool {
        match self {
            SrcMod::None => false,
            SrcMod::BNot => true,
            _ => panic!("Not a bitwise modifier"),
        }
    }

    pub fn fabs(self) -> SrcMod {
        match self {
            SrcMod::None | SrcMod::FAbs | SrcMod::FNeg | SrcMod::FNegAbs => {
                SrcMod::FAbs
            }
            _ => panic!("Not a float source modifier"),
        }
    }

    pub fn fneg(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::FNeg,
            SrcMod::FAbs => SrcMod::FNegAbs,
            SrcMod::FNeg => SrcMod::None,
            SrcMod::FNegAbs => SrcMod::FAbs,
            _ => panic!("Not a float source modifier"),
        }
    }

    pub fn ineg(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::INeg,
            SrcMod::INeg => SrcMod::None,
            _ => panic!("Not an integer source modifier"),
        }
    }

    pub fn bnot(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::BNot,
            SrcMod::BNot => SrcMod::None,
            _ => panic!("Not a boolean source modifier"),
        }
    }

    pub fn modify(self, other: SrcMod) -> SrcMod {
        match other {
            SrcMod::None => self,
            SrcMod::FAbs => self.fabs(),
            SrcMod::FNeg => self.fneg(),
            SrcMod::FNegAbs => self.fabs().fneg(),
            SrcMod::INeg => self.ineg(),
            SrcMod::BNot => self.bnot(),
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum SrcType {
    SSA,
    GPR,
    ALU,
    F32,
    F64,
    I32,
    Pred,
}

#[derive(Clone, Copy)]
pub struct Src {
    pub src_ref: SrcRef,
    pub src_mod: SrcMod,
}

impl Src {
    pub fn new_zero() -> Src {
        SrcRef::Zero.into()
    }

    pub fn new_imm_u32(u: u32) -> Src {
        SrcRef::Imm32(u).into()
    }

    pub fn new_imm_bool(b: bool) -> Src {
        Src::from(if b { SrcRef::True } else { SrcRef::False })
    }

    pub fn new_cbuf(idx: u8, offset: u16) -> Src {
        SrcRef::CBuf(CBufRef {
            buf: CBuf::Binding(idx),
            offset: offset,
        })
        .into()
    }

    pub fn fabs(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.fabs(),
        }
    }

    pub fn fneg(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.fneg(),
        }
    }

    pub fn ineg(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.ineg(),
        }
    }

    pub fn bnot(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.bnot(),
        }
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        if self.src_mod.is_none() {
            self.src_ref.as_ssa()
        } else {
            None
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        self.src_ref.get_reg()
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        self.src_ref.iter_ssa()
    }

    #[allow(dead_code)]
    pub fn is_uniform(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::CBuf(_) => true,
            SrcRef::SSA(ssa) => ssa.is_uniform(),
            SrcRef::Reg(reg) => reg.is_uniform(),
        }
    }

    pub fn is_predicate(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
            SrcRef::True | SrcRef::False => true,
            SrcRef::SSA(ssa) => ssa.is_predicate(),
            SrcRef::Reg(reg) => reg.is_predicate(),
        }
    }

    pub fn is_zero(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero => true,
            _ => false,
        }
    }

    pub fn is_reg_or_zero(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero | SrcRef::SSA(_) | SrcRef::Reg(_) => true,
            SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::CBuf(_) => false,
        }
    }

    #[allow(dead_code)]
    pub fn supports_type(&self, src_type: &SrcType) -> bool {
        match src_type {
            SrcType::SSA => {
                if !self.src_mod.is_none() {
                    return false;
                }

                match self.src_ref {
                    SrcRef::SSA(_) | SrcRef::Reg(_) => true,
                    _ => false,
                }
            }
            SrcType::GPR => {
                if !self.src_mod.is_none() {
                    return false;
                }

                match self.src_ref {
                    SrcRef::Zero | SrcRef::SSA(_) | SrcRef::Reg(_) => true,
                    _ => false,
                }
            }
            SrcType::ALU => self.src_mod.is_none() && self.src_ref.is_alu(),
            SrcType::F32 | SrcType::F64 => {
                match self.src_mod {
                    SrcMod::None
                    | SrcMod::FAbs
                    | SrcMod::FNeg
                    | SrcMod::FNegAbs => (),
                    _ => return false,
                }

                self.src_ref.is_alu()
            }
            SrcType::I32 => {
                match self.src_mod {
                    SrcMod::None | SrcMod::INeg => (),
                    _ => return false,
                }

                self.src_ref.is_alu()
            }
            SrcType::Pred => {
                match self.src_mod {
                    SrcMod::None | SrcMod::BNot => (),
                    _ => return false,
                }

                self.src_ref.is_predicate()
            }
        }
    }
}

impl<T: Into<SrcRef>> From<T> for Src {
    fn from(value: T) -> Src {
        Src {
            src_ref: value.into(),
            src_mod: SrcMod::None,
        }
    }
}

impl fmt::Display for Src {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.src_mod {
            SrcMod::None => write!(f, "{}", self.src_ref),
            SrcMod::FAbs => write!(f, "|{}|", self.src_ref),
            SrcMod::FNeg => write!(f, "-{}", self.src_ref),
            SrcMod::FNegAbs => write!(f, "-|{}|", self.src_ref),
            SrcMod::INeg => write!(f, "-{}", self.src_ref),
            SrcMod::BNot => write!(f, "!{}", self.src_ref),
        }
    }
}

impl SrcType {
    const DEFAULT: SrcType = SrcType::GPR;
}

pub enum SrcTypeList {
    Array(&'static [SrcType]),
    Uniform(SrcType),
}

impl Index<usize> for SrcTypeList {
    type Output = SrcType;

    fn index(&self, idx: usize) -> &SrcType {
        match self {
            SrcTypeList::Array(arr) => &arr[idx],
            SrcTypeList::Uniform(typ) => &typ,
        }
    }
}

pub trait SrcsAsSlice {
    fn srcs_as_slice(&self) -> &[Src];
    fn srcs_as_mut_slice(&mut self) -> &mut [Src];
    fn src_types(&self) -> SrcTypeList;
}

pub trait DstsAsSlice {
    fn dsts_as_slice(&self) -> &[Dst];
    fn dsts_as_mut_slice(&mut self) -> &mut [Dst];
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum PredSetOp {
    And,
    Or,
    Xor,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum FloatCmpOp {
    OrdEq,
    OrdNe,
    OrdLt,
    OrdLe,
    OrdGt,
    OrdGe,
    UnordEq,
    UnordNe,
    UnordLt,
    UnordLe,
    UnordGt,
    UnordGe,
    IsNum,
    IsNan,
}

impl FloatCmpOp {
    pub fn flip(self) -> FloatCmpOp {
        match self {
            FloatCmpOp::OrdEq | FloatCmpOp::OrdNe => self,
            FloatCmpOp::OrdLt => FloatCmpOp::OrdGt,
            FloatCmpOp::OrdLe => FloatCmpOp::OrdGe,
            FloatCmpOp::OrdGt => FloatCmpOp::OrdLt,
            FloatCmpOp::OrdGe => FloatCmpOp::OrdLe,
            FloatCmpOp::UnordEq | FloatCmpOp::UnordNe => self,
            FloatCmpOp::UnordLt => FloatCmpOp::UnordGt,
            FloatCmpOp::UnordLe => FloatCmpOp::UnordGe,
            FloatCmpOp::UnordGt => FloatCmpOp::UnordLt,
            FloatCmpOp::UnordGe => FloatCmpOp::UnordLe,
            FloatCmpOp::IsNum | FloatCmpOp::IsNan => panic!("Cannot flip unop"),
        }
    }
}

impl fmt::Display for FloatCmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FloatCmpOp::OrdEq => write!(f, "EQ"),
            FloatCmpOp::OrdNe => write!(f, "NE"),
            FloatCmpOp::OrdLt => write!(f, "LT"),
            FloatCmpOp::OrdLe => write!(f, "LE"),
            FloatCmpOp::OrdGt => write!(f, "GT"),
            FloatCmpOp::OrdGe => write!(f, "GE"),
            FloatCmpOp::UnordEq => write!(f, "EQU"),
            FloatCmpOp::UnordNe => write!(f, "NEU"),
            FloatCmpOp::UnordLt => write!(f, "LTU"),
            FloatCmpOp::UnordLe => write!(f, "LEU"),
            FloatCmpOp::UnordGt => write!(f, "GTU"),
            FloatCmpOp::UnordGe => write!(f, "GEU"),
            FloatCmpOp::IsNum => write!(f, "NUM"),
            FloatCmpOp::IsNan => write!(f, "NAN"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum IntCmpOp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

impl IntCmpOp {
    pub fn flip(self) -> IntCmpOp {
        match self {
            IntCmpOp::Eq | IntCmpOp::Ne => self,
            IntCmpOp::Lt => IntCmpOp::Gt,
            IntCmpOp::Le => IntCmpOp::Ge,
            IntCmpOp::Gt => IntCmpOp::Lt,
            IntCmpOp::Ge => IntCmpOp::Le,
        }
    }
}

impl fmt::Display for IntCmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntCmpOp::Eq => write!(f, "EQ"),
            IntCmpOp::Ne => write!(f, "NE"),
            IntCmpOp::Lt => write!(f, "LT"),
            IntCmpOp::Le => write!(f, "LE"),
            IntCmpOp::Gt => write!(f, "GT"),
            IntCmpOp::Ge => write!(f, "GE"),
        }
    }
}

pub enum IntCmpType {
    U32,
    I32,
}

impl fmt::Display for IntCmpType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntCmpType::U32 => write!(f, "U32"),
            IntCmpType::I32 => write!(f, "I32"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct LogicOp {
    pub lut: u8,
}

impl LogicOp {
    pub const SRC_MASKS: [u8; 3] = [0xf0, 0xcc, 0xaa];

    #[inline]
    pub fn new_lut<F: Fn(u8, u8, u8) -> u8>(f: &F) -> LogicOp {
        LogicOp {
            lut: f(
                LogicOp::SRC_MASKS[0],
                LogicOp::SRC_MASKS[1],
                LogicOp::SRC_MASKS[2],
            ),
        }
    }

    pub fn new_const(val: bool) -> LogicOp {
        LogicOp {
            lut: if val { !0 } else { 0 },
        }
    }

    pub fn src_used(&self, src_idx: usize) -> bool {
        let mask = LogicOp::SRC_MASKS[src_idx];
        let shift = LogicOp::SRC_MASKS[src_idx].trailing_zeros();
        self.lut & !mask != (self.lut >> shift) & !mask
    }

    pub fn fix_src(&mut self, src_idx: usize, val: bool) {
        let mask = LogicOp::SRC_MASKS[src_idx];
        let shift = LogicOp::SRC_MASKS[src_idx].trailing_zeros();
        if val {
            let t_bits = self.lut & mask;
            self.lut = t_bits | (t_bits >> shift)
        } else {
            let f_bits = self.lut & !mask;
            self.lut = (f_bits << shift) | f_bits
        };
    }

    pub fn invert_src(&mut self, src_idx: usize) {
        let mask = LogicOp::SRC_MASKS[src_idx];
        let shift = LogicOp::SRC_MASKS[src_idx].trailing_zeros();
        let t_bits = self.lut & mask;
        let f_bits = self.lut & !mask;
        self.lut = (f_bits << shift) | (t_bits >> shift);
    }

    pub fn eval<
        T: BitAnd<Output = T> + BitOr<Output = T> + Copy + Not<Output = T>,
    >(
        &self,
        x: T,
        y: T,
        z: T,
    ) -> T {
        let mut res = x & !x; /* zero */
        if (self.lut & (1 << 0)) != 0 {
            res = res | (!x & !y & !z);
        }
        if (self.lut & (1 << 1)) != 0 {
            res = res | (!x & !y & z);
        }
        if (self.lut & (1 << 2)) != 0 {
            res = res | (!x & y & !z);
        }
        if (self.lut & (1 << 3)) != 0 {
            res = res | (!x & y & z);
        }
        if (self.lut & (1 << 4)) != 0 {
            res = res | (x & !y & !z);
        }
        if (self.lut & (1 << 5)) != 0 {
            res = res | (x & !y & z);
        }
        if (self.lut & (1 << 6)) != 0 {
            res = res | (x & y & !z);
        }
        if (self.lut & (1 << 7)) != 0 {
            res = res | (x & y & z);
        }
        res
    }
}

impl fmt::Display for LogicOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "LUT[{:#x}]", self.lut)
    }
}

pub enum FloatType {
    F16,
    F32,
    F64,
}

impl FloatType {
    pub fn from_bytes(bytes: usize) -> FloatType {
        match bytes {
            2 => FloatType::F16,
            4 => FloatType::F32,
            8 => FloatType::F64,
            _ => panic!("Invalid float type size"),
        }
    }

    pub fn bytes(&self) -> usize {
        match self {
            FloatType::F16 => 2,
            FloatType::F32 => 4,
            FloatType::F64 => 8,
        }
    }
}

impl fmt::Display for FloatType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FloatType::F16 => write!(f, "F16"),
            FloatType::F32 => write!(f, "F32"),
            FloatType::F64 => write!(f, "F64"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum FRndMode {
    NearestEven,
    NegInf,
    PosInf,
    Zero,
}

impl fmt::Display for FRndMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FRndMode::NearestEven => write!(f, "RE"),
            FRndMode::NegInf => write!(f, "RM"),
            FRndMode::PosInf => write!(f, "RP"),
            FRndMode::Zero => write!(f, "RZ"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexDim {
    _1D,
    Array1D,
    _2D,
    Array2D,
    _3D,
    Cube,
    ArrayCube,
}

impl fmt::Display for TexDim {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexDim::_1D => write!(f, "1D"),
            TexDim::Array1D => write!(f, "ARRAY_1D"),
            TexDim::_2D => write!(f, "2D"),
            TexDim::Array2D => write!(f, "ARRAY_2D"),
            TexDim::_3D => write!(f, "3D"),
            TexDim::Cube => write!(f, "CUBE"),
            TexDim::ArrayCube => write!(f, "ARRAY_CUBE"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexLodMode {
    Auto,
    Zero,
    Bias,
    Lod,
    Clamp,
    BiasClamp,
}

impl fmt::Display for TexLodMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexLodMode::Auto => write!(f, "LA"),
            TexLodMode::Zero => write!(f, "LZ"),
            TexLodMode::Bias => write!(f, "LB"),
            TexLodMode::Lod => write!(f, "LL"),
            TexLodMode::Clamp => write!(f, "LC"),
            TexLodMode::BiasClamp => write!(f, "LB.LC"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum Tld4OffsetMode {
    None,
    AddOffI,
    PerPx,
}

impl fmt::Display for Tld4OffsetMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Tld4OffsetMode::None => write!(f, "NO_OFF"),
            Tld4OffsetMode::AddOffI => write!(f, "AOFFI"),
            Tld4OffsetMode::PerPx => write!(f, "PTP"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexQuery {
    Dimension,
    TextureType,
    SamplerPos,
}

impl fmt::Display for TexQuery {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexQuery::Dimension => write!(f, "DIMENSION"),
            TexQuery::TextureType => write!(f, "TEXTURE_TYPE"),
            TexQuery::SamplerPos => write!(f, "SAMPLER_POS"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum ImageDim {
    _1D,
    _1DBuffer,
    _1DArray,
    _2D,
    _2DArray,
    _3D,
}

impl ImageDim {
    pub fn coord_comps(&self) -> u8 {
        match self {
            ImageDim::_1D => 1,
            ImageDim::_1DBuffer => 1,
            ImageDim::_1DArray => 2,
            ImageDim::_2D => 2,
            ImageDim::_2DArray => 3,
            ImageDim::_3D => 3,
        }
    }
}

impl fmt::Display for ImageDim {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImageDim::_1D => write!(f, "1D"),
            ImageDim::_1DBuffer => write!(f, "1D_BUFFER"),
            ImageDim::_1DArray => write!(f, "1D_ARRAY"),
            ImageDim::_2D => write!(f, "2D"),
            ImageDim::_2DArray => write!(f, "2D_ARRAY"),
            ImageDim::_3D => write!(f, "3D"),
        }
    }
}

pub enum IntType {
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    U64,
    I64,
}

impl IntType {
    pub fn from_bytes(bytes: usize, is_signed: bool) -> IntType {
        match bytes {
            1 => {
                if is_signed {
                    IntType::I8
                } else {
                    IntType::U8
                }
            }
            2 => {
                if is_signed {
                    IntType::I16
                } else {
                    IntType::U16
                }
            }
            4 => {
                if is_signed {
                    IntType::I32
                } else {
                    IntType::U32
                }
            }
            8 => {
                if is_signed {
                    IntType::I64
                } else {
                    IntType::U64
                }
            }
            _ => panic!("Invalid integer type size"),
        }
    }

    pub fn is_signed(&self) -> bool {
        match self {
            IntType::U8 | IntType::U16 | IntType::U32 | IntType::U64 => false,
            IntType::I8 | IntType::I16 | IntType::I32 | IntType::I64 => true,
        }
    }

    pub fn bytes(&self) -> usize {
        match self {
            IntType::U8 | IntType::I8 => 1,
            IntType::U16 | IntType::I16 => 2,
            IntType::U32 | IntType::I32 => 4,
            IntType::U64 | IntType::I64 => 8,
        }
    }
}

impl fmt::Display for IntType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntType::U8 => write!(f, "U8"),
            IntType::I8 => write!(f, "I8"),
            IntType::U16 => write!(f, "U16"),
            IntType::I16 => write!(f, "I16"),
            IntType::U32 => write!(f, "U32"),
            IntType::I32 => write!(f, "I32"),
            IntType::U64 => write!(f, "U64"),
            IntType::I64 => write!(f, "I64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemAddrType {
    A32,
    A64,
}

impl fmt::Display for MemAddrType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemAddrType::A32 => write!(f, "A32"),
            MemAddrType::A64 => write!(f, "A64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemType {
    U8,
    I8,
    U16,
    I16,
    B32,
    B64,
    B128,
}

impl MemType {
    pub fn from_size(size: u8, is_signed: bool) -> MemType {
        match size {
            1 => {
                if is_signed {
                    MemType::I8
                } else {
                    MemType::U8
                }
            }
            2 => {
                if is_signed {
                    MemType::I16
                } else {
                    MemType::U16
                }
            }
            4 => MemType::B32,
            8 => MemType::B64,
            16 => MemType::B128,
            _ => panic!("Invalid memory load/store size"),
        }
    }
}

impl fmt::Display for MemType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemType::U8 => write!(f, "U8"),
            MemType::I8 => write!(f, "I8"),
            MemType::U16 => write!(f, "U16"),
            MemType::I16 => write!(f, "I16"),
            MemType::B32 => write!(f, "B32"),
            MemType::B64 => write!(f, "B64"),
            MemType::B128 => write!(f, "B128"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemOrder {
    Weak,
    Strong,
}

impl fmt::Display for MemOrder {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemOrder::Weak => write!(f, "WEAK"),
            MemOrder::Strong => write!(f, "STRONG"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemScope {
    CTA,
    Cluster,
    GPU,
    System,
}

impl fmt::Display for MemScope {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemScope::CTA => write!(f, "CTA"),
            MemScope::Cluster => write!(f, "SM"),
            MemScope::GPU => write!(f, "GPU"),
            MemScope::System => write!(f, "SYS"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemSpace {
    Global,
    Local,
    Shared,
}

impl fmt::Display for MemSpace {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemSpace::Global => write!(f, "GLOBAL"),
            MemSpace::Local => write!(f, "LOCAL"),
            MemSpace::Shared => write!(f, "SHARED"),
        }
    }
}

pub struct MemAccess {
    pub addr_type: MemAddrType,
    pub mem_type: MemType,
    pub space: MemSpace,
    pub order: MemOrder,
    pub scope: MemScope,
}

impl fmt::Display for MemAccess {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}.{}.{}.{}",
            self.addr_type, self.mem_type, self.space, self.scope
        )
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum AtomType {
    F16x2,
    U32,
    I32,
    F32,
    U64,
    I64,
    F64,
}

impl AtomType {
    pub fn F(bits: u8) -> AtomType {
        match bits {
            16 => panic!("16-bit float atomics not yet supported"),
            32 => AtomType::F32,
            64 => AtomType::F64,
            _ => panic!("Invalid float atomic type"),
        }
    }

    pub fn U(bits: u8) -> AtomType {
        match bits {
            32 => AtomType::U32,
            64 => AtomType::U64,
            _ => panic!("Invalid uint atomic type"),
        }
    }

    pub fn I(bits: u8) -> AtomType {
        match bits {
            32 => AtomType::I32,
            64 => AtomType::I64,
            _ => panic!("Invalid int atomic type"),
        }
    }
}

impl fmt::Display for AtomType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AtomType::F16x2 => write!(f, "F16x2"),
            AtomType::U32 => write!(f, "U32"),
            AtomType::I32 => write!(f, "I32"),
            AtomType::F32 => write!(f, "F32"),
            AtomType::U64 => write!(f, "U64"),
            AtomType::I64 => write!(f, "I64"),
            AtomType::F64 => write!(f, "F64"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum AtomOp {
    Add,
    Min,
    Max,
    Inc,
    Dec,
    And,
    Or,
    Xor,
    Exch,
}

impl fmt::Display for AtomOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AtomOp::Add => write!(f, "ADD"),
            AtomOp::Min => write!(f, "MIN"),
            AtomOp::Max => write!(f, "MAX"),
            AtomOp::Inc => write!(f, "INC"),
            AtomOp::Dec => write!(f, "DEC"),
            AtomOp::And => write!(f, "AND"),
            AtomOp::Or => write!(f, "OR"),
            AtomOp::Xor => write!(f, "XOR"),
            AtomOp::Exch => write!(f, "EXCH"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum InterpFreq {
    Pass,
    Constant,
    State,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum InterpLoc {
    Default,
    Centroid,
    Offset,
}

pub struct AttrAccess {
    pub addr: u16,
    pub comps: u8,
    pub patch: bool,
    pub out_load: bool,
    pub flags: u8,
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFAdd {
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpFAdd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FADD")?;
        if self.saturate {
            write!(f, ".SAT")?;
        }
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, ".{}", self.rnd_mode)?;
        }
        write!(f, " {} {{ {}, {} }}", self.dst, self.srcs[0], self.srcs[1],)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFFma {
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 3],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpFFma {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FFMA")?;
        if self.saturate {
            write!(f, ".SAT")?;
        }
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, ".{}", self.rnd_mode)?;
        }
        write!(
            f,
            " {} {{ {}, {}, {} }}",
            self.dst, self.srcs[0], self.srcs[1], self.srcs[2]
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFMnMx {
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,
}

impl fmt::Display for OpFMnMx {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "FMNMX {} {{ {}, {} }} {}",
            self.dst, self.srcs[0], self.srcs[1], self.min
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFMul {
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpFMul {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FMUL")?;
        if self.saturate {
            write!(f, ".SAT")?;
        }
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, ".{}", self.rnd_mode)?;
        }
        write!(f, " {} {{ {}, {} }}", self.dst, self.srcs[0], self.srcs[1],)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSet {
    pub dst: Dst,
    pub cmp_op: FloatCmpOp,

    #[src_type(F32)]
    pub srcs: [Src; 2],
}

impl fmt::Display for OpFSet {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "FSET.{} {} {{ {}, {} }}",
            self.cmp_op, self.dst, self.srcs[0], self.srcs[1],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSetP {
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: FloatCmpOp,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,
}

impl fmt::Display for OpFSetP {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "FSETP.{} {} {{ {}, {} }}",
            self.cmp_op, self.dst, self.srcs[0], self.srcs[1],
        )
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum MuFuOp {
    Cos,
    Sin,
    Exp2,
    Log2,
    Rcp,
    Rsq,
    Rcp64H,
    Rsq64H,
    Sqrt,
    Tanh,
}

impl fmt::Display for MuFuOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MuFuOp::Cos => write!(f, "COS"),
            MuFuOp::Sin => write!(f, "SIN"),
            MuFuOp::Exp2 => write!(f, "EXP2"),
            MuFuOp::Log2 => write!(f, "LOG2"),
            MuFuOp::Rcp => write!(f, "RCP"),
            MuFuOp::Rsq => write!(f, "RSQ"),
            MuFuOp::Rcp64H => write!(f, "RCP64H"),
            MuFuOp::Rsq64H => write!(f, "RSQ64H"),
            MuFuOp::Sqrt => write!(f, "SQRT"),
            MuFuOp::Tanh => write!(f, "TANH"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMuFu {
    pub dst: Dst,
    pub op: MuFuOp,

    #[src_type(F32)]
    pub src: Src,
}

impl fmt::Display for OpMuFu {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "MUFU.{} {} {}", self.op, self.dst, self.src)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpDAdd {
    pub dst: Dst,

    #[src_type(F64)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpDAdd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "DADD")?;
        if self.saturate {
            write!(f, ".SAT")?;
        }
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, ".{}", self.rnd_mode)?;
        }
        write!(f, " {} {{ {}, {} }}", self.dst, self.srcs[0], self.srcs[1],)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIAbs {
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,
}

impl fmt::Display for OpIAbs {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "IABS {} {}", self.dst, self.src,)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpINeg {
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,
}

impl fmt::Display for OpINeg {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "INEG {} {}", self.dst, self.src,)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd3 {
    pub dst: Dst,
    pub overflow: Dst,

    #[src_type(I32)]
    pub srcs: [Src; 3],

    #[src_type(Pred)]
    pub carry: Src,
}

impl fmt::Display for OpIAdd3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "IADD3 {{ {} {} }} {{ {}, {}, {}, {} }}",
            self.dst,
            self.overflow,
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
            self.carry,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMad {
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub signed: bool,
}

impl fmt::Display for OpIMad {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "IMAD {} {{ {}, {}, {} }}",
            self.dst, self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMad64 {
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub signed: bool,
}

impl fmt::Display for OpIMad64 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "IMAD64 {} {{ {}, {}, {} }}",
            self.dst, self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMnMx {
    pub dst: Dst,
    pub cmp_type: IntCmpType,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,
}

impl fmt::Display for OpIMnMx {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "IMNMX.{} {} {{ {}, {} }} {}",
            self.cmp_type, self.dst, self.srcs[0], self.srcs[1], self.min
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpISetP {
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: IntCmpOp,
    pub cmp_type: IntCmpType,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,
}

impl fmt::Display for OpISetP {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "ISETP.{}.{} {} {{ {}, {} }}",
            self.cmp_op, self.cmp_type, self.dst, self.srcs[0], self.srcs[1],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLop3 {
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub op: LogicOp,
}

impl fmt::Display for OpLop3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "LOP3.{} {} {{ {}, {}, {} }}",
            self.op, self.dst, self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpShf {
    pub dst: Dst,

    #[src_type(GPR)]
    pub low: Src,

    #[src_type(ALU)]
    pub high: Src,

    #[src_type(GPR)]
    pub shift: Src,

    pub right: bool,
    pub wrap: bool,
    pub data_type: IntType,
    pub dst_high: bool,
}

impl fmt::Display for OpShf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SHF")?;
        if self.right {
            write!(f, ".R")?;
        } else {
            write!(f, ".L")?;
        }
        if self.wrap {
            write!(f, ".W")?;
        }
        write!(f, ".{}", self.data_type)?;
        if self.dst_high {
            write!(f, ".HI")?;
        }
        write!(
            f,
            " {} {{ {}, {} }} {}",
            self.dst, self.low, self.high, self.shift
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpF2F {
    pub dst: Dst,

    #[src_type(F32)]
    pub src: Src,

    pub src_type: FloatType,
    pub dst_type: FloatType,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
}

impl fmt::Display for OpF2F {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "F2F")?;
        if self.ftz {
            write!(f, ".FTZ")?;
        }
        write!(
            f,
            ".{}.{}.{} {} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.dst, self.src,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpF2I {
    pub dst: Dst,

    #[src_type(F32)]
    pub src: Src,

    pub src_type: FloatType,
    pub dst_type: IntType,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpF2I {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "F2I.{}.{}.{} {} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.dst, self.src,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpI2F {
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,

    pub dst_type: FloatType,
    pub src_type: IntType,
    pub rnd_mode: FRndMode,
}

impl fmt::Display for OpI2F {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "I2F.{}.{}.{} {} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.dst, self.src,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMov {
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,

    pub quad_lanes: u8,
}

impl fmt::Display for OpMov {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.quad_lanes == 0xf {
            write!(f, "MOV {} {}", self.dst, self.src)
        } else {
            write!(f, "MOV[{:#x}] {} {}", self.quad_lanes, self.dst, self.src)
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSel {
    pub dst: Dst,

    #[src_type(Pred)]
    pub cond: Src,

    #[src_type(ALU)]
    pub srcs: [Src; 2],
}

impl fmt::Display for OpSel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SEL {} {{ {}, {}, {} }}",
            self.dst, self.cond, self.srcs[0], self.srcs[1],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPLop3 {
    pub dsts: [Dst; 2],

    #[src_type(Pred)]
    pub srcs: [Src; 3],

    pub ops: [LogicOp; 2],
}

impl fmt::Display for OpPLop3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "PLOP3 {{ {}, {} }} {{ {}, {}, {} }} {} {}",
            self.dsts[0],
            self.dsts[1],
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
            self.ops[0],
            self.ops[1],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTex {
    pub dsts: [Dst; 2],
    pub resident: Dst,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub lod_mode: TexLodMode,
    pub z_cmpr: bool,
    pub offset: bool,
    pub mask: u8,
}

impl fmt::Display for OpTex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TEX.B")?;
        if self.lod_mode != TexLodMode::Auto {
            write!(f, ".{}", self.lod_mode)?;
        }
        if self.offset {
            write!(f, ".AOFFI")?;
        }
        if self.z_cmpr {
            write!(f, ".DC")?;
        }
        write!(
            f,
            " {{ {}, {}, {} }} {{ {}, {} }} {}",
            self.dsts[0],
            self.dsts[1],
            self.resident,
            self.srcs[0],
            self.srcs[1],
            self.dim,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTld {
    pub dsts: [Dst; 2],
    pub resident: Dst,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub is_ms: bool,
    pub lod_mode: TexLodMode,
    pub offset: bool,
    pub mask: u8,
}

impl fmt::Display for OpTld {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TLD.B")?;
        if self.lod_mode != TexLodMode::Auto {
            write!(f, ".{}", self.lod_mode)?;
        }
        if self.offset {
            write!(f, ".AOFFI")?;
        }
        if self.is_ms {
            write!(f, ".MS")?;
        }
        write!(
            f,
            " {{ {}, {}, {} }} {{ {}, {} }} {}",
            self.dsts[0],
            self.dsts[1],
            self.resident,
            self.srcs[0],
            self.srcs[1],
            self.dim,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTld4 {
    pub dsts: [Dst; 2],
    pub resident: Dst,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub comp: u8,
    pub offset_mode: Tld4OffsetMode,
    pub z_cmpr: bool,
    pub mask: u8,
}

impl fmt::Display for OpTld4 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TLD4.G.B")?;
        if self.offset_mode != Tld4OffsetMode::None {
            write!(f, ".{}", self.offset_mode)?;
        }
        write!(
            f,
            " {{ {}, {}, {} }} {{ {}, {} }} {}",
            self.dsts[0],
            self.dsts[1],
            self.resident,
            self.srcs[0],
            self.srcs[1],
            self.dim,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTmml {
    pub dsts: [Dst; 2],

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub mask: u8,
}

impl fmt::Display for OpTmml {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "TMML.B.LOD {{ {}, {} }} {{ {}, {} }} {}",
            self.dsts[0], self.dsts[1], self.srcs[0], self.srcs[1], self.dim
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTxd {
    pub dsts: [Dst; 2],
    pub resident: Dst,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub offset: bool,
    pub mask: u8,
}

impl fmt::Display for OpTxd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TXD.B")?;
        if self.offset {
            write!(f, ".AOFFI")?;
        }
        write!(
            f,
            " {{ {}, {}, {} }} {{ {}, {} }} {}",
            self.dsts[0],
            self.dsts[1],
            self.resident,
            self.srcs[0],
            self.srcs[1],
            self.dim,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTxq {
    pub dsts: [Dst; 2],

    #[src_type(SSA)]
    pub src: Src,

    pub query: TexQuery,
    pub mask: u8,
}

impl fmt::Display for OpTxq {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "TXQ.B {{ {}, {} }} {} {}",
            self.dsts[0], self.dsts[1], self.src, self.query
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuLd {
    pub dst: Dst,
    pub resident: Dst,

    pub image_dim: ImageDim,
    pub mem_order: MemOrder,
    pub mem_scope: MemScope,
    pub mask: u8,

    #[src_type(GPR)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,
}

impl fmt::Display for OpSuLd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SULD.P.{}.{}.{} {{ {} {} }} [{}] {}",
            self.image_dim,
            self.mem_order,
            self.mem_scope,
            self.dst,
            self.resident,
            self.coord,
            self.handle,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuSt {
    pub image_dim: ImageDim,
    pub mem_order: MemOrder,
    pub mem_scope: MemScope,
    pub mask: u8,

    #[src_type(GPR)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,

    #[src_type(SSA)]
    pub data: Src,
}

impl fmt::Display for OpSuSt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SUST.P.{}.{}.{} [{}] {} {}",
            self.image_dim,
            self.mem_order,
            self.mem_scope,
            self.coord,
            self.data,
            self.handle,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuAtom {
    pub dst: Dst,
    pub resident: Dst,

    pub image_dim: ImageDim,

    pub atom_op: AtomOp,
    pub atom_type: AtomType,

    pub mem_order: MemOrder,
    pub mem_scope: MemScope,

    #[src_type(GPR)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,

    #[src_type(SSA)]
    pub data: Src,
}

impl fmt::Display for OpSuAtom {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SUATOM.P.{}.{}.{}.{}.{} [{}] {} {}",
            self.image_dim,
            self.atom_op,
            self.atom_type,
            self.mem_order,
            self.mem_scope,
            self.coord,
            self.data,
            self.handle,
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLd {
    pub dst: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    pub offset: i32,
    pub access: MemAccess,
}

impl fmt::Display for OpLd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "LD.{} {} [{}", self.access, self.dst, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "]")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSt {
    #[src_type(GPR)]
    pub addr: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub offset: i32,
    pub access: MemAccess,
}

impl fmt::Display for OpSt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ST.{} [{}", self.access, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpAtom {
    pub dst: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub atom_op: AtomOp,
    pub atom_type: AtomType,

    pub addr_type: MemAddrType,
    pub addr_offset: i32,

    pub mem_space: MemSpace,
    pub mem_order: MemOrder,
    pub mem_scope: MemScope,
}

impl fmt::Display for OpAtom {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "ATOM.{}.{}.{}.{} {}",
            self.atom_op,
            self.atom_type,
            self.mem_order,
            self.mem_scope,
            self.dst
        )?;
        write!(f, " [")?;
        if !self.addr.is_zero() {
            write!(f, "{}", self.addr)?;
        }
        if self.addr_offset > 0 {
            if !self.addr.is_zero() {
                write!(f, "+")?;
            }
            write!(f, "{:#x}", self.addr_offset)?;
        }
        write!(f, "] {}", self.data)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpAtomCas {
    pub dst: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    #[src_type(SSA)]
    pub cmpr: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub atom_type: AtomType,

    pub addr_type: MemAddrType,
    pub addr_offset: i32,

    pub mem_space: MemSpace,
    pub mem_order: MemOrder,
    pub mem_scope: MemScope,
}

impl fmt::Display for OpAtomCas {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "ATOM.CAS.{}.{}.{} {}",
            self.atom_type, self.mem_order, self.mem_scope, self.dst
        )?;
        write!(f, " [")?;
        if !self.addr.is_zero() {
            write!(f, "{}", self.addr)?;
        }
        if self.addr_offset > 0 {
            if !self.addr.is_zero() {
                write!(f, "+")?;
            }
            write!(f, "+{:#x}", self.addr_offset)?;
        }
        write!(f, "] {} {}", self.cmpr, self.data)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpALd {
    pub dst: Dst,

    #[src_type(GPR)]
    pub vtx: Src,

    #[src_type(GPR)]
    pub offset: Src,

    pub access: AttrAccess,
}

impl fmt::Display for OpALd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ALD {} a", self.dst)?;
        if !self.vtx.is_zero() {
            write!(f, "[{}]", self.vtx)?;
        }
        write!(f, "[{:#x}", self.access.addr)?;
        if !self.offset.is_zero() {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "]")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpASt {
    #[src_type(GPR)]
    pub vtx: Src,

    #[src_type(GPR)]
    pub offset: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub access: AttrAccess,
}

impl fmt::Display for OpASt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ALD a")?;
        if !self.vtx.is_zero() {
            write!(f, "[{}]", self.vtx)?;
        }
        write!(f, "[{:#x}", self.access.addr)?;
        if !self.offset.is_zero() {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIpa {
    pub dst: Dst,
    pub addr: u16,
    pub freq: InterpFreq,
    pub loc: InterpLoc,
    pub offset: Src,
}

impl fmt::Display for OpIpa {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "IPA")?;
        match self.freq {
            InterpFreq::Pass => (),
            InterpFreq::Constant => write!(f, ".CONSTANT")?,
            InterpFreq::State => write!(f, ".STATE")?,
        }
        match self.loc {
            InterpLoc::Default => (),
            InterpLoc::Centroid => write!(f, ".CENTROID")?,
            InterpLoc::Offset => write!(f, ".OFFSET")?,
        }

        write!(f, " {} a[{:#x}]", self.dst, self.addr)?;
        if !self.offset.is_zero() {
            write!(f, " {}", self.offset)?;
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMemBar {
    pub scope: MemScope,
}

impl fmt::Display for OpMemBar {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "MEMBAR.SC.{}", self.scope)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBra {
    pub target: u32,
}

impl fmt::Display for OpBra {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "BRA B{}", self.target)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpExit {}

impl fmt::Display for OpExit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "EXIT")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBar {}

impl fmt::Display for OpBar {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "BAR.SYNC")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpS2R {
    pub dst: Dst,
    pub idx: u8,
}

impl fmt::Display for OpS2R {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "S2R {} sr[{:#x}]", self.dst, self.idx)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpUndef {
    pub dst: Dst,
}

impl fmt::Display for OpUndef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "UNDEF {}", self.dst)
    }
}

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpPhiSrcs {
    pub srcs: Vec<Src>,
    pub ids: Vec<u32>,
}

impl OpPhiSrcs {
    pub fn new() -> OpPhiSrcs {
        OpPhiSrcs {
            srcs: Vec::new(),
            ids: Vec::new(),
        }
    }

    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        assert!(self.ids.len() == self.srcs.len());
        self.ids.is_empty()
    }

    pub fn iter(&self) -> Zip<slice::Iter<'_, u32>, slice::Iter<'_, Src>> {
        assert!(self.ids.len() == self.srcs.len());
        self.ids.iter().zip(self.srcs.iter())
    }

    pub fn push(&mut self, id: u32, src: Src) {
        assert!(self.ids.len() == self.srcs.len());
        self.ids.push(id);
        self.srcs.push(src);
    }
}

impl SrcsAsSlice for OpPhiSrcs {
    fn srcs_as_slice(&self) -> &[Src] {
        &self.srcs
    }

    fn srcs_as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs
    }

    fn src_types(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl fmt::Display for OpPhiSrcs {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PHI_SRC {{")?;
        assert!(self.ids.len() == self.srcs.len());
        for i in 0..self.ids.len() {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, " {} <- {}", self.ids[i], self.srcs[i])?;
        }
        write!(f, " }}")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice)]
pub struct OpPhiDsts {
    pub ids: Vec<u32>,
    pub dsts: Vec<Dst>,
}

impl OpPhiDsts {
    pub fn new() -> OpPhiDsts {
        OpPhiDsts {
            ids: Vec::new(),
            dsts: Vec::new(),
        }
    }

    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        assert!(self.ids.len() == self.dsts.len());
        self.ids.is_empty()
    }

    pub fn iter(&self) -> Zip<slice::Iter<'_, u32>, slice::Iter<'_, Dst>> {
        assert!(self.ids.len() == self.dsts.len());
        self.ids.iter().zip(self.dsts.iter())
    }

    pub fn push(&mut self, id: u32, dst: Dst) {
        assert!(self.ids.len() == self.dsts.len());
        self.ids.push(id);
        self.dsts.push(dst);
    }
}

impl DstsAsSlice for OpPhiDsts {
    fn dsts_as_slice(&self) -> &[Dst] {
        &self.dsts
    }

    fn dsts_as_mut_slice(&mut self) -> &mut [Dst] {
        &mut self.dsts
    }
}

impl fmt::Display for OpPhiDsts {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PHI_DST {{")?;
        assert!(self.ids.len() == self.dsts.len());
        for i in 0..self.ids.len() {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, " {} <- {}", self.dsts[i], self.ids[i])?;
        }
        write!(f, " }}")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSwap {
    pub dsts: [Dst; 2],
    pub srcs: [Src; 2],
}

impl fmt::Display for OpSwap {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SWAP {{ {} {} }} {{ {} {} }}",
            self.dsts[0], self.dsts[1], self.srcs[0], self.srcs[1]
        )
    }
}

#[repr(C)]
pub struct OpParCopy {
    pub dsts: Vec<Dst>,
    pub srcs: Vec<Src>,
}

impl OpParCopy {
    pub fn new() -> OpParCopy {
        OpParCopy {
            dsts: Vec::new(),
            srcs: Vec::new(),
        }
    }

    pub fn is_empty(&self) -> bool {
        assert!(self.srcs.len() == self.dsts.len());
        self.srcs.is_empty()
    }

    pub fn iter(&self) -> Zip<slice::Iter<'_, Src>, slice::Iter<'_, Dst>> {
        assert!(self.srcs.len() == self.dsts.len());
        self.srcs.iter().zip(&self.dsts)
    }

    pub fn push(&mut self, src: Src, dst: Dst) {
        assert!(self.srcs.len() == self.dsts.len());
        self.srcs.push(src);
        self.dsts.push(dst);
    }
}

impl SrcsAsSlice for OpParCopy {
    fn srcs_as_slice(&self) -> &[Src] {
        &self.srcs
    }

    fn srcs_as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs
    }

    fn src_types(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl DstsAsSlice for OpParCopy {
    fn dsts_as_slice(&self) -> &[Dst] {
        &self.dsts
    }

    fn dsts_as_mut_slice(&mut self) -> &mut [Dst] {
        &mut self.dsts
    }
}

impl fmt::Display for OpParCopy {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PAR_COPY {{")?;
        assert!(self.srcs.len() == self.dsts.len());
        for i in 0..self.srcs.len() {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, " {} <- {}", self.dsts[i], self.srcs[i])?;
        }
        write!(f, " }}")
    }
}

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpFSOut {
    pub srcs: Vec<Src>,
}

impl SrcsAsSlice for OpFSOut {
    fn srcs_as_slice(&self) -> &[Src] {
        &self.srcs
    }

    fn srcs_as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs
    }

    fn src_types(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl fmt::Display for OpFSOut {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FS_OUT {{ {}", self.srcs[0])?;
        for src in &self.srcs[1..] {
            write!(f, " {}", src)?;
        }
        write!(f, "}}")
    }
}

#[derive(Display, DstsAsSlice, SrcsAsSlice, FromVariants)]
pub enum Op {
    FAdd(OpFAdd),
    FFma(OpFFma),
    FMnMx(OpFMnMx),
    FMul(OpFMul),
    MuFu(OpMuFu),
    FSet(OpFSet),
    FSetP(OpFSetP),
    DAdd(OpDAdd),
    IAbs(OpIAbs),
    INeg(OpINeg),
    IAdd3(OpIAdd3),
    IMad(OpIMad),
    IMad64(OpIMad64),
    IMnMx(OpIMnMx),
    ISetP(OpISetP),
    Lop3(OpLop3),
    Shf(OpShf),
    F2F(OpF2F),
    F2I(OpF2I),
    I2F(OpI2F),
    Mov(OpMov),
    Sel(OpSel),
    PLop3(OpPLop3),
    Tex(OpTex),
    Tld(OpTld),
    Tld4(OpTld4),
    Tmml(OpTmml),
    Txd(OpTxd),
    Txq(OpTxq),
    SuLd(OpSuLd),
    SuSt(OpSuSt),
    SuAtom(OpSuAtom),
    Ld(OpLd),
    St(OpSt),
    Atom(OpAtom),
    AtomCas(OpAtomCas),
    ALd(OpALd),
    ASt(OpASt),
    Ipa(OpIpa),
    MemBar(OpMemBar),
    Bra(OpBra),
    Exit(OpExit),
    Bar(OpBar),
    S2R(OpS2R),
    Undef(OpUndef),
    PhiSrcs(OpPhiSrcs),
    PhiDsts(OpPhiDsts),
    Swap(OpSwap),
    ParCopy(OpParCopy),
    FSOut(OpFSOut),
}

pub enum PredRef {
    None,
    SSA(SSAValue),
    Reg(RegRef),
}

impl PredRef {
    #[allow(dead_code)]
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            PredRef::Reg(r) => Some(r),
            _ => None,
        }
    }

    #[allow(dead_code)]
    pub fn as_ssa(&self) -> Option<&SSAValue> {
        match self {
            PredRef::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn is_none(&self) -> bool {
        match self {
            PredRef::None => true,
            _ => false,
        }
    }
}

impl From<RegRef> for PredRef {
    fn from(reg: RegRef) -> PredRef {
        PredRef::Reg(reg)
    }
}

impl From<SSAValue> for PredRef {
    fn from(ssa: SSAValue) -> PredRef {
        PredRef::SSA(ssa)
    }
}

impl fmt::Display for PredRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PredRef::None => write!(f, "PT"),
            PredRef::SSA(ssa) => ssa.fmt(f),
            PredRef::Reg(reg) => reg.fmt(f),
        }
    }
}

pub struct Pred {
    pub pred_ref: PredRef,
    pub pred_inv: bool,
}

impl Pred {
    pub fn is_true(&self) -> bool {
        self.pred_ref.is_none() && !self.pred_inv
    }

    pub fn is_false(&self) -> bool {
        self.pred_ref.is_none() && self.pred_inv
    }
}

impl<T: Into<PredRef>> From<T> for Pred {
    fn from(p: T) -> Self {
        Pred {
            pred_ref: p.into(),
            pred_inv: false,
        }
    }
}

impl fmt::Display for Pred {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.pred_inv {
            write!(f, "!")?;
        }
        self.pred_ref.fmt(f)
    }
}

pub const MIN_INSTR_DELAY: u8 = 1;
pub const MAX_INSTR_DELAY: u8 = 15;

pub struct InstrDeps {
    pub delay: u8,
    pub yld: bool,
    wr_bar: i8,
    rd_bar: i8,
    pub wt_bar_mask: u8,
    pub reuse_mask: u8,
}

impl InstrDeps {
    pub fn new() -> InstrDeps {
        InstrDeps {
            delay: MAX_INSTR_DELAY,
            yld: false,
            wr_bar: -1,
            rd_bar: -1,
            wt_bar_mask: 0,
            reuse_mask: 0,
        }
    }

    pub fn rd_bar(&self) -> Option<u8> {
        if self.rd_bar < 0 {
            None
        } else {
            Some(self.rd_bar.try_into().unwrap())
        }
    }

    pub fn wr_bar(&self) -> Option<u8> {
        if self.wr_bar < 0 {
            None
        } else {
            Some(self.wr_bar.try_into().unwrap())
        }
    }

    pub fn set_delay(&mut self, delay: u8) {
        assert!(delay <= MAX_INSTR_DELAY);
        self.delay = delay;
    }

    pub fn set_yield(&mut self, yld: bool) {
        self.yld = yld;
    }

    pub fn set_rd_bar(&mut self, idx: u8) {
        assert!(idx < 6);
        self.rd_bar = idx.try_into().unwrap();
    }

    pub fn set_wr_bar(&mut self, idx: u8) {
        assert!(idx < 6);
        self.wr_bar = idx.try_into().unwrap();
    }

    pub fn add_wt_bar(&mut self, idx: u8) {
        self.add_wt_bar_mask(1 << idx);
    }

    pub fn add_wt_bar_mask(&mut self, bar_mask: u8) {
        assert!(bar_mask < 1 << 6);
        self.wt_bar_mask |= bar_mask;
    }

    #[allow(dead_code)]
    pub fn add_reuse(&mut self, idx: u8) {
        assert!(idx < 6);
        self.reuse_mask |= 1_u8 << idx;
    }
}

impl fmt::Display for InstrDeps {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "delay={}", self.delay)?;
        if self.wt_bar_mask != 0 {
            write!(f, " wt={:06b}", self.wt_bar_mask)?;
        }
        if self.rd_bar >= 0 {
            write!(f, " rd:{}", self.rd_bar)?;
        }
        if self.wr_bar >= 0 {
            write!(f, " wr:{}", self.wr_bar)?;
        }
        if self.reuse_mask != 0 {
            write!(f, " reuse={:06b}", self.reuse_mask)?;
        }
        Ok(())
    }
}

pub struct Instr {
    pub pred: Pred,
    pub op: Op,
    pub deps: InstrDeps,
}

impl Instr {
    pub fn new(op: impl Into<Op>) -> Instr {
        Instr {
            op: op.into(),
            pred: PredRef::None.into(),
            deps: InstrDeps::new(),
        }
    }

    pub fn new_fadd(dst: Dst, x: Src, y: Src) -> Instr {
        OpFAdd {
            dst: dst,
            srcs: [x, y],
            saturate: false,
            rnd_mode: FRndMode::NearestEven,
        }
        .into()
    }

    pub fn new_fmul(dst: Dst, x: Src, y: Src) -> Instr {
        OpFMul {
            dst: dst,
            srcs: [x, y],
            saturate: false,
            rnd_mode: FRndMode::NearestEven,
        }
        .into()
    }

    pub fn new_fset(dst: Dst, cmp_op: FloatCmpOp, x: Src, y: Src) -> Instr {
        OpFSet {
            dst: dst,
            cmp_op: cmp_op,
            srcs: [x, y],
        }
        .into()
    }

    pub fn new_fsetp(dst: Dst, cmp_op: FloatCmpOp, x: Src, y: Src) -> Instr {
        OpFSetP {
            dst: dst,
            set_op: PredSetOp::And,
            cmp_op: cmp_op,
            srcs: [x, y],
            accum: SrcRef::True.into(),
        }
        .into()
    }

    pub fn new_mufu(dst: Dst, op: MuFuOp, src: Src) -> Instr {
        OpMuFu {
            dst: dst,
            op: op,
            src: src,
        }
        .into()
    }

    pub fn new_iadd(dst: Dst, x: Src, y: Src) -> Instr {
        OpIAdd3 {
            dst: dst,
            overflow: Dst::None,
            srcs: [Src::new_zero(), x, y],
            carry: Src::new_imm_bool(false),
        }
        .into()
    }

    pub fn new_i2f(dst: Dst, src: Src) -> Instr {
        OpI2F {
            dst: dst,
            src: src,
            dst_type: FloatType::F32,
            src_type: IntType::I32,
            rnd_mode: FRndMode::NearestEven,
        }
        .into()
    }

    pub fn new_u2f(dst: Dst, src: Src) -> Instr {
        OpI2F {
            dst: dst,
            src: src,
            dst_type: FloatType::F32,
            src_type: IntType::U32,
            rnd_mode: FRndMode::NearestEven,
        }
        .into()
    }

    pub fn new_isetp(
        dst: Dst,
        cmp_type: IntCmpType,
        cmp_op: IntCmpOp,
        x: Src,
        y: Src,
    ) -> Instr {
        OpISetP {
            dst: dst,
            set_op: PredSetOp::And,
            cmp_op: cmp_op,
            cmp_type: cmp_type,
            srcs: [x, y],
            accum: SrcRef::True.into(),
        }
        .into()
    }

    pub fn new_lop2(dst: Dst, op: LogicOp, x: Src, y: Src) -> Instr {
        /* Only uses x and y */
        assert!(op.eval(0x5, 0x3, 0x0) == op.eval(0x5, 0x3, 0xf));

        let is_predicate = match dst {
            Dst::None => panic!("No LOP destination"),
            Dst::SSA(ssa) => ssa.is_predicate(),
            Dst::Reg(reg) => reg.is_predicate(),
        };
        assert!(x.src_ref.is_predicate() == is_predicate);
        assert!(x.src_ref.is_predicate() == is_predicate);

        if is_predicate {
            OpPLop3 {
                dsts: [dst, Dst::None],
                srcs: [x, y, Src::new_imm_bool(true)],
                ops: [op, LogicOp::new_const(false)],
            }
            .into()
        } else {
            OpLop3 {
                dst: dst,
                srcs: [x, y, Src::new_zero()],
                op: op,
            }
            .into()
        }
    }

    pub fn new_xor(dst: Dst, x: Src, y: Src) -> Instr {
        let xor_lop = LogicOp::new_lut(&|x, y, _| x ^ y);
        Instr::new_lop2(dst, xor_lop, x, y)
    }

    pub fn new_mov(dst: Dst, src: Src) -> Instr {
        OpMov {
            dst: dst,
            src: src,
            quad_lanes: 0xf,
        }
        .into()
    }

    pub fn new_sel(dst: Dst, sel: Src, x: Src, y: Src) -> Instr {
        OpSel {
            dst: dst,
            cond: sel,
            srcs: [x, y],
        }
        .into()
    }

    pub fn new_plop3(dst: Dst, op: LogicOp, x: Src, y: Src, z: Src) -> Instr {
        assert!(x.is_predicate() && y.is_predicate() && z.is_predicate());
        OpPLop3 {
            dsts: [dst, Dst::None],
            srcs: [x, y, z],
            ops: [op, LogicOp::new_const(false)],
        }
        .into()
    }

    pub fn new_ld(
        dst: Dst,
        access: MemAccess,
        addr: Src,
        offset: i32,
    ) -> Instr {
        OpLd {
            dst: dst,
            addr: addr,
            offset: offset,
            access: access,
        }
        .into()
    }

    pub fn new_st(
        access: MemAccess,
        addr: Src,
        offset: i32,
        data: Src,
    ) -> Instr {
        OpSt {
            addr: addr,
            data: data,
            offset: offset,
            access: access,
        }
        .into()
    }

    pub fn new_ald(dst: Dst, attr_addr: u16, vtx: Src, offset: Src) -> Instr {
        OpALd {
            dst: dst,
            vtx: vtx,
            offset: offset,
            access: AttrAccess {
                addr: attr_addr,
                comps: dst.as_ssa().unwrap().comps(),
                patch: false,
                out_load: false,
                flags: 0,
            },
        }
        .into()
    }

    pub fn new_ast(attr_addr: u16, data: Src, vtx: Src, offset: Src) -> Instr {
        OpASt {
            vtx: vtx,
            offset: offset,
            data: data,
            access: AttrAccess {
                addr: attr_addr,
                comps: data.src_ref.as_ssa().unwrap().comps(),
                patch: false,
                out_load: false,
                flags: 0,
            },
        }
        .into()
    }

    pub fn new_bra(block: u32) -> Instr {
        OpBra { target: block }.into()
    }

    pub fn new_exit() -> Instr {
        OpExit {}.into()
    }

    pub fn new_s2r(dst: Dst, idx: u8) -> Instr {
        OpS2R { dst: dst, idx: idx }.into()
    }

    pub fn new_swap(x: RegRef, y: RegRef) -> Instr {
        assert!(x.file() == y.file());
        OpSwap {
            dsts: [x.into(), y.into()],
            srcs: [y.into(), x.into()],
        }
        .into()
    }

    pub fn new_fs_out(srcs: &[Src]) -> Instr {
        OpFSOut {
            srcs: srcs.to_vec(),
        }
        .into()
    }

    pub fn dsts(&self) -> &[Dst] {
        self.op.dsts_as_slice()
    }

    pub fn dsts_mut(&mut self) -> &mut [Dst] {
        self.op.dsts_as_mut_slice()
    }

    pub fn srcs(&self) -> &[Src] {
        self.op.srcs_as_slice()
    }

    pub fn srcs_mut(&mut self) -> &mut [Src] {
        self.op.srcs_as_mut_slice()
    }

    pub fn src_types(&self) -> SrcTypeList {
        self.op.src_types()
    }

    pub fn is_branch(&self) -> bool {
        match self.op {
            Op::Bra(_) | Op::Exit(_) => true,
            _ => false,
        }
    }

    pub fn is_barrier(&self) -> bool {
        match self.op {
            Op::Bar(_) => true,
            _ => false,
        }
    }

    pub fn can_eliminate(&self) -> bool {
        match self.op {
            Op::ASt(_)
            | Op::SuSt(_)
            | Op::SuAtom(_)
            | Op::St(_)
            | Op::Atom(_)
            | Op::AtomCas(_)
            | Op::MemBar(_)
            | Op::Bra(_)
            | Op::Exit(_)
            | Op::Bar(_)
            | Op::FSOut(_) => false,
            _ => true,
        }
    }

    pub fn get_latency(&self) -> Option<u32> {
        match self.op {
            Op::FAdd(_)
            | Op::FFma(_)
            | Op::FMnMx(_)
            | Op::FMul(_)
            | Op::FSet(_)
            | Op::FSetP(_)
            | Op::MuFu(_)
            | Op::DAdd(_)
            | Op::IAbs(_)
            | Op::INeg(_)
            | Op::IAdd3(_)
            | Op::IMad(_)
            | Op::IMad64(_)
            | Op::IMnMx(_)
            | Op::Lop3(_)
            | Op::PLop3(_)
            | Op::ISetP(_)
            | Op::Shf(_) => Some(6),
            Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::Mov(_) => Some(15),
            Op::Sel(_) => Some(15),
            Op::S2R(_) => None,
            Op::ALd(_) => None,
            Op::ASt(_) => Some(15),
            Op::Ipa(_) => None,
            Op::Tex(_) => None,
            Op::Tld(_) => None,
            Op::Tld4(_) => None,
            Op::Tmml(_) => None,
            Op::Txd(_) => None,
            Op::Txq(_) => None,
            Op::SuLd(_) => None,
            Op::SuSt(_) => None,
            Op::SuAtom(_) => None,
            Op::Ld(_) => None,
            Op::St(_) => None,
            Op::Atom(_) => None,
            Op::AtomCas(_) => None,
            Op::MemBar(_) => None,
            Op::Bar(_) => None,
            Op::Bra(_) | Op::Exit(_) => Some(15),
            Op::Undef(_)
            | Op::PhiSrcs(_)
            | Op::PhiDsts(_)
            | Op::Swap(_)
            | Op::ParCopy(_)
            | Op::FSOut(_) => {
                panic!("Not a hardware opcode")
            }
        }
    }
}

impl fmt::Display for Instr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if !self.pred.is_true() {
            write!(f, "@{} ", self.pred)?;
        }
        write!(f, "{} {}", self.op, self.deps)
    }
}

impl<T: Into<Op>> From<T> for Instr {
    fn from(value: T) -> Self {
        Self::new(value)
    }
}

pub struct BasicBlock {
    pub id: u32,
    pub instrs: Vec<Instr>,
}

impl BasicBlock {
    pub fn new(id: u32) -> BasicBlock {
        BasicBlock {
            id: id,
            instrs: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr, &mut SSAValueAllocator) -> Vec<Instr>>(
        &mut self,
        map: &F,
        ssa_alloc: &mut SSAValueAllocator,
    ) {
        let mut instrs = Vec::new();
        for i in self.instrs.drain(..) {
            instrs.append(&mut map(i, ssa_alloc));
        }
        self.instrs = instrs;
    }

    pub fn branch(&self) -> Option<&Instr> {
        if let Some(i) = self.instrs.last() {
            if i.is_branch() {
                Some(i)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn branch_mut(&mut self) -> Option<&mut Instr> {
        if let Some(i) = self.instrs.last_mut() {
            if i.is_branch() {
                Some(i)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn falls_through(&self) -> bool {
        if let Some(i) = self.branch() {
            !i.pred.is_true()
        } else {
            true
        }
    }
}

impl fmt::Display for BasicBlock {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "block {} {{\n", self.id)?;
        for i in &self.instrs {
            write!(f, "    {}\n", i)?;
        }
        write!(f, "}}\n")
    }
}

pub struct Function {
    pub ssa_alloc: SSAValueAllocator,
    pub blocks: Vec<BasicBlock>,
}

impl Function {
    pub fn new(_id: u32) -> Function {
        Function {
            ssa_alloc: SSAValueAllocator::new(),
            blocks: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr, &mut SSAValueAllocator) -> Vec<Instr>>(
        &mut self,
        map: &F,
    ) {
        for b in &mut self.blocks {
            b.map_instrs(map, &mut self.ssa_alloc);
        }
    }
}

impl fmt::Display for Function {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for b in &self.blocks {
            write!(f, "{}", b)?;
        }
        Ok(())
    }
}

pub struct Shader {
    pub sm: u8,
    pub functions: Vec<Function>,
}

impl Shader {
    pub fn new(sm: u8) -> Shader {
        Shader {
            sm: sm,
            functions: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr, &mut SSAValueAllocator) -> Vec<Instr>>(
        &mut self,
        map: &F,
    ) {
        for f in &mut self.functions {
            f.map_instrs(map);
        }
    }

    pub fn lower_vec_split(&mut self) {
        self.map_instrs(&|instr: Instr, _| -> Vec<Instr> {
            match instr.op {
                Op::INeg(neg) => {
                    vec![OpIAdd3 {
                        dst: neg.dst,
                        overflow: Dst::None,
                        srcs: [
                            Src::new_zero(),
                            neg.src.ineg(),
                            Src::new_zero(),
                        ],
                        carry: Src::new_imm_bool(false),
                    }
                    .into()]
                }
                Op::FSOut(out) => {
                    let mut pcopy = OpParCopy::new();
                    for (i, src) in out.srcs.iter().enumerate() {
                        let dst =
                            RegRef::new(RegFile::GPR, i.try_into().unwrap(), 1);
                        pcopy.srcs.push(*src);
                        pcopy.dsts.push(dst.into());
                    }
                    vec![pcopy.into()]
                }
                _ => vec![instr],
            }
        })
    }

    pub fn lower_swap(&mut self) {
        self.map_instrs(&|instr: Instr, _| -> Vec<Instr> {
            match instr.op {
                Op::Swap(swap) => {
                    let x = *swap.dsts[0].as_reg().unwrap();
                    let y = *swap.dsts[1].as_reg().unwrap();

                    assert!(x.file() == y.file());
                    assert!(x.comps() == 1 && y.comps() == 1);
                    assert!(swap.srcs[0].src_mod.is_none());
                    assert!(*swap.srcs[0].src_ref.as_reg().unwrap() == y);
                    assert!(swap.srcs[1].src_mod.is_none());
                    assert!(*swap.srcs[1].src_ref.as_reg().unwrap() == x);

                    if x == y {
                        Vec::new()
                    } else if x.is_predicate() {
                        vec![OpPLop3 {
                            dsts: [x.into(), y.into()],
                            srcs: [x.into(), y.into(), Src::new_imm_bool(true)],
                            ops: [
                                LogicOp::new_lut(&|_, y, _| y),
                                LogicOp::new_lut(&|x, _, _| x),
                            ],
                        }
                        .into()]
                    } else {
                        vec![
                            Instr::new_xor(x.into(), x.into(), y.into()),
                            Instr::new_xor(y.into(), x.into(), y.into()),
                            Instr::new_xor(x.into(), x.into(), y.into()),
                        ]
                    }
                }
                _ => vec![instr],
            }
        })
    }

    pub fn lower_mov_predicate(&mut self) {
        self.map_instrs(&|instr: Instr, _| -> Vec<Instr> {
            match &instr.op {
                Op::Mov(mov) => {
                    assert!(mov.src.src_mod.is_none());
                    match mov.src.src_ref {
                        SrcRef::True => {
                            vec![Instr::new_isetp(
                                mov.dst,
                                IntCmpType::I32,
                                IntCmpOp::Eq,
                                Src::new_zero(),
                                Src::new_zero(),
                            )]
                        }
                        SrcRef::False => {
                            vec![Instr::new_isetp(
                                mov.dst,
                                IntCmpType::I32,
                                IntCmpOp::Ne,
                                Src::new_zero(),
                                Src::new_zero(),
                            )]
                        }
                        SrcRef::Reg(reg) => {
                            if reg.is_predicate() {
                                vec![Instr::new_plop3(
                                    mov.dst,
                                    LogicOp::new_lut(&|x, _, _| x),
                                    mov.src,
                                    Src::new_imm_bool(true),
                                    Src::new_imm_bool(true),
                                )]
                            } else {
                                vec![instr]
                            }
                        }
                        _ => vec![instr],
                    }
                }
                _ => vec![instr],
            }
        })
    }
}

impl fmt::Display for Shader {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for func in &self.functions {
            write!(f, "{}", func)?;
        }
        Ok(())
    }
}
