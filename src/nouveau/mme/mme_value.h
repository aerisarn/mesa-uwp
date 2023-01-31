#ifndef MME_VALUE_H
#define MME_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/macros.h"

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

static inline bool
mme_is_zero(struct mme_value x)
{
   switch (x.type) {
   case MME_VALUE_TYPE_ZERO:  return true;
   case MME_VALUE_TYPE_IMM:   return x.imm == 0;
   case MME_VALUE_TYPE_REG:   return false;
   default: unreachable("Invalid MME value type");
   }
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

#ifdef __cplusplus
}
#endif

#endif /* MME_VALUE_H */
