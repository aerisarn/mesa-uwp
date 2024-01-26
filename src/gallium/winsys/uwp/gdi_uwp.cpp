#include "gdi_uwp.h"

#include <wrl.h>
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <pix.h>
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <memory>
#include <vector>
#include <agile.h>
#include <concrt.h>

using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::System;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;

inline float ConvertDipsToPixels(float dips, float dpi)
{
   static const float dipsPerInch = 96.0f;
   return floorf(dips * dpi / dipsPerInch + 0.5f); // Arrotonda all'intero più vicino.
}

BOOL WINAPI GetClientRect( _In_ HWND hWnd, _Out_ LPRECT lpRect)
{
   if (nullptr != lpRect)
   {
      CoreWindow^ coreWindow = CoreWindow::GetForCurrentThread();
      DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();
      lpRect->top = 0;
      lpRect->bottom = ConvertDipsToPixels(coreWindow->Bounds.Bottom - coreWindow->Bounds.Top, currentDisplayInformation->LogicalDpi);
      lpRect->left = 0;
      lpRect->right = ConvertDipsToPixels(coreWindow->Bounds.Right - coreWindow->Bounds.Left, currentDisplayInformation->LogicalDpi);
      return TRUE;
   }
   return FALSE;
}

/*There is only a single corewindow in our model, and we don't even have control over it*/

HWND
WINAPI
WindowFromDC(
   _In_ HDC hDC)
{
   CoreWindow^ coreWindow = CoreWindow::GetForCurrentThread();
   Platform::Agile<Windows::UI::Core::CoreWindow> m_window;
   m_window = coreWindow;
   return (HWND)reinterpret_cast<IUnknown*>(m_window.Get());
}

HDC
WINAPI
GetDC(
   _In_opt_ HWND hWnd)
{
   return (HDC)hWnd;
}

int
WINAPI
ReleaseDC(
   _In_opt_ HWND hWnd,
   _In_ HDC hDC)
{
   return 1;
}

int WINAPI GetPixelFormat(HDC hdc)
{
   return 1;
}

int DescribePixelFormat(
   HDC                     hdc,
   int                     iPixelFormat,
   UINT                    nBytes,
   LPPIXELFORMATDESCRIPTOR ppfd
)
{

   ppfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
   ppfd->nVersion = 1;                           // Version Number
   ppfd->dwFlags = PFD_DRAW_TO_WINDOW |         // Format Must Support Window
      PFD_SUPPORT_OPENGL |         // Format Must Support OpenGL
      PFD_SUPPORT_COMPOSITION |    // Format Must Support Composition
      PFD_DOUBLEBUFFER;            // Must Support Double Buffering
   ppfd->iPixelType = PFD_TYPE_RGBA;               // Request An RGBA Format
   ppfd->cColorBits = 32;                          // Select Our Color Depth
   ppfd->cRedBits = 0;                           // Color Bits Ignored
   ppfd->cRedShift = 0;                           // Color Bits Ignored
   ppfd->cGreenBits = 0;                           // Color Bits Ignored
   ppfd->cGreenShift = 0;                           // Color Bits Ignored
   ppfd->cBlueBits = 0;                           // Color Bits Ignored
   ppfd->cBlueShift = 0;                           // Color Bits Ignored
   ppfd->cAlphaBits = 8;                           // An Alpha Buffer
   ppfd->cAlphaShift = 0;                           // Shift Bit Ignored
   ppfd->cAccumBits = 0;                           // No Accumulation Buffer
   ppfd->cAccumRedBits = 0;                           // Accumulation Bits Ignored
   ppfd->cAccumGreenBits = 0;                           // Accumulation Bits Ignored
   ppfd->cAccumBlueBits = 0;                           // Accumulation Bits Ignored
   ppfd->cAccumAlphaBits = 0;                           // Accumulation Bits Ignored
   ppfd->cDepthBits = 24;                          // 16Bit Z-Buffer (Depth Buffer)
   ppfd->cStencilBits = 8;                           // Some Stencil Buffer
   ppfd->cAuxBuffers = 0;                           // No Auxiliary Buffer
   ppfd->iLayerType = PFD_MAIN_PLANE,              // Main Drawing Layer
      ppfd->bReserved = 0;                           // Reserved
   ppfd->dwLayerMask = 0;                           // Layer Masks Ignored
   ppfd->dwVisibleMask = 0;                           // Layer Masks Ignored
   ppfd->dwDamageMask = 0;                           // Layer Masks Ignored
   return 1;
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
   _In_opt_ LPVOID lpParam)
{
   return NULL;
}

BOOL
WINAPI
AdjustWindowRectEx(
   _Inout_ LPRECT lpRect,
   _In_ DWORD dwStyle,
   _In_ BOOL bMenu,
   _In_ DWORD dwExStyle)
{
   return TRUE;
}

BOOL
WINAPI
DestroyWindow(
   _In_ HWND hWnd)
{
   return TRUE;
}
