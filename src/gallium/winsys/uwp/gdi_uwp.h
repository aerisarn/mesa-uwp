#pragma once

#include <Windows.h>

/* Layer plane descriptor */
typedef struct tagLAYERPLANEDESCRIPTOR { // lpd
   WORD  nSize;
   WORD  nVersion;
   DWORD dwFlags;
   BYTE  iPixelType;
   BYTE  cColorBits;
   BYTE  cRedBits;
   BYTE  cRedShift;
   BYTE  cGreenBits;
   BYTE  cGreenShift;
   BYTE  cBlueBits;
   BYTE  cBlueShift;
   BYTE  cAlphaBits;
   BYTE  cAlphaShift;
   BYTE  cAccumBits;
   BYTE  cAccumRedBits;
   BYTE  cAccumGreenBits;
   BYTE  cAccumBlueBits;
   BYTE  cAccumAlphaBits;
   BYTE  cDepthBits;
   BYTE  cStencilBits;
   BYTE  cAuxBuffers;
   BYTE  iLayerPlane;
   BYTE  bReserved;
   COLORREF crTransparent;
} LAYERPLANEDESCRIPTOR, * PLAYERPLANEDESCRIPTOR, FAR* LPLAYERPLANEDESCRIPTOR;

/* WGL */
typedef struct _WGLSWAP
{
   HDC hdc;
   UINT uiFlags;
} WGLSWAP;

#define WGL_SWAPMULTIPLE_MAX 16

WINGDIAPI DWORD WINAPI
wglSwapMultipleBuffers(UINT n,
   CONST WGLSWAP* ps);

WINGDIAPI BOOL  WINAPI wglDeleteContext(HGLRC);

/* wglSwapLayerBuffers flags */
#define WGL_SWAP_MAIN_PLANE     0x00000001

/* Glyph Info */

typedef struct tagGLYPHMETRICSFLOAT {
   FLOAT      gmfBlackBoxX;
   FLOAT      gmfBlackBoxY;
   FLOAT gmfptGlyphOriginX;
   FLOAT gmfptGlyphOriginY;
   FLOAT      gmfCellIncX;
   FLOAT      gmfCellIncY;
} GLYPHMETRICSFLOAT, * PGLYPHMETRICSFLOAT, * LPGLYPHMETRICSFLOAT;

/* Bitmap Header */

typedef long FXPT2DOT30;

typedef struct {
   DWORD        bV5Size;
   LONG         bV5Width;
   LONG         bV5Height;
   WORD         bV5Planes;
   WORD         bV5BitCount;
   DWORD        bV5Compression;
   DWORD        bV5SizeImage;
   LONG         bV5XPelsPerMeter;
   LONG         bV5YPelsPerMeter;
   DWORD        bV5ClrUsed;
   DWORD        bV5ClrImportant;
   DWORD        bV5RedMask;
   DWORD        bV5GreenMask;
   DWORD        bV5BlueMask;
   DWORD        bV5AlphaMask;
   DWORD        bV5CSType;
   CIEXYZTRIPLE bV5Endpoints;
   DWORD        bV5GammaRed;
   DWORD        bV5GammaGreen;
   DWORD        bV5GammaBlue;
   DWORD        bV5Intent;
   DWORD        bV5ProfileData;
   DWORD        bV5ProfileSize;
   DWORD        bV5Reserved;
} BITMAPV5HEADER, * LPBITMAPV5HEADER, * PBITMAPV5HEADER;

WINGDIAPI BOOL  WINAPI wglDeleteContext(HGLRC);

void StretchDIBits(HDC hdc, unsigned int xDest, unsigned int yDest, unsigned int DestWidth, unsigned int DestHeight, unsigned int xSrc, unsigned int ySrc, unsigned int SrcWidth, unsigned int SrcHeight, void* lpBits, void* lpbmi, unsigned int iUsage, DWORD rop);

#ifdef __cplusplus
extern "C"
{
#endif

      HWND
      WINAPI
      WindowFromDC(
         _In_ HDC hDC);

      HDC
      WINAPI
      GetDC(
         _In_opt_ HWND hWnd);

      int
         WINAPI
         ReleaseDC(
            _In_opt_ HWND hWnd,
            _In_ HDC hDC);

   BOOL
      WINAPI
      GetClientRect(
         _In_ HWND hWnd,
         _Out_ LPRECT lpRect);

   int   WINAPI GetPixelFormat(_In_ HDC hdc);

   BOOL SetPixelFormat(
      HDC                         hdc,
      int                         format,
      const PIXELFORMATDESCRIPTOR* ppfd);

   int  WINAPI DescribePixelFormat(_In_ HDC hdc,
      _In_ int iPixelFormat,
      _In_ UINT nBytes,
      _Out_writes_bytes_opt_(nBytes) LPPIXELFORMATDESCRIPTOR ppfd);


      HWND
      WINAPI
      CreateWindowEx(
         _In_ DWORD dwExStyle,
         _In_opt_ LPCSTR lpClassName,
         _In_opt_ LPCSTR lpWindowName,
         _In_ DWORD dwStyle,
         _In_ int X,
         _In_ int Y,
         _In_ int nWidth,
         _In_ int nHeight,
         _In_opt_ HWND hWndParent,
         _In_opt_ HMENU hMenu,
         _In_opt_ HINSTANCE hInstance,
         _In_opt_ LPVOID lpParam);

      BOOL
      WINAPI
      AdjustWindowRectEx(
         _Inout_ LPRECT lpRect,
         _In_ DWORD dwStyle,
         _In_ BOOL bMenu,
         _In_ DWORD dwExStyle);

         BOOL
         WINAPI
         DestroyWindow(
            _In_ HWND hWnd);

#ifdef __cplusplus
}
#endif
