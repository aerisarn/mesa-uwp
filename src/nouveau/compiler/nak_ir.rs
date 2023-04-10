/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

extern crate nak_ir_proc;

use nak_ir_proc::*;
use std::fmt;
use std::ops::{BitAnd, BitOr, Not, Range};

#[derive(Clone, Copy)]
pub struct Immediate {
    pub u: u32,
}

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

    pub fn is_predicate(&self) -> bool {
        match self {
            RegFile::GPR | RegFile::UGPR => false,
            RegFile::Pred | RegFile::UPred => true,
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

    fn is_predicate(&self) -> bool {
        self.file().is_predicate()
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct SSAValue {
    packed: u32,
}

impl SSAValue {
    pub fn new(file: RegFile, idx: u32, comps: u8) -> SSAValue {
        assert!(idx < (1 << 27));
        let mut packed = idx;
        assert!(comps > 0 && comps <= 8);
        packed |= u32::from(comps - 1) << 27;
        assert!(u8::from(file) < 4);
        packed |= u32::from(u8::from(file)) << 30;
        SSAValue { packed: packed }
    }

    pub fn idx(&self) -> u32 {
        self.packed & 0x07ffffff
    }

    pub fn comps(&self) -> u8 {
        (((self.packed >> 27) & 0x7) + 1).try_into().unwrap()
    }
}

impl HasRegFile for SSAValue {
    fn file(&self) -> RegFile {
        RegFile::try_from(self.packed >> 30).unwrap()
    }
}

impl fmt::Display for SSAValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_uniform() {
            write!(f, "USSA{}@{}", self.idx(), self.comps())
        } else {
            write!(f, "SSA{}@{}", self.idx(), self.comps())
        }
    }
}

pub struct SSAValueAllocator {
    count: u32,
}

impl SSAValueAllocator {
    pub fn new(initial_count: u32) -> SSAValueAllocator {
        SSAValueAllocator {
            count: initial_count,
        }
    }

    pub fn alloc(&mut self, file: RegFile, comps: u8) -> SSAValue {
        let idx = self.count;
        self.count += 1;
        SSAValue::new(file, idx, comps)
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

    pub fn as_comp(&self, comp: u8) -> Option<RegRef> {
        if comp < self.comps() {
            Some(RegRef::new(self.file(), self.base_idx() + comp, 1))
        } else {
            None
        }
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
    SSA(SSAValue),
    Reg(RegRef),
}

impl Dst {
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            Dst::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSAValue> {
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

impl From<SSAValue> for Dst {
    fn from(ssa: SSAValue) -> Dst {
        Dst::SSA(ssa)
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

#[derive(Clone, Copy)]
pub enum CBuf {
    Binding(u8),
    BindlessSSA(SSAValue),
    BindlessGPR(RegRef),
}

#[derive(Clone, Copy)]
pub struct CBufRef {
    pub buf: CBuf,
    pub offset: u16,
}

#[derive(Clone, Copy)]
pub enum SrcRef {
    Zero,
    Imm(Immediate),
    CBuf(CBufRef),
    SSA(SSAValue),
    Reg(RegRef),
}

impl SrcRef {
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSAValue> {
        match self {
            SrcRef::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Zero | SrcRef::Imm(_) | SrcRef::SSA(_) => None,
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessSSA(_) => None,
                CBuf::BindlessGPR(reg) => Some(reg),
            },
            SrcRef::Reg(reg) => Some(reg),
        }
    }

    pub fn get_ssa(&self) -> Option<&SSAValue> {
        match self {
            SrcRef::Zero | SrcRef::Imm(_) | SrcRef::Reg(_) => None,
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessGPR(_) => None,
                CBuf::BindlessSSA(ssa) => Some(ssa),
            },
            SrcRef::SSA(ssa) => Some(ssa),
        }
    }
}

impl From<RegRef> for SrcRef {
    fn from(reg: RegRef) -> SrcRef {
        SrcRef::Reg(reg)
    }
}

impl From<SSAValue> for SrcRef {
    fn from(ssa: SSAValue) -> SrcRef {
        SrcRef::SSA(ssa)
    }
}

impl fmt::Display for SrcRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SrcRef::Zero => write!(f, "ZERO")?,
            SrcRef::Imm(x) => write!(f, "{:#x}", x.u)?,
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
    Abs,
    Neg,
    NegAbs,
    Not,
}

impl SrcMod {
    pub fn is_none(&self) -> bool {
        match self {
            SrcMod::None => true,
            _ => false,
        }
    }

    pub fn is_alu(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::Abs | SrcMod::Neg | SrcMod::NegAbs => true,
            SrcMod::Not => false,
        }
    }

    pub fn is_bitwise(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::Not => true,
            SrcMod::Abs | SrcMod::Neg | SrcMod::NegAbs => false,
        }
    }

    pub fn has_neg(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::Abs => false,
            SrcMod::Neg | SrcMod::NegAbs => true,
            SrcMod::Not => panic!("Not an ALU source modifier"),
        }
    }

    pub fn has_abs(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::Neg => false,
            SrcMod::Abs | SrcMod::NegAbs => true,
            SrcMod::Not => panic!("Not an ALU source modifier"),
        }
    }

    pub fn has_not(&self) -> bool {
        match self {
            SrcMod::None => false,
            SrcMod::Not => true,
            _ => panic!("Not a boolean source modifier"),
        }
    }

    pub fn abs(&self) -> SrcMod {
        match self {
            SrcMod::None | SrcMod::Abs | SrcMod::Neg | SrcMod::NegAbs => {
                SrcMod::Abs
            }
            SrcMod::Not => panic!("Not an ALU source modifier"),
        }
    }

    pub fn neg(&self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::Neg,
            SrcMod::Abs => SrcMod::NegAbs,
            SrcMod::Neg => SrcMod::None,
            SrcMod::NegAbs => SrcMod::Abs,
            SrcMod::Not => panic!("Not an ALU source modifier"),
        }
    }

    pub fn not(&self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::Not,
            SrcMod::Not => SrcMod::None,
            _ => panic!("Not a boolean source modifier"),
        }
    }
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
        SrcRef::Imm(Immediate { u: u }).into()
    }

    pub fn new_cbuf(idx: u8, offset: u16) -> Src {
        SrcRef::CBuf(CBufRef {
            buf: CBuf::Binding(idx),
            offset: offset,
        })
        .into()
    }

    pub fn abs(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.abs(),
        }
    }

    pub fn neg(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.neg(),
        }
    }

    pub fn not(&self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.not(),
        }
    }

    pub fn as_ssa(&self) -> Option<&SSAValue> {
        if self.src_mod.is_none() {
            self.src_ref.as_ssa()
        } else {
            None
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        self.src_ref.get_reg()
    }

    pub fn get_ssa(&self) -> Option<&SSAValue> {
        self.src_ref.get_ssa()
    }

    pub fn is_uniform(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero | SrcRef::Imm(_) | SrcRef::CBuf(_) => true,
            SrcRef::SSA(ssa) => ssa.is_uniform(),
            SrcRef::Reg(reg) => reg.is_uniform(),
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
            SrcRef::Imm(_) | SrcRef::CBuf(_) => false,
        }
    }
}

impl From<SrcRef> for Src {
    fn from(src_ref: SrcRef) -> Src {
        Src {
            src_ref: src_ref,
            src_mod: SrcMod::None,
        }
    }
}

impl From<RegRef> for Src {
    fn from(reg: RegRef) -> Src {
        SrcRef::from(reg).into()
    }
}

impl From<SSAValue> for Src {
    fn from(ssa: SSAValue) -> Src {
        SrcRef::from(ssa).into()
    }
}

impl fmt::Display for Src {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.src_mod {
            SrcMod::None => write!(f, "{}", self.src_ref),
            SrcMod::Abs => write!(f, "|{}|", self.src_ref),
            SrcMod::Neg => write!(f, "-{}", self.src_ref),
            SrcMod::NegAbs => write!(f, "-|{}|", self.src_ref),
            SrcMod::Not => write!(f, "!{}", self.src_ref),
        }
    }
}

pub trait SrcsAsSlice {
    fn srcs_as_slice(&self) -> &[Src];
    fn srcs_as_mut_slice(&mut self) -> &mut [Src];
}

pub trait DstsAsSlice {
    fn dsts_as_slice(&self) -> &[Dst];
    fn dsts_as_mut_slice(&mut self) -> &mut [Dst];
}

pub enum PredSetOp {
    Set,
    And,
    Or,
    Xor,
    AndNot,
    OrNot,
    XorNot,
}

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

pub struct LogicOp {
    pub lut: u8,
}

impl LogicOp {
    #[inline]
    pub fn new_lut<F: Fn(u8, u8, u8) -> u8>(f: &F) -> LogicOp {
        LogicOp {
            lut: f(0xf0, 0xcc, 0xaa),
        }
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

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemOrder {
    Strong,
}

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
    pub order: MemOrder,
    pub scope: MemScope,
}

impl fmt::Display for MemAccess {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.addr_type, self.mem_type, self.scope)
    }
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
pub struct OpFSet {
    pub dst: Dst,
    pub cmp_op: FloatCmpOp,
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
    pub cmp_op: FloatCmpOp,
    pub srcs: [Src; 2],
    /* TODO: Other predicates? Combine ops? */
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

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd3 {
    pub dst: Dst,
    pub overflow: Dst,
    pub srcs: [Src; 3],
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
pub struct OpISetP {
    pub dst: Dst,

    pub cmp_op: IntCmpOp,
    pub cmp_type: IntCmpType,

    pub srcs: [Src; 2],
    /* TODO: Other predicates? Combine ops? */
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
pub struct OpShl {
    pub dst: Dst,
    pub srcs: [Src; 2],
}

impl fmt::Display for OpShl {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SHL {} {{ {}, {} }}",
            self.dst, self.srcs[0], self.srcs[1],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpI2F {
    pub dst: Dst,
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
    pub cond: Src,
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
    pub dst: Dst,
    pub srcs: [Src; 3],
    pub op: LogicOp,
}

impl fmt::Display for OpPLop3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "PLOP3.{} {} {{ {}, {}, {} }}",
            self.op, self.dst, self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLd {
    pub dst: Dst,
    pub addr: Src,
    pub offset: u32,
    pub access: MemAccess,
}

impl fmt::Display for OpLd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "LD.{} {} [{}", self.access, self.dst, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "]")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSt {
    pub addr: Src,
    pub data: Src,
    pub offset: u32,
    pub access: MemAccess,
}

impl fmt::Display for OpSt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ST.{} [{}", self.access, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpALd {
    pub dst: Dst,
    pub vtx: Src,
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
    pub vtx: Src,
    pub offset: Src,
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
pub struct OpFMov {
    pub dst: Dst,
    pub src: Src,
    pub saturate: bool,
}

impl fmt::Display for OpFMov {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FMOV")?;
        if self.saturate {
            write!(f, ".SAT")?;
        }
        write!(f, " {} {}", self.dst, self.src)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMov {
    pub dst: Dst,
    pub src: Src,
}

impl fmt::Display for OpIMov {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "IMOV {} {}", self.dst, self.src)
    }
}

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpVec {
    pub dst: Dst,
    pub srcs: Vec<Src>,
}

impl SrcsAsSlice for OpVec {
    fn srcs_as_slice(&self) -> &[Src] {
        &self.srcs
    }

    fn srcs_as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs
    }
}

impl fmt::Display for OpVec {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "VEC {} {{ {}", self.dst, self.srcs[0])?;
        for src in &self.srcs[1..] {
            write!(f, " {}", src)?;
        }
        write!(f, "}}")
    }
}

#[repr(C)]
#[derive(SrcsAsSlice)]
pub struct OpSplit {
    pub dsts: Vec<Dst>,
    pub src: Src,
}

impl DstsAsSlice for OpSplit {
    fn dsts_as_slice(&self) -> &[Dst] {
        &self.dsts
    }

    fn dsts_as_mut_slice(&mut self) -> &mut [Dst] {
        &mut self.dsts
    }
}

impl fmt::Display for OpSplit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SPLIT {{ {}", self.dsts[0])?;
        for dst in &self.dsts[1..] {
            write!(f, " {}", dst)?;
        }
        write!(f, "}} {}", self.src)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPhiSrc {
    pub src: Src,
    pub phi_id: u32,
}

impl fmt::Display for OpPhiSrc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PHI_SRC({}) {}", self.phi_id, self.src)
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPhiDst {
    pub dst: Dst,
    pub phi_id: u32,
}

impl fmt::Display for OpPhiDst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PHI_DST({}) {}", self.phi_id, self.dst)
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

#[derive(Display, DstsAsSlice, SrcsAsSlice)]
pub enum Op {
    FAdd(OpFAdd),
    FSet(OpFSet),
    FSetP(OpFSetP),
    IAdd3(OpIAdd3),
    ISetP(OpISetP),
    Lop3(OpLop3),
    Shl(OpShl),
    I2F(OpI2F),
    Mov(OpMov),
    Sel(OpSel),
    PLop3(OpPLop3),
    Ld(OpLd),
    St(OpSt),
    ALd(OpALd),
    ASt(OpASt),
    Bra(OpBra),
    Exit(OpExit),
    S2R(OpS2R),
    FMov(OpFMov),
    IMov(OpIMov),
    PhiSrc(OpPhiSrc),
    PhiDst(OpPhiDst),
    Vec(OpVec),
    Split(OpSplit),
    FSOut(OpFSOut),
}

pub enum Pred {
    None,
    SSA(SSAValue),
    Reg(RegRef),
}

impl Pred {
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            Pred::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSAValue> {
        match self {
            Pred::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn is_none(&self) -> bool {
        match self {
            Pred::None => true,
            _ => false,
        }
    }
}

impl From<RegRef> for Pred {
    fn from(reg: RegRef) -> Pred {
        Pred::Reg(reg)
    }
}

impl From<SSAValue> for Pred {
    fn from(ssa: SSAValue) -> Pred {
        Pred::SSA(ssa)
    }
}

impl fmt::Display for Pred {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Pred::None => (),
            Pred::SSA(v) => {
                if v.is_uniform() {
                    write!(f, "USSA{}@{}", v.idx(), v.comps())?;
                } else {
                    write!(f, "SSA{}@{}", v.idx(), v.comps())?;
                }
            }
            Pred::Reg(r) => r.fmt(f)?,
        }
        Ok(())
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

    pub fn add_reuse_bar(&mut self, idx: u8) {
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
    pub pred_inv: bool,
    pub op: Op,
    pub deps: InstrDeps,
}

impl Instr {
    pub fn new(op: Op) -> Instr {
        Instr {
            op: op,
            pred: Pred::None,
            pred_inv: false,
            deps: InstrDeps::new(),
        }
    }

    pub fn new_fadd(dst: Dst, x: Src, y: Src) -> Instr {
        Instr::new(Op::FAdd(OpFAdd {
            dst: dst,
            srcs: [x, y],
            saturate: false,
            rnd_mode: FRndMode::NearestEven,
        }))
    }

    pub fn new_fset(dst: Dst, cmp_op: FloatCmpOp, x: Src, y: Src) -> Instr {
        Instr::new(Op::FSet(OpFSet {
            dst: dst,
            cmp_op: cmp_op,
            srcs: [x, y],
        }))
    }

    pub fn new_fsetp(dst: Dst, cmp_op: FloatCmpOp, x: Src, y: Src) -> Instr {
        Instr::new(Op::FSetP(OpFSetP {
            dst: dst,
            cmp_op: cmp_op,
            srcs: [x, y],
        }))
    }

    pub fn new_iadd(dst: Dst, x: Src, y: Src) -> Instr {
        Instr::new(Op::IAdd3(OpIAdd3 {
            dst: dst,
            overflow: Dst::None,
            srcs: [Src::new_zero(), x, y],
            carry: Src::new_zero(),
        }))
    }

    pub fn new_i2f(dst: Dst, src: Src) -> Instr {
        Instr::new(Op::I2F(OpI2F {
            dst: dst,
            src: src,
            dst_type: FloatType::F32,
            src_type: IntType::I32,
            rnd_mode: FRndMode::NearestEven,
        }))
    }

    pub fn new_isetp(
        dst: Dst,
        cmp_type: IntCmpType,
        cmp_op: IntCmpOp,
        x: Src,
        y: Src,
    ) -> Instr {
        Instr::new(Op::ISetP(OpISetP {
            dst: dst,
            cmp_op: cmp_op,
            cmp_type: cmp_type,
            srcs: [x, y],
        }))
    }

    pub fn new_lop3(dst: Dst, op: LogicOp, x: Src, y: Src, z: Src) -> Instr {
        Instr::new(Op::Lop3(OpLop3 {
            dst: dst,
            srcs: [x, y, z],
            op: op,
        }))
    }

    pub fn new_shl(dst: Dst, x: Src, shift: Src) -> Instr {
        Instr::new(Op::Shl(OpShl {
            dst: dst,
            srcs: [x, shift],
        }))
    }

    pub fn new_mov(dst: Dst, src: Src) -> Instr {
        Instr::new(Op::Mov(OpMov {
            dst: dst,
            src: src,
            quad_lanes: 0xf,
        }))
    }

    pub fn new_sel(dst: Dst, sel: Src, x: Src, y: Src) -> Instr {
        Instr::new(Op::Sel(OpSel {
            dst: dst,
            cond: sel,
            srcs: [x, y],
        }))
    }

    pub fn new_plop3(dst: Dst, op: LogicOp, x: Src, y: Src, z: Src) -> Instr {
        Instr::new(Op::PLop3(OpPLop3 {
            dst: dst,
            srcs: [x, y, z],
            op: op,
        }))
    }

    pub fn new_ld(dst: Dst, access: MemAccess, addr: Src) -> Instr {
        Instr::new(Op::Ld(OpLd {
            dst: dst,
            addr: addr,
            offset: 0,
            access: access,
        }))
    }

    pub fn new_st(access: MemAccess, addr: Src, data: Src) -> Instr {
        Instr::new(Op::St(OpSt {
            addr: addr,
            data: data,
            offset: 0,
            access: access,
        }))
    }

    pub fn new_ald(dst: Dst, attr_addr: u16, vtx: Src, offset: Src) -> Instr {
        Instr::new(Op::ALd(OpALd {
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
        }))
    }

    pub fn new_ast(attr_addr: u16, data: Src, vtx: Src, offset: Src) -> Instr {
        Instr::new(Op::ASt(OpASt {
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
        }))
    }

    pub fn new_bra(block: u32) -> Instr {
        Instr::new(Op::Bra(OpBra { target: block }))
    }

    pub fn new_exit() -> Instr {
        Instr::new(Op::Exit(OpExit {}))
    }

    pub fn new_s2r(dst: Dst, idx: u8) -> Instr {
        Instr::new(Op::S2R(OpS2R { dst: dst, idx: idx }))
    }

    pub fn new_phi_src(phi_id: u32, src: Src) -> Instr {
        Instr::new(Op::PhiSrc(OpPhiSrc {
            phi_id: phi_id,
            src: src,
        }))
    }

    pub fn new_phi_dst(phi_id: u32, dst: Dst) -> Instr {
        Instr::new(Op::PhiDst(OpPhiDst {
            phi_id: phi_id,
            dst: dst,
        }))
    }

    pub fn new_vec(dst: Dst, srcs: &[Src]) -> Instr {
        Instr::new(Op::Vec(OpVec {
            dst: dst,
            srcs: srcs.to_vec(),
        }))
    }

    pub fn new_split(dsts: &[Dst], src: Src) -> Instr {
        Instr::new(Op::Split(OpSplit {
            dsts: dsts.to_vec(),
            src: src,
        }))
    }

    pub fn new_fs_out(srcs: &[Src]) -> Instr {
        Instr::new(Op::FSOut(OpFSOut {
            srcs: srcs.to_vec(),
        }))
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

    pub fn is_branch(&self) -> bool {
        match self.op {
            Op::Bra(_) => true,
            _ => false,
        }
    }

    pub fn can_eliminate(&self) -> bool {
        match self.op {
            Op::ASt(_)
            | Op::St(_)
            | Op::Bra(_)
            | Op::Exit(_)
            | Op::FSOut(_) => false,
            _ => true,
        }
    }

    pub fn get_latency(&self) -> Option<u32> {
        match self.op {
            Op::FAdd(_)
            | Op::FSet(_)
            | Op::FSetP(_)
            | Op::IAdd3(_)
            | Op::Lop3(_)
            | Op::PLop3(_)
            | Op::ISetP(_)
            | Op::Shl(_) => Some(6),
            Op::I2F(_) | Op::Mov(_) => Some(15),
            Op::Sel(_) => Some(15),
            Op::S2R(_) => None,
            Op::ALd(_) => None,
            Op::ASt(_) => Some(15),
            Op::Ld(_) => None,
            Op::St(_) => None,
            Op::Bra(_) | Op::Exit(_) => Some(15),
            Op::FMov(_)
            | Op::IMov(_)
            | Op::PhiSrc(_)
            | Op::PhiDst(_)
            | Op::Vec(_)
            | Op::Split(_)
            | Op::FSOut(_) => {
                panic!("Not a hardware opcode")
            }
        }
    }
}

impl fmt::Display for Instr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if !self.pred.is_none() {
            if self.pred_inv {
                write!(f, "@!{} ", self.pred)?;
            } else {
                write!(f, "@{} ", self.pred)?;
            }
        }
        write!(f, "{} {}", self.op, self.deps)
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

    pub fn map_instrs<F: Fn(Instr) -> Vec<Instr>>(&mut self, map: &F) {
        let mut instrs = Vec::new();
        for i in self.instrs.drain(..) {
            instrs.append(&mut map(i));
        }
        self.instrs = instrs;
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
    id: u32,
    pub ssa_alloc: SSAValueAllocator,
    pub blocks: Vec<BasicBlock>,
}

impl Function {
    pub fn new(id: u32, reserved_ssa_count: u32) -> Function {
        Function {
            id: id,
            ssa_alloc: SSAValueAllocator::new(reserved_ssa_count),
            blocks: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr) -> Vec<Instr>>(&mut self, map: &F) {
        for b in &mut self.blocks {
            b.map_instrs(map);
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

    pub fn map_instrs<F: Fn(Instr) -> Vec<Instr>>(&mut self, map: &F) {
        for f in &mut self.functions {
            f.map_instrs(map);
        }
    }

    pub fn lower_vec_split(&mut self) {
        self.map_instrs(&|instr: Instr| -> Vec<Instr> {
            match instr.op {
                Op::FMov(mov) => {
                    vec![Instr::new(Op::FAdd(OpFAdd {
                        dst: mov.dst,
                        srcs: [Src::new_zero(), mov.src],
                        saturate: mov.saturate,
                        rnd_mode: FRndMode::NearestEven,
                    }))]
                }
                Op::IMov(mov) => {
                    vec![Instr::new(Op::IAdd3(OpIAdd3 {
                        dst: mov.dst,
                        overflow: Dst::None,
                        srcs: [Src::new_zero(), mov.src, Src::new_zero()],
                        carry: Src::new_zero(),
                    }))]
                }
                Op::Vec(vec) => {
                    let mut instrs = Vec::new();
                    let comps = u8::try_from(vec.srcs.len()).unwrap();
                    if comps == 1 {
                        let src = vec.srcs[0];
                        let dst = vec.dst;
                        instrs.push(Instr::new_mov(dst, src));
                    } else {
                        let vec_dst = vec.dst.as_reg().unwrap();
                        assert!(comps == vec_dst.comps());
                        for i in 0..comps {
                            let src = vec.srcs[usize::from(i)];
                            let dst = Dst::Reg(vec_dst.as_comp(i).unwrap());
                            instrs.push(Instr::new_mov(dst, src));
                        }
                    }
                    instrs
                }
                Op::Split(split) => {
                    let mut instrs = Vec::new();
                    let comps = u8::try_from(split.dsts.len()).unwrap();
                    if comps == 1 {
                        let src = split.src;
                        let dst = split.dsts[0];
                        instrs.push(Instr::new_mov(dst, src));
                    } else {
                        let vec_src = split.src.src_ref.as_reg().unwrap();
                        assert!(comps == vec_src.comps());
                        for i in 0..comps {
                            let src = vec_src.as_comp(i).unwrap();
                            let dst = split.dsts[usize::from(i)];
                            if let Dst::None = dst {
                                continue;
                            }
                            instrs.push(Instr::new_mov(dst.into(), src.into()));
                        }
                    }
                    instrs
                }
                Op::FSOut(out) => {
                    let mut instrs = Vec::new();
                    for (i, src) in out.srcs.iter().enumerate() {
                        let dst =
                            RegRef::new(RegFile::GPR, i.try_into().unwrap(), 1);
                        instrs.push(Instr::new_mov(dst.into(), *src));
                    }
                    instrs
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
