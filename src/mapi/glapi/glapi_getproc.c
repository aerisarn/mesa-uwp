/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
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
 * \file glapi_getproc.c
 *
 * Code for implementing glXGetProcAddress(), etc.
 * This was originally in glapi.c but refactored out.
 */


#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "glapi/glapi_priv.h"
#include "glapitable.h"


/**********************************************************************
 * Static function management.
 */


#if !defined(DISPATCH_FUNCTION_SIZE) 
# define NEED_FUNCTION_POINTER
#endif
#include "glprocs.h"


/**
 * Search the table of static entrypoint functions for the named function
 * and return the corresponding glprocs_table_t entry.
 */
static const glprocs_table_t *
get_static_proc( const char * n )
{
   GLuint i;
   for (i = 0; static_functions[i].Name_offset >= 0; i++) {
      const char *testName = gl_string_table + static_functions[i].Name_offset;
      if (strcmp(testName, n) == 0)
      {
	 return &static_functions[i];
      }
   }
   return NULL;
}


/**
 * Return dispatch table offset of the named static (built-in) function.
 * Return -1 if function not found.
 */
static GLint
get_static_proc_offset(const char *funcName)
{
   const glprocs_table_t * const f = get_static_proc( funcName );
   if (f == NULL) {
      return -1;
   }

   return f->Offset;
}



/**
 * Return dispatch function address for the named static (built-in) function.
 * Return NULL if function not found.
 */
static _glapi_proc
get_static_proc_address(const char *funcName)
{
   const glprocs_table_t * const f = get_static_proc( funcName );
   if (f == NULL) {
      return NULL;
   }

#if defined(DISPATCH_FUNCTION_SIZE) && defined(GLX_INDIRECT_RENDERING)
   return (f->Address == NULL)
      ? get_entrypoint_address(f->Offset)
      : f->Address;
#elif defined(DISPATCH_FUNCTION_SIZE)
   return get_entrypoint_address(f->Offset);
#else
   return f->Address;
#endif
}



/**
 * Return the name of the function at the given offset in the dispatch
 * table.  For debugging only.
 */
static const char *
get_static_proc_name( GLuint offset )
{
   GLuint i;
   for (i = 0; static_functions[i].Name_offset >= 0; i++) {
      if (static_functions[i].Offset == offset) {
	 return gl_string_table + static_functions[i].Name_offset;
      }
   }
   return NULL;
}



/**********************************************************************
 * Extension function management.
 */


/**
 * Track information about a function added to the GL API.
 */
struct _glapi_function {
   /**
    * Name of the function.
    */
   const char * name;
};


/**
 * This function is intended to be called by Mesa core, returning the dispatch
 * table offset for the passed set of aliased gl* functions.
 *
 * \param function_names       Array of pointers to function names that should
 *                             share a common dispatch offset.
 *
 * \returns
 * The offset in the dispatch table of the named function.  A pointer to the
 * driver's implementation of the named function should be stored at
 * \c dispatch_table[\c offset].  Return -1 if error/problem.
 *
 * \sa glXGetProcAddress
 *
 * \warning
 * This function can only handle up to 8 names at a time.  As far as I know,
 * the maximum number of names ever associated with an existing GL function is
 * 4 (\c glPointParameterfSGIS, \c glPointParameterfEXT,
 * \c glPointParameterfARB, and \c glPointParameterf), so this should not be
 * too painful of a limitation.
 *
 * \todo
 * Determine if code should be added to reject function names that start with
 * 'glX'.
 */

int
_glapi_add_dispatch(const char *const *function_names)
{
   unsigned i;
   int offset = ~0;

   init_glapi_relocs_once();

   /* Find the _single_ dispatch offset for all function names that already
    * exist (and have a dispatch offset).
    */

   for (i = 0; function_names[i] != NULL; i++) {
      const char *funcName = function_names[i];
      int static_offset;
      int extension_offset;

      if (funcName[0] != 'g' || funcName[1] != 'l')
         return -1;

      /* search built-in functions */
      static_offset = get_static_proc_offset(funcName);

      if (static_offset >= 0) {
         /* FIXME: Make sure the parameter signatures match!  How do we get
          * FIXME: the parameter signature for static functions?
          */

         if ((offset != ~0) && (static_offset != offset)) {
            return -1;
         }

         offset = static_offset;
      } else {
         return -1;
      }
   }

   return offset;
}

/**
 * Return offset of entrypoint for named function within dispatch table.
 */
GLint
_glapi_get_proc_offset(const char *funcName)
{
   /* search static functions */
   return get_static_proc_offset(funcName);
}



/**
 * Return pointer to the named function.  If the function name isn't found
 * in the name of static functions, try generating a new API entrypoint on
 * the fly with assembly language.
 */
_glapi_proc
_glapi_get_proc_address(const char *funcName)
{
   _glapi_proc func;
   struct _glapi_function * entry;

   init_glapi_relocs_once();

  if (!funcName || funcName[0] != 'g' || funcName[1] != 'l')
      return NULL;

   /* search static functions */
   func = get_static_proc_address(funcName);
   if (func)
      return func;

   return NULL;
}



/**
 * Return the name of the function at the given dispatch offset.
 * This is only intended for debugging.
 */
const char *
_glapi_get_proc_name(GLuint offset)
{
   const char * n;

   /* search built-in functions */
   n = get_static_proc_name(offset);
   if ( n != NULL ) {
      return n;
   }

   /* search added extension functions */
   return get_extension_proc_name(offset);
}



/**********************************************************************
 * GL API table functions.
 */


/**
 * Return size of dispatch table struct as number of functions (or
 * slots).
 */
GLuint
_glapi_get_dispatch_table_size(void)
{
   return sizeof(struct _glapi_table) / sizeof(void *);
}
