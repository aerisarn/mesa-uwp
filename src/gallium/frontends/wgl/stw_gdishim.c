/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Certain Win32-like platforms (i.e. Xbox GDK) do not support the GDI library.
 * stw_gdishim acts as a shim layer to provide the APIs required for gallium.
 */

#if defined _GAMING_XBOX || defined _XBOX_UWP

#include "stw_gdishim.h"
#include "stw_pixelformat.h"
#include "stw_framebuffer.h"

int GetPixelFormat(HDC hdc)
{
   #ifdef _XBOX_UWP
   //We can't recurse back into the stw because we don't have a framebuffer.
   //TODO: return the actual format
   return 1;
   #endif
   return stw_pixelformat_get(hdc);
}

int DescribePixelFormat(
   HDC                     hdc,
   int                     iPixelFormat,
   UINT                    nBytes,
   LPPIXELFORMATDESCRIPTOR ppfd
)
{
//TODO: actually describe the surface
#ifdef _XBOX_UWP

    ppfd->nSize               = sizeof(PIXELFORMATDESCRIPTOR);
    ppfd->nVersion            = 1;                           // Version Number
    ppfd->dwFlags             = PFD_DRAW_TO_WINDOW |         // Format Must Support Window
                                PFD_SUPPORT_OPENGL |         // Format Must Support OpenGL
                                PFD_SUPPORT_COMPOSITION |    // Format Must Support Composition
                                PFD_DOUBLEBUFFER;            // Must Support Double Buffering
    ppfd->iPixelType          = PFD_TYPE_RGBA;               // Request An RGBA Format
    ppfd->cColorBits          = 32;                          // Select Our Color Depth
    ppfd->cRedBits            = 0;                           // Color Bits Ignored
    ppfd->cRedShift           = 0;                           // Color Bits Ignored
    ppfd->cGreenBits          = 0;                           // Color Bits Ignored
    ppfd->cGreenShift         = 0;                           // Color Bits Ignored
    ppfd->cBlueBits           = 0;                           // Color Bits Ignored
    ppfd->cBlueShift          = 0;                           // Color Bits Ignored
    ppfd->cAlphaBits          = 8;                           // An Alpha Buffer
    ppfd->cAlphaShift         = 0;                           // Shift Bit Ignored
    ppfd->cAccumBits          = 0;                           // No Accumulation Buffer
    ppfd->cAccumRedBits       = 0;                           // Accumulation Bits Ignored
    ppfd->cAccumGreenBits     = 0;                           // Accumulation Bits Ignored
    ppfd->cAccumBlueBits      = 0;                           // Accumulation Bits Ignored
    ppfd->cAccumAlphaBits     = 0;                           // Accumulation Bits Ignored
    ppfd->cDepthBits          = 24;                          // 16Bit Z-Buffer (Depth Buffer)
    ppfd->cStencilBits        = 8;                           // Some Stencil Buffer
    ppfd->cAuxBuffers         = 0;                           // No Auxiliary Buffer
    ppfd->iLayerType          = PFD_MAIN_PLANE,              // Main Drawing Layer
    ppfd->bReserved           = 0;                           // Reserved
    ppfd->dwLayerMask         = 0;                           // Layer Masks Ignored
    ppfd->dwVisibleMask       = 0;                           // Layer Masks Ignored
    ppfd->dwDamageMask        = 0;                           // Layer Masks Ignored
    return 1;
#else

   if (iPixelFormat >= stw_pixelformat_get_count(hdc))
      return 0;

   const struct stw_pixelformat_info* info = stw_pixelformat_get_info(iPixelFormat);
   memcpy(ppfd, &info->pfd, nBytes);
   return 1;
#endif
}

BOOL SetPixelFormat(
   HDC                         hdc,
   int                         format,
   const PIXELFORMATDESCRIPTOR* ppfd
)
{
// TODO: can we support this?
#if 0
   struct stw_framebuffer* fb;

   fb = stw_framebuffer_from_hdc(hdc);
   if (fb && fb->pfi) {
      fb->pfi->iPixelFormat = format;
      stw_framebuffer_unlock(fb);
      return true;
   }
#endif
   return true;
}

#ifndef _XBOX_UWP
void StretchDIBits(HDC hdc, unsigned int xDest, unsigned int yDest, unsigned int DestWidth, unsigned int DestHeight, unsigned int xSrc, unsigned int ySrc, unsigned int SrcWidth, unsigned int SrcHeight, void* lpBits, void* lpbmi, unsigned int iUsage, DWORD rop)
{

}
#endif

#endif /* GAMING_XBOX || defined _XBOX_UWP */
