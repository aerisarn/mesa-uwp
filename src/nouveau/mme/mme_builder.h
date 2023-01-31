#ifndef MME_BUILDER_H
#define MME_BUILDER_H

#include "mme_tu104.h"

#include "util/bitscan.h"
#include "util/enum_operators.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mme_value_type {
   MME_VALUE_TYPE_ZERO,
   MME_VALUE_TYPE_IMM,
   MME_VALUE_TYPE_REG,
};

struct mme_value {
   enum mme_value_type type;

   union {
      uint32_t imm;
      uint32_t reg;
   };
};

struct mme_value64 {
   struct mme_value lo;
   struct mme_value hi;
};

struct mme_builder;

enum mme_tu104_instr_parts {
   MME_TU104_INSTR_PART_IMM0  = BITFIELD_BIT(0),
   MME_TU104_INSTR_PART_IMM1  = BITFIELD_BIT(1),
   MME_TU104_INSTR_PART_LOAD0 = BITFIELD_BIT(2),
   MME_TU104_INSTR_PART_LOAD1 = BITFIELD_BIT(3),
   MME_TU104_INSTR_PART_ALU0  = BITFIELD_BIT(4),
   MME_TU104_INSTR_PART_ALU1  = BITFIELD_BIT(5),
   MME_TU104_INSTR_PART_MTHD0 = BITFIELD_BIT(6),
   MME_TU104_INSTR_PART_MTHD1 = BITFIELD_BIT(7),
   MME_TU104_INSTR_PART_EMIT0 = BITFIELD_BIT(8),
   MME_TU104_INSTR_PART_EMIT1 = BITFIELD_BIT(9),
};

#define MME_TU104_BUILDER_MAX_INSTS 128

enum mme_tu104_cf_type {
   MME_CF_TYPE_IF,
   MME_CF_TYPE_LOOP,
   MME_CF_TYPE_WHILE,
};

struct mme_tu104_cf {
   enum mme_tu104_cf_type type;
   uint16_t start_ip;
};

struct mme_tu104_builder {
   uint32_t inst_count;
   struct mme_tu104_inst insts[MME_TU104_BUILDER_MAX_INSTS];
   enum mme_tu104_instr_parts inst_parts;

   uint32_t cf_depth;
   struct mme_tu104_cf cf_stack[8];
};

void mme_tu104_add_inst(struct mme_builder *b,
                        const struct mme_tu104_inst *inst);

#define mme_tu104_asm(b, __inst)                                     \
   for (struct mme_tu104_inst __inst = { MME_TU104_INST_DEFAULTS };  \
        !__inst.end_next;                                            \
        mme_tu104_add_inst((b), &__inst), __inst.end_next = true)

void mme_tu104_alu_to(struct mme_builder *b,
                      struct mme_value dst,
                      enum mme_tu104_alu_op op,
                      struct mme_value x,
                      struct mme_value y,
                      uint16_t implicit_imm);

void mme_tu104_alu64_to(struct mme_builder *b,
                        struct mme_value64 dst,
                        enum mme_tu104_alu_op op_lo,
                        enum mme_tu104_alu_op op_hi,
                        struct mme_value64 x,
                        struct mme_value64 y);

void mme_tu104_load_to(struct mme_builder *b,
                       struct mme_value dst);

void mme_tu104_mthd(struct mme_builder *b,
                    uint16_t mthd, struct mme_value index);

void mme_tu104_emit(struct mme_builder *b,
                    struct mme_value data);

void mme_tu104_start_loop(struct mme_builder *b,
                          struct mme_value count);
void mme_tu104_end_loop(struct mme_builder *b);

void mme_tu104_start_if(struct mme_builder *b,
                        enum mme_tu104_alu_op op,
                        bool if_true,
                        struct mme_value x,
                        struct mme_value y);
void mme_tu104_end_if(struct mme_builder *b);

void mme_tu104_start_while(struct mme_builder *b);
void mme_tu104_end_while(struct mme_builder *b,
                         enum mme_tu104_alu_op op,
                         bool if_true,
                         struct mme_value x,
                         struct mme_value y);

uint32_t *mme_tu104_builder_finish(struct mme_tu104_builder *b,
                                   size_t *size_out);

#define MME_BUILDER_MAX_REGS 23

struct mme_builder {
   uint32_t reg_alloc;

   struct mme_tu104_builder tu104;
};

static inline void
mme_builder_init(struct mme_builder *b)
{
   memset(b, 0, sizeof(*b));
}

static inline uint32_t *
mme_builder_finish(struct mme_builder *b, size_t *size_out)
{
   return mme_tu104_builder_finish(&b->tu104, size_out);
}

static inline struct mme_value
mme_zero()
{
   struct mme_value val = {
      .type = MME_VALUE_TYPE_ZERO,
   };
   return val;
}

static inline struct mme_value
mme_imm(uint32_t imm)
{
   struct mme_value val = {
      .type = MME_VALUE_TYPE_IMM,
      .imm = imm,
   };
   return val;
}

static inline struct mme_value64
mme_value64(struct mme_value lo, struct mme_value hi)
{
   struct mme_value64 val = { lo, hi };
   return val;
}

static inline struct mme_value64
mme_imm64(uint64_t imm)
{
   struct mme_value64 val = {
      mme_imm((uint32_t)imm),
      mme_imm((uint32_t)(imm >> 32)),
   };
   return val;
}

static inline uint8_t
__mme_alloc_reg(struct mme_builder *b)
{
   uint8_t reg = ffs(~b->reg_alloc) - 1;
   b->reg_alloc |= (1u << reg);
   return reg;
}

static inline void
__mme_free_reg(struct mme_builder *b, uint8_t reg)
{
   b->reg_alloc &= ~(1u << reg);
}

static inline struct mme_value
mme_alloc_reg(struct mme_builder *b)
{
   struct mme_value val = {
      .type = MME_VALUE_TYPE_REG,
      .reg = __mme_alloc_reg(b),
   };
   return val;
}

static inline void
mme_free_reg(struct mme_builder *b, struct mme_value val)
{
   assert(val.type == MME_VALUE_TYPE_REG);
   __mme_free_reg(b, val.reg);
}

static inline struct mme_value
mme_tu104_alu(struct mme_builder *b,
              enum mme_tu104_alu_op op,
              struct mme_value x,
              struct mme_value y,
              uint16_t implicit_imm)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_tu104_alu_to(b, dst, op, x, y, implicit_imm);
   return dst;
}

static inline void
mme_tu104_alu_no_dst(struct mme_builder *b,
                     enum mme_tu104_alu_op op,
                     struct mme_value x,
                     struct mme_value y,
                     uint16_t implicit_imm)
{
   mme_tu104_alu_to(b, mme_zero(), op, x, y, implicit_imm);
}

static inline struct mme_value64
mme_tu104_alu64(struct mme_builder *b,
                enum mme_tu104_alu_op op_lo, enum mme_tu104_alu_op op_hi,
                struct mme_value64 x, struct mme_value64 y)
{
   struct mme_value64 dst = {
      mme_alloc_reg(b),
      mme_alloc_reg(b),
   };
   mme_tu104_alu64_to(b, dst, op_lo, op_hi, x, y);
   return dst;
}

#define MME_TU104_DEF_ALU1(op, OP)                                      \
static inline void                                                      \
mme_##op##_to(struct mme_builder *b, struct mme_value dst,              \
              struct mme_value x)                                       \
{                                                                       \
   mme_tu104_alu_to(b, dst, MME_TU104_ALU_OP_##OP, x, mme_zero(), 0);   \
}                                                                       \
                                                                        \
static inline struct mme_value                                          \
mme_##op(struct mme_builder *b,                                         \
         struct mme_value x)                                            \
{                                                                       \
   return mme_tu104_alu(b, MME_TU104_ALU_OP_##OP, x, mme_zero(), 0);    \
}

#define MME_TU104_DEF_ALU2(op, OP)                             \
static inline void                                             \
mme_##op##_to(struct mme_builder *b, struct mme_value dst,     \
              struct mme_value x, struct mme_value y)          \
{                                                              \
   mme_tu104_alu_to(b, dst, MME_TU104_ALU_OP_##OP, x, y, 0);   \
}                                                              \
                                                               \
static inline struct mme_value                                 \
mme_##op(struct mme_builder *b,                                \
         struct mme_value x, struct mme_value y)               \
{                                                              \
   return mme_tu104_alu(b, MME_TU104_ALU_OP_##OP, x, y, 0);    \
}

MME_TU104_DEF_ALU1(mov,    ADD);
MME_TU104_DEF_ALU2(add,    ADD);
MME_TU104_DEF_ALU2(sub,    SUB);
MME_TU104_DEF_ALU2(mul,    MUL);
MME_TU104_DEF_ALU1(clz,    CLZ);
MME_TU104_DEF_ALU2(sll,    SLL);
MME_TU104_DEF_ALU2(srl,    SRL);
MME_TU104_DEF_ALU2(sra,    SRA);
MME_TU104_DEF_ALU2(and,    AND);
MME_TU104_DEF_ALU2(nand,   NAND);
MME_TU104_DEF_ALU2(or,     OR);
MME_TU104_DEF_ALU2(xor,    XOR);
MME_TU104_DEF_ALU2(slt,    SLT);
MME_TU104_DEF_ALU2(sltu,   SLTU);
MME_TU104_DEF_ALU2(sle,    SLE);
MME_TU104_DEF_ALU2(sleu,   SLEU);
MME_TU104_DEF_ALU2(seq,    SEQ);
MME_TU104_DEF_ALU2(blt,    BLT);
MME_TU104_DEF_ALU2(bltu,   BLTU);
MME_TU104_DEF_ALU2(ble,    BLE);
MME_TU104_DEF_ALU2(bleu,   BLEU);
MME_TU104_DEF_ALU2(beq,    BEQ);
MME_TU104_DEF_ALU1(dread,  DREAD);

#undef MME_TU104_DEF_ALU1
#undef MME_TU104_DEF_ALU2

static inline void
mme_mov64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x)
{
   mme_tu104_alu64_to(b, dst, MME_TU104_ALU_OP_ADD,
                      MME_TU104_ALU_OP_ADD, x, mme_imm64(0));
}

static inline struct mme_value64
mme_mov64(struct mme_builder *b, struct mme_value64 x)
{
   return mme_tu104_alu64(b, MME_TU104_ALU_OP_ADD,
                          MME_TU104_ALU_OP_ADD, x, mme_imm64(0));
}

static inline void
mme_add64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x, struct mme_value64 y)
{
   mme_tu104_alu64_to(b, dst, MME_TU104_ALU_OP_ADD,
                      MME_TU104_ALU_OP_ADDC, x, y);
}

static inline struct mme_value64
mme_add64(struct mme_builder *b,
          struct mme_value64 x, struct mme_value64 y)
{
   return mme_tu104_alu64(b, MME_TU104_ALU_OP_ADD,
                          MME_TU104_ALU_OP_ADDC, x, y);
}

static inline void
mme_sub64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x, struct mme_value64 y)
{
   mme_tu104_alu64_to(b, dst, MME_TU104_ALU_OP_SUB,
                      MME_TU104_ALU_OP_SUBB, x, y);
}

static inline struct mme_value64
mme_sub64(struct mme_builder *b,
          struct mme_value64 x, struct mme_value64 y)
{
   return mme_tu104_alu64(b, MME_TU104_ALU_OP_SUB,
                          MME_TU104_ALU_OP_SUBB, x, y);
}

static inline void
mme_imul_32x32_64_to(struct mme_builder *b, struct mme_value64 dst,
                     struct mme_value x, struct mme_value y)
{
   mme_tu104_alu64_to(b, dst, MME_TU104_ALU_OP_MUL, MME_TU104_ALU_OP_MULH,
                      mme_value64(x, mme_zero()),
                      mme_value64(y, mme_zero()));
}

static inline struct mme_value64
mme_imul_32x32_64(struct mme_builder *b,
                  struct mme_value x, struct mme_value y)
{
   return mme_tu104_alu64(b, MME_TU104_ALU_OP_MUL, MME_TU104_ALU_OP_MULH,
                          mme_value64(x, mme_zero()),
                          mme_value64(y, mme_zero()));
}

static inline void
mme_umul_32x32_64_to(struct mme_builder *b, struct mme_value64 dst,
                     struct mme_value x, struct mme_value y)
{
   mme_tu104_alu64_to(b, dst, MME_TU104_ALU_OP_MULU, MME_TU104_ALU_OP_MULH,
                      mme_value64(x, mme_zero()),
                      mme_value64(y, mme_zero()));
}

static inline struct mme_value64
mme_umul_32x32_64(struct mme_builder *b,
                  struct mme_value x, struct mme_value y)
{
   return mme_tu104_alu64(b, MME_TU104_ALU_OP_MULU, MME_TU104_ALU_OP_MULH,
                          mme_value64(x, mme_zero()),
                          mme_value64(y, mme_zero()));
}

static inline void
mme_merge_to(struct mme_builder *b, struct mme_value dst,
             struct mme_value x, struct mme_value y,
             uint16_t dst_pos, uint16_t bits, uint16_t src_pos)
{
   assert(dst_pos < 32);
   assert(bits < 32);
   assert(src_pos < 32);
   mme_tu104_alu_to(b, dst, MME_TU104_ALU_OP_MERGE, x, y,
                    (dst_pos << 10) | (bits << 5) | src_pos);
}

static inline struct mme_value
mme_merge(struct mme_builder *b,
          struct mme_value x, struct mme_value y,
          uint16_t dst_pos, uint16_t bits, uint16_t src_pos)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_merge_to(b, dst, x, y, dst_pos, bits, src_pos);
   return dst;
}

#define mme_set_field(b, x, FIELD, val) \
   mme_merge_to(b, x, x, val, DRF_LO(FIELD), DRF_BITS(FIELD), 0)

#define mme_set_field_enum(b, x, FIELD, ENUM) \
   mme_set_field(b, x, FIELD, mme_imm(FIELD##_##ENUM)) \

static inline void
mme_state_arr_to(struct mme_builder *b, struct mme_value dst,
                 uint16_t state, struct mme_value index)
{
   assert(state % 4 == 0);
   mme_tu104_alu_to(b, dst, MME_TU104_ALU_OP_STATE,
                    mme_imm(state >> 2), index, 0);
}

static inline void
mme_state_to(struct mme_builder *b, struct mme_value dst,
             uint16_t state)
{
   mme_state_arr_to(b, dst, state, mme_zero());
}

static inline struct mme_value
mme_state_arr(struct mme_builder *b,
              uint16_t state, struct mme_value index)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_state_arr_to(b, dst, state, index);
   return dst;
}

static inline struct mme_value
mme_state(struct mme_builder *b,
          uint16_t state)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_state_to(b, dst, state);
   return dst;
}

static inline void
mme_dwrite(struct mme_builder *b,
           struct mme_value idx, struct mme_value val)
{
   mme_tu104_alu_no_dst(b, MME_TU104_ALU_OP_DWRITE, idx, val, 0);
}

static inline void
mme_load_to(struct mme_builder *b, struct mme_value dst)
{
   mme_tu104_load_to(b, dst);
}

static inline struct mme_value
mme_load(struct mme_builder *b)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_load_to(b, dst);
   return dst;
}

static inline void
mme_mthd_arr(struct mme_builder *b, uint16_t mthd,
             struct mme_value index)
{
   mme_tu104_mthd(b, mthd, index);
}

static inline void
mme_mthd(struct mme_builder *b, uint16_t mthd)
{
   mme_mthd_arr(b, mthd, mme_zero());
}

static inline void
mme_emit(struct mme_builder *b,
         struct mme_value data)
{
   mme_tu104_emit(b, data);
}

static inline void
mme_start_loop(struct mme_builder *b, struct mme_value count)
{
   mme_tu104_start_loop(b, count);
}

static inline void
mme_end_loop(struct mme_builder *b)
{
   mme_tu104_end_loop(b);
}

#define mme_loop(b, count) \
   for (bool run = (mme_start_loop((b), count), true); run; \
        run = false, mme_end_loop(b))

#define MME_DEF_START_IF(op, OP, if_true)                         \
static inline void                                                \
mme_start_if_##op(struct mme_builder *b,                          \
                  struct mme_value x, struct mme_value y)         \
{                                                                 \
   mme_tu104_start_if(b, MME_TU104_ALU_OP_##OP, if_true, x, y);   \
}

MME_DEF_START_IF(ilt,   BLT,  true)
MME_DEF_START_IF(ult,   BLTU, true)
MME_DEF_START_IF(ile,   BLE,  true)
MME_DEF_START_IF(ule,   BLEU, true)
MME_DEF_START_IF(ieq,   BEQ,  true)
MME_DEF_START_IF(ige,   BLT,  false)
MME_DEF_START_IF(uge,   BLTU, false)
MME_DEF_START_IF(igt,   BLE,  false)
MME_DEF_START_IF(ugt,   BLEU, false)
MME_DEF_START_IF(ine,   BEQ,  false)

#undef MME_DEF_START_IF

static inline void
mme_end_if(struct mme_builder *b)
{
   mme_tu104_end_if(b);
}

#define mme_if(b, cmp, x, y) \
   for (bool run = (mme_start_if_##cmp((b), x, y), true); run; \
        run = false, mme_end_if(b))

static inline void
mme_start_while(struct mme_builder *b)
{
   mme_tu104_start_while(b);
}

#define MME_DEF_END_WHILE(op, OP, if_true)                        \
static inline void                                                \
mme_end_while_##op(struct mme_builder *b,                         \
                   struct mme_value x, struct mme_value y)        \
{                                                                 \
   mme_tu104_end_while(b, MME_TU104_ALU_OP_##OP, if_true, x, y);  \
}

MME_DEF_END_WHILE(ilt,   BLT,  true)
MME_DEF_END_WHILE(ult,   BLTU, true)
MME_DEF_END_WHILE(ile,   BLE,  true)
MME_DEF_END_WHILE(ule,   BLEU, true)
MME_DEF_END_WHILE(ieq,   BEQ,  true)
MME_DEF_END_WHILE(ige,   BLT,  false)
MME_DEF_END_WHILE(uge,   BLTU, false)
MME_DEF_END_WHILE(igt,   BLE,  false)
MME_DEF_END_WHILE(ugt,   BLEU, false)
MME_DEF_END_WHILE(ine,   BEQ,  false)

#define mme_while(b, cmp, x, y) \
   for (bool run = (mme_start_while(b), true); run; \
        run = false, mme_end_while_##cmp((b), x, y))

#ifdef __cplusplus
}
#endif

#endif /* MME_BUILDER_H */

