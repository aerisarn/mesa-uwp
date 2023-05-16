/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * \file  programopt.c 
 * Vertex/Fragment program optimizations and transformations for program
 * options, etc.
 *
 * \author Brian Paul
 */


#include "util/glheader.h"
#include "main/context.h"
#include "prog_parameter.h"
#include "prog_statevars.h"
#include "program.h"
#include "programopt.h"
#include "prog_instruction.h"


/**
 * This function inserts instructions for coordinate modelview * projection
 * into a vertex program.
 * May be used to implement the position_invariant option.
 */
static void
insert_mvp_dp4_code(struct gl_context *ctx, struct gl_program *vprog)
{
   struct prog_instruction *newInst;
   const GLuint origLen = vprog->arb.NumInstructions;
   const GLuint newLen = origLen + 4;
   GLuint i;

   /*
    * Setup state references for the modelview/projection matrix.
    * XXX we should check if these state vars are already declared.
    */
   static const gl_state_index16 mvpState[4][STATE_LENGTH] = {
      { STATE_MVP_MATRIX, 0, 0, 0 },  /* state.matrix.mvp.row[0] */
      { STATE_MVP_MATRIX, 0, 1, 1 },  /* state.matrix.mvp.row[1] */
      { STATE_MVP_MATRIX, 0, 2, 2 },  /* state.matrix.mvp.row[2] */
      { STATE_MVP_MATRIX, 0, 3, 3 },  /* state.matrix.mvp.row[3] */
   };
   GLint mvpRef[4];

   for (i = 0; i < 4; i++) {
      mvpRef[i] = _mesa_add_state_reference(vprog->Parameters, mvpState[i]);
   }

   /* Alloc storage for new instructions */
   newInst = rzalloc_array(vprog, struct prog_instruction, newLen);
   if (!newInst) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY,
                  "glProgramString(inserting position_invariant code)");
      return;
   }

   /*
    * Generated instructions:
    * newInst[0] = DP4 result.position.x, mvp.row[0], vertex.position;
    * newInst[1] = DP4 result.position.y, mvp.row[1], vertex.position;
    * newInst[2] = DP4 result.position.z, mvp.row[2], vertex.position;
    * newInst[3] = DP4 result.position.w, mvp.row[3], vertex.position;
    */
   _mesa_init_instructions(newInst, 4);
   for (i = 0; i < 4; i++) {
      newInst[i].Opcode = OPCODE_DP4;
      newInst[i].DstReg.File = PROGRAM_OUTPUT;
      newInst[i].DstReg.Index = VARYING_SLOT_POS;
      newInst[i].DstReg.WriteMask = (WRITEMASK_X << i);
      newInst[i].SrcReg[0].File = PROGRAM_STATE_VAR;
      newInst[i].SrcReg[0].Index = mvpRef[i];
      newInst[i].SrcReg[0].Swizzle = SWIZZLE_NOOP;
      newInst[i].SrcReg[1].File = PROGRAM_INPUT;
      newInst[i].SrcReg[1].Index = VERT_ATTRIB_POS;
      newInst[i].SrcReg[1].Swizzle = SWIZZLE_NOOP;
   }

   /* Append original instructions after new instructions */
   _mesa_copy_instructions (newInst + 4, vprog->arb.Instructions, origLen);

   /* free old instructions */
   ralloc_free(vprog->arb.Instructions);

   /* install new instructions */
   vprog->arb.Instructions = newInst;
   vprog->arb.NumInstructions = newLen;
   vprog->info.inputs_read |= VERT_BIT_POS;
   vprog->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_POS);
}


static void
insert_mvp_mad_code(struct gl_context *ctx, struct gl_program *vprog)
{
   struct prog_instruction *newInst;
   const GLuint origLen = vprog->arb.NumInstructions;
   const GLuint newLen = origLen + 4;
   GLuint hposTemp;
   GLuint i;

   /*
    * Setup state references for the modelview/projection matrix.
    * XXX we should check if these state vars are already declared.
    */
   static const gl_state_index16 mvpState[4][STATE_LENGTH] = {
      { STATE_MVP_MATRIX_TRANSPOSE, 0, 0, 0 },
      { STATE_MVP_MATRIX_TRANSPOSE, 0, 1, 1 },
      { STATE_MVP_MATRIX_TRANSPOSE, 0, 2, 2 },
      { STATE_MVP_MATRIX_TRANSPOSE, 0, 3, 3 },
   };
   GLint mvpRef[4];

   for (i = 0; i < 4; i++) {
      mvpRef[i] = _mesa_add_state_reference(vprog->Parameters, mvpState[i]);
   }

   /* Alloc storage for new instructions */
   newInst = rzalloc_array(vprog, struct prog_instruction, newLen);
   if (!newInst) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY,
                  "glProgramString(inserting position_invariant code)");
      return;
   }

   /* TEMP hposTemp; */
   hposTemp = vprog->arb.NumTemporaries++;

   /*
    * Generated instructions:
    *    emit_op2(p, OPCODE_MUL, tmp, 0, swizzle1(src,X), mat[0]);
    *    emit_op3(p, OPCODE_MAD, tmp, 0, swizzle1(src,Y), mat[1], tmp);
    *    emit_op3(p, OPCODE_MAD, tmp, 0, swizzle1(src,Z), mat[2], tmp);
    *    emit_op3(p, OPCODE_MAD, dest, 0, swizzle1(src,W), mat[3], tmp);
    */
   _mesa_init_instructions(newInst, 4);

   newInst[0].Opcode = OPCODE_MUL;
   newInst[0].DstReg.File = PROGRAM_TEMPORARY;
   newInst[0].DstReg.Index = hposTemp;
   newInst[0].DstReg.WriteMask = WRITEMASK_XYZW;
   newInst[0].SrcReg[0].File = PROGRAM_INPUT;
   newInst[0].SrcReg[0].Index = VERT_ATTRIB_POS;
   newInst[0].SrcReg[0].Swizzle = SWIZZLE_XXXX;
   newInst[0].SrcReg[1].File = PROGRAM_STATE_VAR;
   newInst[0].SrcReg[1].Index = mvpRef[0];
   newInst[0].SrcReg[1].Swizzle = SWIZZLE_NOOP;

   for (i = 1; i <= 2; i++) {
      newInst[i].Opcode = OPCODE_MAD;
      newInst[i].DstReg.File = PROGRAM_TEMPORARY;
      newInst[i].DstReg.Index = hposTemp;
      newInst[i].DstReg.WriteMask = WRITEMASK_XYZW;
      newInst[i].SrcReg[0].File = PROGRAM_INPUT;
      newInst[i].SrcReg[0].Index = VERT_ATTRIB_POS;
      newInst[i].SrcReg[0].Swizzle = MAKE_SWIZZLE4(i,i,i,i);
      newInst[i].SrcReg[1].File = PROGRAM_STATE_VAR;
      newInst[i].SrcReg[1].Index = mvpRef[i];
      newInst[i].SrcReg[1].Swizzle = SWIZZLE_NOOP;
      newInst[i].SrcReg[2].File = PROGRAM_TEMPORARY;
      newInst[i].SrcReg[2].Index = hposTemp;
      newInst[1].SrcReg[2].Swizzle = SWIZZLE_NOOP;
   }

   newInst[3].Opcode = OPCODE_MAD;
   newInst[3].DstReg.File = PROGRAM_OUTPUT;
   newInst[3].DstReg.Index = VARYING_SLOT_POS;
   newInst[3].DstReg.WriteMask = WRITEMASK_XYZW;
   newInst[3].SrcReg[0].File = PROGRAM_INPUT;
   newInst[3].SrcReg[0].Index = VERT_ATTRIB_POS;
   newInst[3].SrcReg[0].Swizzle = SWIZZLE_WWWW;
   newInst[3].SrcReg[1].File = PROGRAM_STATE_VAR;
   newInst[3].SrcReg[1].Index = mvpRef[3];
   newInst[3].SrcReg[1].Swizzle = SWIZZLE_NOOP;
   newInst[3].SrcReg[2].File = PROGRAM_TEMPORARY;
   newInst[3].SrcReg[2].Index = hposTemp;
   newInst[3].SrcReg[2].Swizzle = SWIZZLE_NOOP;


   /* Append original instructions after new instructions */
   _mesa_copy_instructions (newInst + 4, vprog->arb.Instructions, origLen);

   /* free old instructions */
   ralloc_free(vprog->arb.Instructions);

   /* install new instructions */
   vprog->arb.Instructions = newInst;
   vprog->arb.NumInstructions = newLen;
   vprog->info.inputs_read |= VERT_BIT_POS;
   vprog->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_POS);
}


void
_mesa_insert_mvp_code(struct gl_context *ctx, struct gl_program *vprog)
{
   if (ctx->Const.ShaderCompilerOptions[MESA_SHADER_VERTEX].OptimizeForAOS)
      insert_mvp_dp4_code( ctx, vprog );
   else
      insert_mvp_mad_code( ctx, vprog );
}
