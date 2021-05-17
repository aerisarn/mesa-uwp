/*
 * Copyright © 2021 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <err.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rnn.h"
#include "rnndec.h"

#include "util.h"

static struct rnndeccontext *ctx;
static struct rnndb *db;
static struct rnndomain *control_regs;
struct rnndomain *dom[2];
static struct rnnenum *pm4_packets;

static int
find_reg(struct rnndomain *dom, const char *name)
{
   for (int i = 0; i < dom->subelemsnum; i++)
      if (!strcmp(name, dom->subelems[i]->name))
         return dom->subelems[i]->offset;

   return -1;
}

/**
 * Map control reg name to offset.
 */
unsigned
afuc_control_reg(const char *name)
{
   int val = find_reg(control_regs, name);
   if (val < 0) {
      char *endptr = NULL;
      val = strtol(name, &endptr, 0);
      if (endptr) {
         printf("invalid control reg: %s\n", name);
         exit(2);
      }
   }
   return (unsigned)val;
}

/**
 * Map offset to control reg name (or NULL), caller frees
 */
char *
afuc_control_reg_name(unsigned id)
{
   if (rnndec_checkaddr(ctx, control_regs, id, 0)) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(ctx, control_regs, id, 0);
      char *name = info->name;
      free(info);
      return name;
   } else {
      return NULL;
   }
}

/**
 * Map GPU reg name to offset.
 */
unsigned
afuc_gpu_reg(const char *name)
{
   int val = find_reg(dom[0], name);
   if (val < 0)
      val = find_reg(dom[1], name);
   if (val < 0) {
      char *endptr = NULL;
      val = strtol(name, &endptr, 0);
      if (endptr) {
         printf("invalid control reg: %s\n", name);
         exit(2);
      }
   }
   return (unsigned)val;
}

/**
 * Map offset to gpu reg name (or NULL), caller frees
 */
char *
afuc_gpu_reg_name(unsigned id)
{
   struct rnndomain *d = NULL;

   if (rnndec_checkaddr(ctx, dom[0], id, 0)) {
      d = dom[0];
   } else if (rnndec_checkaddr(ctx, dom[1], id, 0)) {
      d = dom[1];
   }

   if (d) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(ctx, d, id, 0);
      if (info) {
         char *name = info->name;
         free(info);
         return name;
      }
   }

   return NULL;
}

static int
find_enum_val(struct rnnenum *en, const char *name)
{
   int i;

   for (i = 0; i < en->valsnum; i++)
      if (en->vals[i]->valvalid && !strcmp(name, en->vals[i]->name))
         return en->vals[i]->value;

   return -1;
}

/**
 * Map pm4 packet name to id
 */
int
afuc_pm4_id(const char *name)
{
   return find_enum_val(pm4_packets, name);
}

const char *
afuc_pm_id_name(unsigned id)
{
   return rnndec_decode_enum(ctx, "adreno_pm4_type3_packets", id);
}

void
afuc_printc(enum afuc_color c, const char *fmt, ...)
{
   va_list args;
   if (c == AFUC_ERR) {
      printf("%s", ctx->colors->err);
   } else if (c == AFUC_LBL) {
      printf("%s", ctx->colors->btarg);
   }
   va_start(args, fmt);
   vprintf(fmt, args);
   va_end(args);
   printf("%s", ctx->colors->reset);
}

int afuc_util_init(int gpuver, bool colors)
{
   char *name, *control_reg_name;

   switch (gpuver) {
   case 6:
      name = "A6XX";
      control_reg_name = "A6XX_CONTROL_REG";
      break;
   case 5:
      name = "A5XX";
      control_reg_name = "A5XX_CONTROL_REG";
      break;
   default:
      fprintf(stderr, "unknown GPU version!\n");
      return -1;
   }

   rnn_init();
   db = rnn_newdb();

   ctx = rnndec_newcontext(db);
   ctx->colors = colors ? &envy_def_colors : &envy_null_colors;

   rnn_parsefile(db, "adreno.xml");
   rnn_prepdb(db);
   if (db->estatus)
      errx(db->estatus, "failed to parse register database");
   dom[0] = rnn_finddomain(db, name);
   dom[1] = rnn_finddomain(db, "AXXX");
   control_regs = rnn_finddomain(db, control_reg_name);

   rnndec_varadd(ctx, "chip", name);

   pm4_packets = rnn_findenum(ctx->db, "adreno_pm4_type3_packets");

   return 0;
}

