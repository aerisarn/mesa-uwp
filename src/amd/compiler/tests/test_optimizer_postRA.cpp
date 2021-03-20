/*
 * Copyright © 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "helpers.h"

using namespace aco;

BEGIN_TEST(optimizer_postRA.vcmp)
    PhysReg reg_v0(256);
    PhysReg reg_s0(0);
    PhysReg reg_s2(2);
    PhysReg reg_s4(4);

    //>> v1: %a:v[0] = p_startpgm
    ASSERTED bool setup_ok = setup_cs("v1", GFX8);
    assert(setup_ok);

    auto &startpgm = bld.instructions->at(0);
    assert(startpgm->opcode == aco_opcode::p_startpgm);
    startpgm->definitions[0].setFixed(reg_v0);

    Temp v_in = inputs[0];

    {
        /* Recognize when the result of VOPC goes to VCC, and use that for the branching then. */

        //! s2: %b:vcc = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %e:s[2-3] = p_cbranch_z %b:vcc
        //! p_unit_test 0, %e:s[2-3]
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, vcc), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), bld.vcc(vcmp), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(0, Operand(br, reg_s2));
    }

    //; del b, e

    {
        /* When VCC is overwritten inbetween, don't optimize. */

        //! s2: %b:vcc = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:vcc, %x:exec
        //! s2: %f:vcc = s_mov_b64 0
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 1, %e:s[2-3], %f:vcc
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, vcc), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), bld.vcc(vcmp), Operand(exec, bld.lm));
        auto ovrwr = bld.sop1(Builder::s_mov, bld.def(bld.lm, vcc), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(1, Operand(br, reg_s2), Operand(ovrwr, vcc));
    }

    //; del b, c, d, e, f

    {
        /* When the result of VOPC goes to an SGPR pair other than VCC, don't optimize */

        //! s2: %b:s[4-5] = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:s[4-5], %x:exec
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 2, %e:s[2-3]
        auto vcmp = bld.vopc_e64(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, reg_s4), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), Operand(vcmp, reg_s4), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(2, Operand(br, reg_s2));
    }

    //; del b, c, d, e

    {
        /* When the VCC isn't written by VOPC, don't optimize */

        //! s2: %b:vcc, s1: %f:scc = s_or_b64 1, %0:s[4-5]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:vcc, %x:exec
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 2, %e:s[2-3]
        auto salu = bld.sop2(Builder::s_or, bld.def(bld.lm, vcc), bld.def(s1, scc), Operand(1u), Operand(reg_s4, bld.lm));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), Operand(salu, vcc), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(2, Operand(br, reg_s2));
    }

    //; del b, c, d, e, f, x

    {
        /* When EXEC is overwritten inbetween, don't optimize. */

        //! s2: %b:vcc = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:vcc, %x:exec
        //! s2: %f:exec = s_mov_b64 42
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 4, %e:s[2-3], %f:exec
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, vcc), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), bld.vcc(vcmp), Operand(exec, bld.lm));
        auto ovrwr = bld.sop1(Builder::s_mov, bld.def(bld.lm, exec), Operand(42u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(4, Operand(br, reg_s2), Operand(ovrwr, exec));
    }

    //; del b, c, d, e, f, x

    finish_optimizer_postRA_test();
END_TEST
