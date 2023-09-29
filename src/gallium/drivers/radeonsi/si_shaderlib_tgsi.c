/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_pipe.h"
#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_ureg.h"

/* Create the compute shader that is used to collect the results of gfx10+
 * shader queries.
 *
 * One compute grid with a single thread is launched for every query result
 * buffer. The thread (optionally) reads a previous summary buffer, then
 * accumulates data from the query result buffer, and writes the result either
 * to a summary buffer to be consumed by the next grid invocation or to the
 * user-supplied buffer.
 *
 * Data layout:
 *
 * BUFFER[0] = query result buffer (layout is defined by gfx10_sh_query_buffer_mem)
 * BUFFER[1] = previous summary buffer
 * BUFFER[2] = next summary buffer or user-supplied buffer
 *
 * CONST
 *  0.x = config; the low 3 bits indicate the mode:
 *          0: sum up counts
 *          1: determine result availability and write it as a boolean
 *          2: SO_OVERFLOW
 *          3: SO_ANY_OVERFLOW
 *        the remaining bits form a bitfield:
 *          8: write result as a 64-bit value
 *  0.y = offset in bytes to counts or stream for SO_OVERFLOW mode
 *  0.z = chain bit field:
 *          1: have previous summary buffer
 *          2: write next summary buffer
 *  0.w = result_count
 */
void *gfx11_create_sh_query_result_cs(struct si_context *sctx)
{
   /* TEMP[0].x = accumulated result so far
    * TEMP[0].y = result missing
    * TEMP[0].z = whether we're in overflow mode
    */
   static const char text_tmpl[] =
         "COMP\n"
         "PROPERTY CS_FIXED_BLOCK_WIDTH 1\n"
         "PROPERTY CS_FIXED_BLOCK_HEIGHT 1\n"
         "PROPERTY CS_FIXED_BLOCK_DEPTH 1\n"
         "DCL BUFFER[0]\n"
         "DCL BUFFER[1]\n"
         "DCL BUFFER[2]\n"
         "DCL CONST[0][0..0]\n"
         "DCL TEMP[0..5]\n"
         "IMM[0] UINT32 {0, 7, 256, 4294967295}\n"
         "IMM[1] UINT32 {1, 2, 4, 8}\n"
         "IMM[2] UINT32 {16, 32, 64, 128}\n"

         /* acc_result = 0;
          * acc_missing = 0;
          */
         "MOV TEMP[0].xy, IMM[0].xxxx\n"

         /* if (chain & 1) {
          *    acc_result = buffer[1][0];
          *    acc_missing = buffer[1][1];
          * }
          */
         "AND TEMP[5], CONST[0][0].zzzz, IMM[1].xxxx\n"
         "UIF TEMP[5]\n"
         "LOAD TEMP[0].xy, BUFFER[1], IMM[0].xxxx\n"
         "ENDIF\n"

         /* is_overflow (TEMP[0].z) = (config & 7) >= 2; */
         "AND TEMP[5].x, CONST[0][0].xxxx, IMM[0].yyyy\n"
         "USGE TEMP[0].z, TEMP[5].xxxx, IMM[1].yyyy\n"

         /* result_remaining (TEMP[1].x) = (is_overflow && acc_result) ? 0 : result_count; */
         "AND TEMP[5].x, TEMP[0].zzzz, TEMP[0].xxxx\n"
         "UCMP TEMP[1].x, TEMP[5].xxxx, IMM[0].xxxx, CONST[0][0].wwww\n"

         /* base_offset (TEMP[1].y) = 0; */
         "MOV TEMP[1].y, IMM[0].xxxx\n"

         /* for (;;) {
          *    if (!result_remaining) {
          *       break;
          *    }
          *    result_remaining--;
          */
         "BGNLOOP\n"
         "  USEQ TEMP[5], TEMP[1].xxxx, IMM[0].xxxx\n"
         "  UIF TEMP[5]\n"
         "     BRK\n"
         "  ENDIF\n"
         "  UADD TEMP[1].x, TEMP[1].xxxx, IMM[0].wwww\n"

         /*    fence = buffer[0]@(base_offset + sizeof(gfx10_sh_query_buffer_mem.stream)); */
         "  UADD TEMP[5].x, TEMP[1].yyyy, IMM[2].wwww\n"
         "  LOAD TEMP[5].x, BUFFER[0], TEMP[5].xxxx\n"

         /*    if (!fence) {
          *       acc_missing = ~0u;
          *       break;
          *    }
          */
         "  USEQ TEMP[5], TEMP[5].xxxx, IMM[0].xxxx\n"
         "  UIF TEMP[5]\n"
         "     MOV TEMP[0].y, TEMP[5].xxxx\n"
         "     BRK\n"
         "  ENDIF\n"

         /*    stream_offset (TEMP[2].x) = base_offset + offset; */
         "  UADD TEMP[2].x, TEMP[1].yyyy, CONST[0][0].yyyy\n"

         /*    if (!(config & 7)) {
          *       acc_result += buffer[0]@stream_offset;
          *    }
          */
         "  AND TEMP[5].x, CONST[0][0].xxxx, IMM[0].yyyy\n"
         "  USEQ TEMP[5], TEMP[5].xxxx, IMM[0].xxxx\n"
         "  UIF TEMP[5]\n"
         "     LOAD TEMP[5].x, BUFFER[0], TEMP[2].xxxx\n"
         "     UADD TEMP[0].x, TEMP[0].xxxx, TEMP[5].xxxx\n"
         "  ENDIF\n"

         /*    if ((config & 7) >= 2) {
          *       count (TEMP[2].y) = (config & 1) ? 4 : 1;
          */
         "  AND TEMP[5].x, CONST[0][0].xxxx, IMM[0].yyyy\n"
         "  USGE TEMP[5], TEMP[5].xxxx, IMM[1].yyyy\n"
         "  UIF TEMP[5]\n"
         "     AND TEMP[5].x, CONST[0][0].xxxx, IMM[1].xxxx\n"
         "     UCMP TEMP[2].y, TEMP[5].xxxx, IMM[1].zzzz, IMM[1].xxxx\n"

         /*       do {
          *          generated = buffer[0]@(stream_offset + 2 * sizeof(uint64_t));
          *          emitted = buffer[0]@(stream_offset + 3 * sizeof(uint64_t));
          *          if (generated != emitted) {
          *             acc_result = 1;
          *             result_remaining = 0;
          *             break;
          *          }
          *
          *          stream_offset += sizeof(gfx10_sh_query_buffer_mem.stream[0]);
          *       } while (--count);
          *    }
          */
         "     BGNLOOP\n"
         "        UADD TEMP[5].x, TEMP[2].xxxx, IMM[2].xxxx\n"
         "        LOAD TEMP[4].xyzw, BUFFER[0], TEMP[5].xxxx\n"
         "        USNE TEMP[5], TEMP[4].xyxy, TEMP[4].zwzw\n"
         "        UIF TEMP[5]\n"
         "           MOV TEMP[0].x, IMM[1].xxxx\n"
         "           MOV TEMP[1].y, IMM[0].xxxx\n"
         "           BRK\n"
         "        ENDIF\n"

         "        UADD TEMP[2].y, TEMP[2].yyyy, IMM[0].wwww\n"
         "        USEQ TEMP[5], TEMP[2].yyyy, IMM[0].xxxx\n"
         "        UIF TEMP[5]\n"
         "           BRK\n"
         "        ENDIF\n"
         "        UADD TEMP[2].x, TEMP[2].xxxx, IMM[2].yyyy\n"
         "     ENDLOOP\n"
         "  ENDIF\n"

         /*    base_offset += sizeof(gfx10_sh_query_buffer_mem);
          * } // end outer loop
          */
         "  UADD TEMP[1].y, TEMP[1].yyyy, IMM[0].zzzz\n"
         "ENDLOOP\n"

         /* if (chain & 2) {
          *    buffer[2][0] = acc_result;
          *    buffer[2][1] = acc_missing;
          * } else {
          */
         "AND TEMP[5], CONST[0][0].zzzz, IMM[1].yyyy\n"
         "UIF TEMP[5]\n"
         "  STORE BUFFER[2].xy, IMM[0].xxxx, TEMP[0]\n"
         "ELSE\n"

         /*    if ((config & 7) == 1) {
          *       acc_result = acc_missing ? 0 : 1;
          *       acc_missing = 0;
          *    }
          */
         "  AND TEMP[5], CONST[0][0].xxxx, IMM[0].yyyy\n"
         "  USEQ TEMP[5], TEMP[5].xxxx, IMM[1].xxxx\n"
         "  UIF TEMP[5]\n"
         "     UCMP TEMP[0].x, TEMP[0].yyyy, IMM[0].xxxx, IMM[1].xxxx\n"
         "     MOV TEMP[0].y, IMM[0].xxxx\n"
         "  ENDIF\n"

         /*    if (!acc_missing) {
          *       buffer[2][0] = acc_result;
          *       if (config & 8) {
          *          buffer[2][1] = 0;
          *       }
          *    }
          * }
          */
         "  USEQ TEMP[5], TEMP[0].yyyy, IMM[0].xxxx\n"
         "  UIF TEMP[5]\n"
         "     STORE BUFFER[2].x, IMM[0].xxxx, TEMP[0].xxxx\n"
         "     AND TEMP[5], CONST[0][0].xxxx, IMM[1].wwww\n"
         "     UIF TEMP[5]\n"
         "        STORE BUFFER[2].x, IMM[1].zzzz, TEMP[0].yyyy\n"
         "     ENDIF\n"
         "  ENDIF\n"
         "ENDIF\n"
         "END\n";

   struct tgsi_token tokens[1024];
   struct pipe_compute_state state = {};

   if (!tgsi_text_translate(text_tmpl, tokens, ARRAY_SIZE(tokens))) {
      assert(false);
      return NULL;
   }

   state.ir_type = PIPE_SHADER_IR_TGSI;
   state.prog = tokens;

   return sctx->b.create_compute_state(&sctx->b, &state);
}
