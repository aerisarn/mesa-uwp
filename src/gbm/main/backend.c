/*
 * Copyright © 2011 Intel Corporation
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "backend.h"

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define VER_MIN(a, b) ((a) < (b) ? (a) : (b))

extern const struct gbm_backend gbm_dri_backend;

struct gbm_backend_desc {
   const char *name;
   const struct gbm_backend *backend;
};

static const struct gbm_backend_desc builtin_backends[] = {
   { "dri", &gbm_dri_backend },
};

static struct gbm_device *
backend_create_device(const struct gbm_backend_desc *bd, int fd)
{
   const uint32_t abi_ver = VER_MIN(GBM_BACKEND_ABI_VERSION,
                                    bd->backend->v0.backend_version);
   struct gbm_device *dev = bd->backend->v0.create_device(fd, abi_ver);

   if (dev) {
      assert(abi_ver == dev->v0.backend_version);
      dev->v0.backend_desc = bd;
   }

   return dev;
}

static struct gbm_device *
find_backend(const char *name, int fd)
{
   struct gbm_device *dev = NULL;
   const struct gbm_backend_desc *bd;
   unsigned i;

   for (i = 0; i < ARRAY_SIZE(builtin_backends); ++i) {
      bd = &builtin_backends[i];

      if (name && strcmp(bd->name, name))
         continue;

      dev = backend_create_device(bd, fd);

      if (dev)
         break;
   }

   return dev;
}

static struct gbm_device *
override_backend(int fd)
{
   struct gbm_device *dev = NULL;
   const char *b;

   b = getenv("GBM_BACKEND");
   if (b)
      dev = find_backend(b, fd);

   return dev;
}

struct gbm_device *
_gbm_create_device(int fd)
{
   struct gbm_device *dev;

   dev = override_backend(fd);

   if (!dev)
      dev = find_backend(NULL, fd);

   return dev;
}

void
_gbm_device_destroy(struct gbm_device *gbm)
{
   gbm->v0.destroy(gbm);
}
