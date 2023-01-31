#ifndef MME_BUILDER_H
#define MME_BUILDER_H

#include "mme_value.h"
#include "mme_tu104.h"

#include "util/bitscan.h"
#include "util/enum_operators.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mme_alu_op {
   MME_ALU_OP_ADD,
   MME_ALU_OP_ADDC,
   MME_ALU_OP_SUB,
   MME_ALU_OP_SUBB,
   MME_ALU_OP_MUL,
   MME_ALU_OP_MULH,
   MME_ALU_OP_MULU,
   MME_ALU_OP_CLZ,
   MME_ALU_OP_SLL,
   MME_ALU_OP_SRL,
   MME_ALU_OP_SRA,
   MME_ALU_OP_AND,
   MME_ALU_OP_NAND,
   MME_ALU_OP_OR,
   MME_ALU_OP_XOR,
   MME_ALU_OP_SLT,
   MME_ALU_OP_SLTU,
   MME_ALU_OP_SLE,
   MME_ALU_OP_SLEU,
   MME_ALU_OP_SEQ,
   MME_ALU_OP_DREAD,
   MME_ALU_OP_DWRITE,
};

enum mme_cmp_op {
   MME_CMP_OP_LT,
   MME_CMP_OP_LTU,
   MME_CMP_OP_LE,
   MME_CMP_OP_LEU,
   MME_CMP_OP_EQ,
};

struct mme_builder;

#include "mme_tu104_builder.h"

struct mme_builder {
   struct mme_reg_alloc reg_alloc;

   struct mme_tu104_builder tu104;
};

static inline void
mme_builder_init(struct mme_builder *b)
{
   memset(b, 0, sizeof(*b));
   mme_tu104_builder_init(b);
}

static inline uint32_t *
mme_builder_finish(struct mme_builder *b, size_t *size_out)
{
   return mme_tu104_builder_finish(&b->tu104, size_out);
}

static inline struct mme_value
mme_alloc_reg(struct mme_builder *b)
{
   return mme_reg_alloc_alloc(&b->reg_alloc);
}

static inline void
mme_free_reg(struct mme_builder *b, struct mme_value val)
{
   mme_reg_alloc_free(&b->reg_alloc, val);
}

static inline void
mme_alu_to(struct mme_builder *b,
           struct mme_value dst,
           enum mme_alu_op op,
           struct mme_value x,
           struct mme_value y)
{
   mme_tu104_alu_to(b, dst, op, x, y);
}

static inline struct mme_value
mme_alu(struct mme_builder *b,
        enum mme_alu_op op,
        struct mme_value x,
        struct mme_value y)
{
   struct mme_value dst = mme_alloc_reg(b);
   mme_alu_to(b, dst, op, x, y);
   return dst;
}

static inline void
mme_alu_no_dst(struct mme_builder *b,
               enum mme_alu_op op,
               struct mme_value x,
               struct mme_value y)
{
   mme_alu_to(b, mme_zero(), op, x, y);
}

static inline void
mme_alu64_to(struct mme_builder *b,
             struct mme_value64 dst,
             enum mme_alu_op op_lo,
             enum mme_alu_op op_hi,
             struct mme_value64 x,
             struct mme_value64 y)
{
   mme_tu104_alu64_to(b, dst, op_lo, op_hi, x, y);
}

static inline struct mme_value64
mme_alu64(struct mme_builder *b,
          enum mme_alu_op op_lo, enum mme_alu_op op_hi,
          struct mme_value64 x, struct mme_value64 y)
{
   struct mme_value64 dst = {
      mme_alloc_reg(b),
      mme_alloc_reg(b),
   };
   mme_alu64_to(b, dst, op_lo, op_hi, x, y);
   return dst;
}

#define MME_DEF_ALU1(op, OP)                                \
static inline void                                          \
mme_##op##_to(struct mme_builder *b, struct mme_value dst,  \
              struct mme_value x)                           \
{                                                           \
   mme_alu_to(b, dst, MME_ALU_OP_##OP, x, mme_zero());      \
}                                                           \
                                                            \
static inline struct mme_value                              \
mme_##op(struct mme_builder *b,                             \
         struct mme_value x)                                \
{                                                           \
   return mme_alu(b, MME_ALU_OP_##OP, x, mme_zero());       \
}

#define MME_DEF_ALU2(op, OP)                                \
static inline void                                          \
mme_##op##_to(struct mme_builder *b, struct mme_value dst,  \
              struct mme_value x, struct mme_value y)       \
{                                                           \
   mme_alu_to(b, dst, MME_ALU_OP_##OP, x, y);               \
}                                                           \
                                                            \
static inline struct mme_value                              \
mme_##op(struct mme_builder *b,                             \
         struct mme_value x, struct mme_value y)            \
{                                                           \
   return mme_alu(b, MME_ALU_OP_##OP, x, y);                \
}

MME_DEF_ALU1(mov,    ADD);
MME_DEF_ALU2(add,    ADD);
MME_DEF_ALU2(sub,    SUB);
MME_DEF_ALU2(mul,    MUL);
MME_DEF_ALU1(clz,    CLZ);
MME_DEF_ALU2(sll,    SLL);
MME_DEF_ALU2(srl,    SRL);
MME_DEF_ALU2(sra,    SRA);
MME_DEF_ALU2(and,    AND);
MME_DEF_ALU2(nand,   NAND);
MME_DEF_ALU2(or,     OR);
MME_DEF_ALU2(xor,    XOR);
MME_DEF_ALU2(slt,    SLT);
MME_DEF_ALU2(sltu,   SLTU);
MME_DEF_ALU2(sle,    SLE);
MME_DEF_ALU2(sleu,   SLEU);
MME_DEF_ALU2(seq,    SEQ);
MME_DEF_ALU1(dread,  DREAD);

#undef MME_DEF_ALU1
#undef MME_DEF_ALU2

static inline void
mme_mov64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x)
{
   mme_alu64_to(b, dst, MME_ALU_OP_ADD, MME_ALU_OP_ADD, x, mme_imm64(0));
}

static inline struct mme_value64
mme_mov64(struct mme_builder *b, struct mme_value64 x)
{
   return mme_alu64(b, MME_ALU_OP_ADD, MME_ALU_OP_ADD, x, mme_imm64(0));
}

static inline void
mme_add64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x, struct mme_value64 y)
{
   mme_alu64_to(b, dst, MME_ALU_OP_ADD, MME_ALU_OP_ADDC, x, y);
}

static inline struct mme_value64
mme_add64(struct mme_builder *b,
          struct mme_value64 x, struct mme_value64 y)
{
   return mme_alu64(b, MME_ALU_OP_ADD, MME_ALU_OP_ADDC, x, y);
}

static inline void
mme_sub64_to(struct mme_builder *b, struct mme_value64 dst,
             struct mme_value64 x, struct mme_value64 y)
{
   mme_alu64_to(b, dst, MME_ALU_OP_SUB, MME_ALU_OP_SUBB, x, y);
}

static inline struct mme_value64
mme_sub64(struct mme_builder *b,
          struct mme_value64 x, struct mme_value64 y)
{
   return mme_alu64(b, MME_ALU_OP_SUB, MME_ALU_OP_SUBB, x, y);
}

static inline void
mme_imul_32x32_64_to(struct mme_builder *b, struct mme_value64 dst,
                     struct mme_value x, struct mme_value y)
{
   mme_alu64_to(b, dst, MME_ALU_OP_MUL, MME_ALU_OP_MULH,
                mme_value64(x, mme_zero()),
                mme_value64(y, mme_zero()));
}

static inline struct mme_value64
mme_imul_32x32_64(struct mme_builder *b,
                  struct mme_value x, struct mme_value y)
{
   return mme_alu64(b, MME_ALU_OP_MUL, MME_ALU_OP_MULH,
                    mme_value64(x, mme_zero()),
                    mme_value64(y, mme_zero()));
}

static inline void
mme_umul_32x32_64_to(struct mme_builder *b, struct mme_value64 dst,
                     struct mme_value x, struct mme_value y)
{
   mme_alu64_to(b, dst, MME_ALU_OP_MULU, MME_ALU_OP_MULH,
                mme_value64(x, mme_zero()),
                mme_value64(y, mme_zero()));
}

static inline struct mme_value64
mme_umul_32x32_64(struct mme_builder *b,
                  struct mme_value x, struct mme_value y)
{
   return mme_alu64(b, MME_ALU_OP_MULU, MME_ALU_OP_MULH,
                    mme_value64(x, mme_zero()),
                    mme_value64(y, mme_zero()));
}

static inline struct mme_value64
mme_mul64(struct mme_builder *b,
          struct mme_value64 x, struct mme_value64 y)
{
   if (mme_is_zero(x.hi) && mme_is_zero(y.hi))
      return mme_umul_32x32_64(b, x.lo, y.lo);

   struct mme_value64 dst = mme_umul_32x32_64(b, x.lo, y.lo);
   struct mme_value tmp = mme_alloc_reg(b);

   mme_mul_to(b, tmp, x.lo, y.hi);
   mme_add64_to(b, dst, dst, mme_value64(mme_zero(), tmp));

   mme_mul_to(b, tmp, x.hi, y.lo);
   mme_add64_to(b, dst, dst, mme_value64(mme_zero(), tmp));

   mme_free_reg(b, tmp);

   return dst;
}

static inline void
mme_merge_to(struct mme_builder *b, struct mme_value dst,
             struct mme_value x, struct mme_value y,
             uint16_t dst_pos, uint16_t bits, uint16_t src_pos)
{
   mme_tu104_merge_to(b, dst, x, y, dst_pos, bits, src_pos);
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
   mme_tu104_state_arr_to(b, dst, state, index);
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
   mme_alu_no_dst(b, MME_ALU_OP_DWRITE, idx, val);
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

static inline struct mme_value64
mme_load_addr64(struct mme_builder *b)
{
   struct mme_value hi = mme_load(b);
   struct mme_value lo = mme_load(b);
   return mme_value64(lo, hi);
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
mme_emit_addr64(struct mme_builder *b, struct mme_value64 addr)
{
   mme_emit(b, addr.hi);
   mme_emit(b, addr.lo);
}

static inline void
mme_tu104_read_fifoed(struct mme_builder *b,
                      struct mme_value64 addr,
                      struct mme_value count)
{
   mme_mthd(b, 0x0550 /* NVC597_SET_MME_MEM_ADDRESS_A */);
   mme_emit_addr64(b, addr);

   mme_mthd(b, 0x0560 /* NVC597_MME_DMA_READ_FIFOED */);
   mme_emit(b, count);

   mme_tu104_load_barrier(b);
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
   mme_tu104_start_if(b, MME_CMP_OP_##OP, if_true, x, y);         \
}

MME_DEF_START_IF(ilt,   LT,  true)
MME_DEF_START_IF(ult,   LTU, true)
MME_DEF_START_IF(ile,   LE,  true)
MME_DEF_START_IF(ule,   LEU, true)
MME_DEF_START_IF(ieq,   EQ,  true)
MME_DEF_START_IF(ige,   LT,  false)
MME_DEF_START_IF(uge,   LTU, false)
MME_DEF_START_IF(igt,   LE,  false)
MME_DEF_START_IF(ugt,   LEU, false)
MME_DEF_START_IF(ine,   EQ,  false)

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
   mme_tu104_end_while(b, MME_CMP_OP_##OP, if_true, x, y);        \
}

MME_DEF_END_WHILE(ilt,   LT,  true)
MME_DEF_END_WHILE(ult,   LTU, true)
MME_DEF_END_WHILE(ile,   LE,  true)
MME_DEF_END_WHILE(ule,   LEU, true)
MME_DEF_END_WHILE(ieq,   EQ,  true)
MME_DEF_END_WHILE(ige,   LT,  false)
MME_DEF_END_WHILE(uge,   LTU, false)
MME_DEF_END_WHILE(igt,   LE,  false)
MME_DEF_END_WHILE(ugt,   LEU, false)
MME_DEF_END_WHILE(ine,   EQ,  false)

#define mme_while(b, cmp, x, y) \
   for (bool run = (mme_start_while(b), true); run; \
        run = false, mme_end_while_##cmp((b), x, y))

#ifdef __cplusplus
}
#endif

#endif /* MME_BUILDER_H */

