/*
 * (C) Copyright IBM Corporation 2002, 2004
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEM, IBM AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * \file utils.c
 * Utility functions for DRI drivers.
 *
 * \author Ian Romanick <idr@us.ibm.com>
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "main/cpuinfo.h"
#include "main/extensions.h"
#include "utils.h"
#include "dri_util.h"

/* WARNING: HACK: Local defines to avoid pulling glx.h.
 *
 * Any parts of this file that use the following defines are either partial or
 * entirely broken wrt EGL.
 *
 * For example any getConfigAttrib() or indexConfigAttrib() query from EGL for
 * SLOW or NON_CONFORMANT_CONFIG will not work as expected since the EGL tokens
 * are different from the GLX ones.
 */
#define GLX_NONE                                                0x8000
#define GLX_SLOW_CONFIG                                         0x8001
#define GLX_NON_CONFORMANT_CONFIG                               0x800D
#define GLX_DONT_CARE                                           0xFFFFFFFF

#define __ATTRIB(attrib, field) case attrib: *value = config->modes.field; break

/**
 * Return the value of a configuration attribute.  The attribute is
 * indicated by the index.
 */
static int
driGetConfigAttribIndex(const __DRIconfig *config,
			unsigned int index, unsigned int *value)
{
    switch (index + 1) {
    __ATTRIB(__DRI_ATTRIB_BUFFER_SIZE,			rgbBits);
    __ATTRIB(__DRI_ATTRIB_RED_SIZE,			redBits);
    __ATTRIB(__DRI_ATTRIB_GREEN_SIZE,			greenBits);
    __ATTRIB(__DRI_ATTRIB_BLUE_SIZE,			blueBits);
    case __DRI_ATTRIB_LEVEL:
    case __DRI_ATTRIB_LUMINANCE_SIZE:
    case __DRI_ATTRIB_AUX_BUFFERS:
        *value = 0;
        break;
    __ATTRIB(__DRI_ATTRIB_ALPHA_SIZE,			alphaBits);
    case __DRI_ATTRIB_ALPHA_MASK_SIZE:
        /* I have no idea what this value was ever meant to mean, it's
         * never been set to anything, just say 0.
         */
        *value = 0;
        break;
    __ATTRIB(__DRI_ATTRIB_DEPTH_SIZE,			depthBits);
    __ATTRIB(__DRI_ATTRIB_STENCIL_SIZE,			stencilBits);
    __ATTRIB(__DRI_ATTRIB_ACCUM_RED_SIZE,		accumRedBits);
    __ATTRIB(__DRI_ATTRIB_ACCUM_GREEN_SIZE,		accumGreenBits);
    __ATTRIB(__DRI_ATTRIB_ACCUM_BLUE_SIZE,		accumBlueBits);
    __ATTRIB(__DRI_ATTRIB_ACCUM_ALPHA_SIZE,		accumAlphaBits);
    case __DRI_ATTRIB_SAMPLE_BUFFERS:
        *value = !!config->modes.samples;
        break;
    __ATTRIB(__DRI_ATTRIB_SAMPLES,			samples);
    case __DRI_ATTRIB_RENDER_TYPE:
        /* no support for color index mode */
	*value = __DRI_ATTRIB_RGBA_BIT;
        if (config->modes.floatMode)
            *value |= __DRI_ATTRIB_FLOAT_BIT;
	break;
    case __DRI_ATTRIB_CONFIG_CAVEAT:
	if (config->modes.accumRedBits != 0)
	    *value = __DRI_ATTRIB_SLOW_BIT;
	else
	    *value = 0;
	break;
    case __DRI_ATTRIB_CONFORMANT:
        *value = GL_TRUE;
        break;
    __ATTRIB(__DRI_ATTRIB_DOUBLE_BUFFER,		doubleBufferMode);
    __ATTRIB(__DRI_ATTRIB_STEREO,			stereoMode);
    case __DRI_ATTRIB_TRANSPARENT_TYPE:
    case __DRI_ATTRIB_TRANSPARENT_INDEX_VALUE: /* horrible bc hack */
        *value = GLX_NONE;
        break;
    case __DRI_ATTRIB_TRANSPARENT_RED_VALUE:
    case __DRI_ATTRIB_TRANSPARENT_GREEN_VALUE:
    case __DRI_ATTRIB_TRANSPARENT_BLUE_VALUE:
    case __DRI_ATTRIB_TRANSPARENT_ALPHA_VALUE:
        *value = GLX_DONT_CARE;
        break;
    case __DRI_ATTRIB_FLOAT_MODE:
        *value = config->modes.floatMode;
        break;
    __ATTRIB(__DRI_ATTRIB_RED_MASK,			redMask);
    __ATTRIB(__DRI_ATTRIB_GREEN_MASK,			greenMask);
    __ATTRIB(__DRI_ATTRIB_BLUE_MASK,			blueMask);
    __ATTRIB(__DRI_ATTRIB_ALPHA_MASK,			alphaMask);
    case __DRI_ATTRIB_MAX_PBUFFER_WIDTH:
    case __DRI_ATTRIB_MAX_PBUFFER_HEIGHT:
    case __DRI_ATTRIB_MAX_PBUFFER_PIXELS:
    case __DRI_ATTRIB_OPTIMAL_PBUFFER_WIDTH:
    case __DRI_ATTRIB_OPTIMAL_PBUFFER_HEIGHT:
    case __DRI_ATTRIB_VISUAL_SELECT_GROUP:
        *value = 0;
        break;
    __ATTRIB(__DRI_ATTRIB_SWAP_METHOD,			swapMethod);
    case __DRI_ATTRIB_MAX_SWAP_INTERVAL:
        *value = INT_MAX;
        break;
    case __DRI_ATTRIB_MIN_SWAP_INTERVAL:
        *value = 0;
        break;
    case __DRI_ATTRIB_BIND_TO_TEXTURE_RGB:
    case __DRI_ATTRIB_BIND_TO_TEXTURE_RGBA:
    case __DRI_ATTRIB_YINVERTED:
        *value = GL_TRUE;
        break;
    case __DRI_ATTRIB_BIND_TO_MIPMAP_TEXTURE:
        *value = GL_FALSE;
        break;
    case __DRI_ATTRIB_BIND_TO_TEXTURE_TARGETS:
        *value = __DRI_ATTRIB_TEXTURE_1D_BIT |
                 __DRI_ATTRIB_TEXTURE_2D_BIT |
                 __DRI_ATTRIB_TEXTURE_RECTANGLE_BIT;
        break;
    __ATTRIB(__DRI_ATTRIB_FRAMEBUFFER_SRGB_CAPABLE,	sRGBCapable);
    case __DRI_ATTRIB_MUTABLE_RENDER_BUFFER:
        *value = GL_FALSE;
        break;
    __ATTRIB(__DRI_ATTRIB_RED_SHIFT,			redShift);
    __ATTRIB(__DRI_ATTRIB_GREEN_SHIFT,			greenShift);
    __ATTRIB(__DRI_ATTRIB_BLUE_SHIFT,			blueShift);
    __ATTRIB(__DRI_ATTRIB_ALPHA_SHIFT,			alphaShift);
    default:
        /* XXX log an error or smth */
        return GL_FALSE;
    }

    return GL_TRUE;
}

/**
 * Get the value of a configuration attribute.
 * \param attrib  the attribute (one of the _DRI_ATTRIB_x tokens)
 * \param value  returns the attribute's value
 * \return 1 for success, 0 for failure
 */
int
driGetConfigAttrib(const __DRIconfig *config,
		   unsigned int attrib, unsigned int *value)
{
    return driGetConfigAttribIndex(config, attrib - 1, value);
}


/**
 * Get a configuration attribute name and value, given an index.
 * \param index  which field of the __DRIconfig to query
 * \param attrib  returns the attribute name (one of the _DRI_ATTRIB_x tokens)
 * \param value  returns the attribute's value
 * \return 1 for success, 0 for failure
 */
int
driIndexConfigAttrib(const __DRIconfig *config, int index,
		     unsigned int *attrib, unsigned int *value)
{
    if (driGetConfigAttribIndex(config, index, value)) {
        *attrib = index + 1;
        return GL_TRUE;
    }

    return GL_FALSE;
}

