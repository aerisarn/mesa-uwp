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

#include "backend.h"

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

extern const struct gbm_backend gbm_dri_backend;

struct backend_desc {
   const char *name;
   const struct gbm_backend *backend;
};

static const struct backend_desc backends[] = {
   { "gbm_dri.so", &gbm_dri_backend },
};

static struct gbm_device *
find_backend(const char *name, int fd)
{
   struct gbm_device *dev = NULL;
   unsigned i;

   for (i = 0; i < ARRAY_SIZE(backends) && dev == NULL; ++i) {
      if (name && strcmp(backends[i].name, name))
         continue;

      dev = backends[i].backend->create_device(fd);
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
