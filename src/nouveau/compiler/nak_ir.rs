/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use std::fmt;
use std::ops::{BitAnd, BitOr, Not, Range};
use std::slice;

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
pub enum Ref {
    Zero,
    Imm(Immediate),
    CBuf(CBufRef),
    SSA(SSAValue),
    Reg(RegRef),
}

impl Ref {
    pub fn new_zero() -> Ref {
        Ref::Zero
    }

    pub fn new_imm_u32(u: u32) -> Ref {
        Ref::Imm(Immediate { u: u })
    }

    pub fn new_cbuf(idx: u8, offset: u16) -> Ref {
        Ref::CBuf(CBufRef {
            buf: CBuf::Binding(idx),
            offset: offset,
        })
    }

    pub fn new_ssa(file: RegFile, idx: u32, comps: u8) -> Ref {
        Ref::SSA(SSAValue::new(file, idx, comps))
    }

    pub fn new_reg(file: RegFile, idx: u8, comps: u8) -> Ref {
        Ref::Reg(RegRef::new(file, idx, comps))
    }

    pub fn as_dst(&self) -> Dst {
        *self
    }

    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            Ref::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSAValue> {
        match self {
            Ref::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        match self {
            Src::Zero | Src::Imm(_) | Src::SSA(_) => None,
            Src::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessSSA(_) => None,
                CBuf::BindlessGPR(reg) => Some(reg),
            },
            Src::Reg(reg) => Some(reg),
        }
    }

    pub fn get_ssa(&self) -> Option<&SSAValue> {
        match self {
            Src::Zero | Src::Imm(_) | Src::Reg(_) => None,
            Src::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessGPR(_) => None,
                CBuf::BindlessSSA(ssa) => Some(ssa),
            },
            Src::SSA(ssa) => Some(ssa),
        }
    }

    pub fn is_zero(&self) -> bool {
        match self {
            Ref::Zero => true,
            Ref::Imm(i) => i.u == 0,
            _ => false,
        }
    }
}

impl fmt::Display for Ref {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Ref::Zero => write!(f, "ZERO")?,
            Ref::Imm(x) => write!(f, "{:#x}", x.u)?,
            Ref::CBuf(r) => {
                match r.buf {
                    CBuf::Binding(idx) => write!(f, "c[{:#x}]", idx)?,
                    CBuf::BindlessSSA(r) => write!(f, "cx[USSA{}]", r.idx())?,
                    CBuf::BindlessGPR(r) => write!(f, "cx[UR{}]", r)?,
                }
                write!(f, "[{:#x}]", r.offset)?;
            }
            Ref::SSA(v) => {
                if v.is_uniform() {
                    write!(f, "USSA{}@{}", v.idx(), v.comps())?;
                } else {
                    write!(f, "SSA{}@{}", v.idx(), v.comps())?;
                }
            }
            Ref::Reg(r) => r.fmt(f)?,
        }
        Ok(())
    }
}

pub type Src = Ref;
pub type Dst = Ref;

pub enum Pred {
    None,
    SSA(SSAValue),
    Reg(RegRef),
}

impl Pred {
    pub fn new_ssa(file: RegFile, idx: u32, comps: u8) -> Pred {
        Pred::SSA(SSAValue::new(file, idx, comps))
    }

    pub fn new_reg(file: RegFile, idx: u8, comps: u8) -> Pred {
        Pred::Reg(RegRef::new(file, idx, comps))
    }

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

struct InstrRefArr {
    num_dsts: u8,
    num_srcs: u8,
    refs: [Ref; 4],
}

struct InstrRefVecs {
    dsts: Vec<Dst>,
    srcs: Vec<Src>,
}

enum InstrRefs {
    Array(InstrRefArr),
    Vecs(InstrRefVecs),
}

impl InstrRefs {
    pub fn new(dsts: &[Dst], srcs: &[Src]) -> InstrRefs {
        if dsts.len() + srcs.len() > 4 {
            InstrRefs::Vecs(InstrRefVecs {
                dsts: Vec::from(dsts),
                srcs: Vec::from(srcs),
            })
        } else {
            let mut refs = [Ref::Zero, Ref::Zero, Ref::Zero, Ref::Zero];
            for i in 0..dsts.len() {
                refs[i] = dsts[i];
            }
            for i in 0..srcs.len() {
                refs[dsts.len() + i] = srcs[i];
            }
            InstrRefs::Array(InstrRefArr {
                num_dsts: dsts.len().try_into().unwrap(),
                num_srcs: srcs.len().try_into().unwrap(),
                refs: refs,
            })
        }
    }

    pub fn dsts(&self) -> &[Dst] {
        match self {
            InstrRefs::Array(x) => &x.refs[..x.num_dsts.into()],
            InstrRefs::Vecs(x) => &x.dsts,
        }
    }

    pub fn dsts_mut(&mut self) -> &mut [Dst] {
        match self {
            InstrRefs::Array(x) => &mut x.refs[..x.num_dsts.into()],
            InstrRefs::Vecs(x) => &mut x.dsts,
        }
    }

    pub fn srcs(&self) -> &[Src] {
        match self {
            InstrRefs::Array(x) => {
                &x.refs[x.num_dsts.into()..(x.num_dsts + x.num_srcs).into()]
            }
            InstrRefs::Vecs(x) => &x.srcs,
        }
    }

    pub fn srcs_mut(&mut self) -> &mut [Src] {
        match self {
            InstrRefs::Array(x) => {
                &mut x.refs[x.num_dsts.into()..(x.num_dsts + x.num_srcs).into()]
            }
            InstrRefs::Vecs(x) => &mut x.srcs,
        }
    }
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

pub enum CmpOp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

impl fmt::Display for CmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CmpOp::Eq => write!(f, "EQ"),
            CmpOp::Ne => write!(f, "NE"),
            CmpOp::Lt => write!(f, "LT"),
            CmpOp::Le => write!(f, "LE"),
            CmpOp::Gt => write!(f, "GT"),
            CmpOp::Ge => write!(f, "GE"),
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

pub struct IntCmpOp {
    pub cmp_op: CmpOp,
    pub cmp_type: IntCmpType,
}

impl fmt::Display for IntCmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}", self.cmp_op, self.cmp_type)
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

pub struct AttrAccess {
    pub addr: u16,
    pub comps: u8,
    pub patch: bool,
    pub out_load: bool,
    pub flags: u8,
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

pub enum MemOrder {
    Strong,
}

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
    pub op: Opcode,
    pub pred: Pred,
    pub pred_inv: bool,
    pub deps: InstrDeps,
    refs: InstrRefs,
}

impl Instr {
    pub fn new(op: Opcode, dsts: &[Dst], srcs: &[Src]) -> Instr {
        Instr {
            op: op,
            pred: Pred::None,
            pred_inv: false,
            refs: InstrRefs::new(dsts, srcs),
            deps: InstrDeps::new(),
        }
    }

    pub fn new_noop() -> Instr {
        Instr::new(Opcode::NOOP, &[], &[])
    }

    pub fn new_meta(instrs: Vec<Instr>) -> Instr {
        Instr::new(Opcode::META(MetaInstr::new(instrs)), &[], &[])
    }

    pub fn new_fadd(dst: Dst, x: Src, y: Src) -> Instr {
        Instr::new(Opcode::FADD, slice::from_ref(&dst), &[x, y])
    }

    pub fn new_iadd(dst: Dst, x: Src, y: Src) -> Instr {
        Instr::new(Opcode::IADD3, slice::from_ref(&dst), &[Src::Zero, x, y])
    }

    pub fn new_isetp(
        dst: Dst,
        cmp_type: IntCmpType,
        cmp_op: CmpOp,
        x: Src,
        y: Src,
    ) -> Instr {
        let op = IntCmpOp {
            cmp_type: cmp_type,
            cmp_op: cmp_op,
        };
        Instr::new(Opcode::ISETP(op), slice::from_ref(&dst), &[x, y])
    }

    pub fn new_lop3(dst: Dst, op: LogicOp, x: Src, y: Src, z: Src) -> Instr {
        Instr::new(Opcode::LOP3(op), slice::from_ref(&dst), &[x, y, z])
    }

    pub fn new_shl(dst: Dst, x: Src, shift: Src) -> Instr {
        Instr::new(Opcode::SHL, slice::from_ref(&dst), &[x, shift])
    }

    pub fn new_s2r(dst: Dst, idx: u8) -> Instr {
        Instr::new(Opcode::S2R(idx), slice::from_ref(&dst), &[])
    }

    pub fn new_mov(dst: Dst, src: Src) -> Instr {
        Instr::new(Opcode::MOV, slice::from_ref(&dst), slice::from_ref(&src))
    }

    pub fn new_sel(dst: Dst, sel: Src, x: Src, y: Src) -> Instr {
        Instr::new(Opcode::SEL, slice::from_ref(&dst), &[sel, x, y])
    }

    pub fn new_vec(dst: Dst, srcs: &[Src]) -> Instr {
        Instr::new(Opcode::VEC, slice::from_ref(&dst), srcs)
    }

    pub fn new_split(dsts: &[Dst], src: Src) -> Instr {
        Instr::new(Opcode::SPLIT, dsts, slice::from_ref(&src))
    }

    pub fn new_ald(dst: Dst, attr_addr: u16, vtx: Src, offset: Src) -> Instr {
        let attr = AttrAccess {
            addr: attr_addr,
            comps: dst.as_ssa().unwrap().comps(),
            patch: false,
            out_load: false,
            flags: 0,
        };
        Instr::new(Opcode::ALD(attr), slice::from_ref(&dst), &[vtx, offset])
    }

    pub fn new_ast(attr_addr: u16, data: Src, vtx: Src, offset: Src) -> Instr {
        let attr = AttrAccess {
            addr: attr_addr,
            comps: data.as_ssa().unwrap().comps(),
            patch: false,
            out_load: false,
            flags: 0,
        };
        Instr::new(Opcode::AST(attr), &[], &[data, vtx, offset])
    }

    pub fn new_ld(dst: Dst, access: MemAccess, addr: Src) -> Instr {
        Instr::new(
            Opcode::LD(access),
            slice::from_ref(&dst),
            slice::from_ref(&addr),
        )
    }

    pub fn new_st(access: MemAccess, addr: Src, data: Src) -> Instr {
        Instr::new(Opcode::ST(access), &[], &[addr, data])
    }

    pub fn new_fs_out(srcs: &[Src]) -> Instr {
        Instr::new(Opcode::FS_OUT, &[], srcs)
    }

    pub fn new_exit() -> Instr {
        let mut instr = Instr::new(Opcode::EXIT, &[], &[]);
        for i in 0..6 {
            instr.deps.add_wt_bar(i);
        }
        instr
    }

    pub fn dst(&self, idx: usize) -> &Dst {
        &self.refs.dsts()[idx]
    }

    pub fn dst_mut(&mut self, idx: usize) -> &mut Dst {
        &mut self.refs.dsts_mut()[idx]
    }

    pub fn dsts(&self) -> &[Dst] {
        self.refs.dsts()
    }

    pub fn dsts_mut(&mut self) -> &mut [Dst] {
        self.refs.dsts_mut()
    }

    pub fn src(&self, idx: usize) -> &Src {
        &self.refs.srcs()[idx]
    }

    pub fn src_mut(&mut self, idx: usize) -> &mut Src {
        &mut self.refs.srcs_mut()[idx]
    }

    pub fn srcs(&self) -> &[Src] {
        self.refs.srcs()
    }

    pub fn srcs_mut(&mut self) -> &mut [Src] {
        self.refs.srcs_mut()
    }

    pub fn num_dsts(&self) -> usize {
        self.dsts().len()
    }

    pub fn num_srcs(&self) -> usize {
        self.srcs().len()
    }

    pub fn can_eliminate(&self) -> bool {
        match self.op {
            Opcode::FS_OUT | Opcode::EXIT | Opcode::AST(_) | Opcode::ST(_) => {
                false
            }
            _ => true,
        }
    }

    pub fn get_latency(&self) -> Option<u32> {
        match self.op {
            Opcode::FADD
            | Opcode::FFMA
            | Opcode::FMNMX
            | Opcode::FMUL
            | Opcode::IADD3
            | Opcode::LOP3(_)
            | Opcode::ISETP(_)
            | Opcode::SHL => Some(6),
            Opcode::MOV => Some(15),
            Opcode::SEL => Some(15),
            Opcode::S2R(_) => None,
            Opcode::ALD(_) => None,
            Opcode::AST(_) => Some(15),
            Opcode::LD(_) => None,
            Opcode::ST(_) => None,
            Opcode::EXIT => Some(15),
            Opcode::NOOP
            | Opcode::META(_)
            | Opcode::VEC
            | Opcode::SPLIT
            | Opcode::FS_OUT => panic!("Not a hardware opcode"),
        }
    }
}

pub struct MetaInstr {
    instrs: Vec<Instr>,
}

impl MetaInstr {
    pub fn new(instrs: Vec<Instr>) -> MetaInstr {
        MetaInstr { instrs: instrs }
    }
}

impl fmt::Display for MetaInstr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{{\n")?;
        for i in &self.instrs {
            write!(f, "{}\n", i)?;
        }
        write!(f, "}}")
    }
}

pub enum Opcode {
    NOOP,
    META(MetaInstr),
    FADD,
    FFMA,
    FMNMX,
    FMUL,

    IADD3,
    LOP3(LogicOp),
    ISETP(IntCmpOp),
    SHL,

    S2R(u8),
    MOV,
    SEL,
    VEC,
    SPLIT,

    ALD(AttrAccess),
    AST(AttrAccess),
    LD(MemAccess),
    ST(MemAccess),

    FS_OUT,

    EXIT,
}

impl fmt::Display for Opcode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Opcode::NOOP => write!(f, "NOOP"),
            Opcode::META(m) => write!(f, "META {}", m),
            Opcode::FADD => write!(f, "FADD"),
            Opcode::FFMA => write!(f, "FFMA"),
            Opcode::FMNMX => write!(f, "FMNMX"),
            Opcode::FMUL => write!(f, "FMUL"),
            Opcode::IADD3 => write!(f, "IADD3"),
            Opcode::LOP3(op) => write!(f, "LOP3.{}", op),
            Opcode::ISETP(op) => write!(f, "ISETP.{}", op),
            Opcode::SHL => write!(f, "SHL"),
            Opcode::S2R(i) => write!(f, "S2R({})", i),
            Opcode::MOV => write!(f, "MOV"),
            Opcode::SEL => write!(f, "SEL"),
            Opcode::VEC => write!(f, "VEC"),
            Opcode::SPLIT => write!(f, "SPLIT"),
            Opcode::ALD(_) => write!(f, "ALD"),
            Opcode::AST(_) => write!(f, "AST"),
            Opcode::LD(a) => write!(f, "LD.{}", a),
            Opcode::ST(a) => write!(f, "ST.{}", a),
            Opcode::FS_OUT => write!(f, "FS_OUT"),
            Opcode::EXIT => write!(f, "EXIT"),
        }
    }
}

impl fmt::Display for Instr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if !self.pred.is_none() {
            if self.pred_inv {
                write!(f, "@!{}", self.pred)?;
            } else {
                write!(f, "@{}", self.pred)?;
            }
        }
        write!(f, "{} {{", self.op)?;
        if self.num_dsts() > 0 {
            write!(f, " {}", self.dst(0))?;
            for dst in &self.dsts()[1..] {
                write!(f, ", {}", dst)?;
            }
        }
        write!(f, " }} {{")?;
        if self.num_srcs() > 0 {
            write!(f, " {}", self.src(0))?;
            for src in &self.srcs()[1..] {
                write!(f, ", {}", src)?;
            }
        }
        write!(f, " }} {}", self.deps)
    }
}

pub struct BasicBlock {
    id: u32,
    pub instrs: Vec<Instr>,
}

impl BasicBlock {
    pub fn new(id: u32) -> BasicBlock {
        BasicBlock {
            id: id,
            instrs: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr) -> Instr>(&mut self, map: &F) {
        let mut instrs = Vec::new();
        for i in self.instrs.drain(..) {
            let new_instr = map(i);
            match new_instr.op {
                Opcode::NOOP => {}
                Opcode::META(mut meta) => {
                    instrs.append(&mut meta.instrs);
                }
                _ => {
                    instrs.push(new_instr);
                }
            }
        }
        self.instrs = instrs;
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
    pub ssa_count: u32,
    pub blocks: Vec<BasicBlock>,
}

impl Function {
    pub fn new(id: u32, reserved_ssa_count: u32) -> Function {
        Function {
            id: id,
            ssa_count: reserved_ssa_count,
            blocks: Vec::new(),
        }
    }

    pub fn alloc_ssa(&mut self, file: RegFile, comps: u8) -> Ref {
        let idx = self.ssa_count;
        self.ssa_count += 1;
        Ref::new_ssa(file, idx, comps)
    }

    pub fn map_instrs<F: Fn(Instr) -> Instr>(&mut self, map: &F) {
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
    pub functions: Vec<Function>,
}

impl Shader {
    pub fn new() -> Shader {
        Shader {
            functions: Vec::new(),
        }
    }

    pub fn map_instrs<F: Fn(Instr) -> Instr>(&mut self, map: &F) {
        for f in &mut self.functions {
            f.map_instrs(map);
        }
    }

    pub fn lower_vec_split(&mut self) {
        self.map_instrs(&|instr: Instr| -> Instr {
            match instr.op {
                Opcode::VEC => {
                    let comps = u8::try_from(instr.num_srcs()).unwrap();
                    if comps == 1 {
                        let src = instr.src(0);
                        let dst = instr.dst(0);
                        Instr::new_mov(*dst, *src)
                    } else {
                        let mut instrs = Vec::new();
                        let vec_dst = instr.dst(0).as_reg().unwrap();
                        assert!(comps == vec_dst.comps());
                        for i in 0..comps {
                            let src = instr.src(i.into());
                            let dst = Dst::Reg(vec_dst.as_comp(i).unwrap());
                            instrs.push(Instr::new_mov(dst, *src));
                        }
                        Instr::new_meta(instrs)
                    }
                }
                Opcode::SPLIT => {
                    let comps = u8::try_from(instr.num_dsts()).unwrap();
                    if instr.num_dsts() == 1 {
                        let src = instr.src(0);
                        let dst = instr.dst(0);
                        Instr::new_mov(*dst, *src)
                    } else {
                        let mut instrs = Vec::new();
                        let vec_src = instr.src(0).as_reg().unwrap();
                        assert!(comps == vec_src.comps());
                        for i in 0..comps {
                            let src = Dst::Reg(vec_src.as_comp(i).unwrap());
                            let dst = instr.dst(i.into());
                            if let Dst::Zero = dst {
                                continue;
                            }
                            instrs.push(Instr::new_mov(*dst, src));
                        }
                        Instr::new_meta(instrs)
                    }
                }
                Opcode::FS_OUT => {
                    let mut instrs = Vec::new();
                    for i in 0..instr.num_srcs() {
                        let dst = Ref::new_reg(
                            RegFile::GPR,
                            i.try_into().unwrap(),
                            1,
                        );
                        let src = instr.src(i);
                        instrs.push(Instr::new_mov(dst, *src));
                    }
                    Instr::new_meta(instrs)
                }
                _ => instr,
            }
        })
    }

    pub fn lower_zero_to_gpr255(&mut self) {
        for f in &mut self.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    for dst in instr.dsts_mut() {
                        if dst.is_zero() {
                            *dst = Dst::Reg(RegRef::zero(RegFile::GPR, 1))
                        }
                    }
                    for src in instr.srcs_mut() {
                        if src.is_zero() {
                            *src = Src::Reg(RegRef::zero(RegFile::GPR, 1))
                        }
                    }
                }
            }
        }
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