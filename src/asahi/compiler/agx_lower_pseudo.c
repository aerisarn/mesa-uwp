/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_builder.h"
#include "agx_compiler.h"

/* Lower pseudo instructions created during optimization. */
static agx_instr *
lower(agx_builder *b, agx_instr *I)
{
   switch (I->op) {

   /* Various instructions are implemented as bitwise truth tables */
   case AGX_OPCODE_MOV:
      return agx_bitop_to(b, I->dest[0], I->src[0], agx_zero(), AGX_BITOP_MOV);

   case AGX_OPCODE_NOT:
      return agx_bitop_to(b, I->dest[0], I->src[0], agx_zero(), AGX_BITOP_NOT);

   case AGX_OPCODE_AND:
      return agx_bitop_to(b, I->dest[0], I->src[0], I->src[1], AGX_BITOP_AND);

   case AGX_OPCODE_XOR:
      return agx_bitop_to(b, I->dest[0], I->src[0], I->src[1], AGX_BITOP_XOR);

   case AGX_OPCODE_OR:
      return agx_bitop_to(b, I->dest[0], I->src[0], I->src[1], AGX_BITOP_OR);

   /* Writes to the nesting counter lowered to the real register */
   case AGX_OPCODE_NEST:
      return agx_mov_imm_to(b, agx_register(0, AGX_SIZE_16), I->imm);

   default:
      return NULL;
   }
}

void
agx_lower_pseudo(agx_context *ctx)
{
   agx_foreach_instr_global_safe(ctx, I) {
      agx_builder b = agx_init_builder(ctx, agx_before_instr(I));

      if (lower(&b, I))
         agx_remove_instruction(I);
   }
}
