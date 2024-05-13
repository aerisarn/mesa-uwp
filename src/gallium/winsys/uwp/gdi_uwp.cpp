#include "gdi_uwp.h"

#include "..\..\frontends\wgl\stw_pixelformat.h"
#include "..\..\frontends/wgl/stw_winsys.h"

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

static int iPixelFormat = 0;

inline float ConvertDipsToPixels(float dips, float dpi)
{
   static const float dipsPerInch = 96.0f;
   return floorf(dips * dpi / dipsPerInch + 0.5f); // Arrotonda all'intero più vicino.
}

   bool is_running_on_xbox(void)
   {
      Platform::String^ device_family = Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily;
      return (device_family == L"Windows.Xbox");
   }

   int current_height = -1;

   int uwp_get_height(void)
   {
      if (current_height != -1)
         return current_height;

      /* This function must be performed within UI thread,
       * otherwise it will cause a crash in specific cases
       * https://github.com/libretro/RetroArch/issues/13491 */
      float surface_scale    = 0;
      int ret                = -1;
      volatile bool finished = false;
      Windows::ApplicationModel::Core::CoreApplication::MainView->CoreWindow->Dispatcher->RunAsync(
            CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([&surface_scale, &ret, &finished]()
               {
               if (is_running_on_xbox())
               {
               const Windows::Graphics::Display::Core::HdmiDisplayInformation^ hdi = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
               if (hdi)
               ret = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView()->GetCurrentDisplayMode()->ResolutionHeightInRawPixels;
               }

               if (ret == -1)
               {
               const LONG32 resolution_scale = static_cast<LONG32>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->ResolutionScale);
               surface_scale                 = static_cast<float>(resolution_scale) / 100.0f;
               ret                           = static_cast<LONG32>(
                     CoreWindow::GetForCurrentThread()->Bounds.Height 
                     * surface_scale);
               }
               finished = true;
               }));
      Windows::UI::Core::CoreWindow^ corewindow = Windows::UI::Core::CoreWindow::GetForCurrentThread();
      while (!finished)
      {
         if (corewindow)
            corewindow->Dispatcher->ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
      }
      current_height = ret;
      return ret;
   }

   int current_width = -1;

   int uwp_get_width(void)
   {
      if (current_width != -1)
         return current_width;

      /* This function must be performed within UI thread,
       * otherwise it will cause a crash in specific cases
       * https://github.com/libretro/RetroArch/issues/13491 */
      float surface_scale    = 0;
      int returnValue        = -1;
      volatile bool finished = false;
      Windows::ApplicationModel::Core::CoreApplication::MainView->CoreWindow->Dispatcher->RunAsync(
            CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([&surface_scale, &returnValue, &finished]()
               {
               if (is_running_on_xbox())
               {
               const Windows::Graphics::Display::Core::HdmiDisplayInformation^ hdi = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
               if (hdi)
               returnValue = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView()->GetCurrentDisplayMode()->ResolutionWidthInRawPixels;
               }

               if(returnValue == -1)
               {
               const LONG32 resolution_scale = static_cast<LONG32>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->ResolutionScale);
               surface_scale = static_cast<float>(resolution_scale) / 100.0f;
               returnValue   = static_cast<LONG32>(
                     CoreWindow::GetForCurrentThread()->Bounds.Width 
                     * surface_scale);
               }
               finished = true;
               }));
      Windows::UI::Core::CoreWindow^ corewindow = Windows::UI::Core::CoreWindow::GetForCurrentThread();
      while (!finished)
      {
         if (corewindow)
            corewindow->Dispatcher->ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
      }

      current_width = returnValue;
      return returnValue;
   }

BOOL WINAPI GetClientRect( _In_ HWND hWnd, _Out_ LPRECT lpRect)
{
   if (nullptr != lpRect)
   {
      CoreWindow^ coreWindow = CoreWindow::GetForCurrentThread();
      DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();
      lpRect->top = 0;
      lpRect->bottom = uwp_get_height();
      lpRect->left = 0;
      lpRect->right = uwp_get_width();
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
   iPixelFormat = 0;
   return 1;
}

int WINAPI GetPixelFormat(HDC hdc)
{
   if (iPixelFormat == 0)
   {
      int count = stw_pixelformat_get_count(hdc);
      if (count <= 0)
      {
         return 0;
      }
      for (iPixelFormat = 1; iPixelFormat <= count; ++iPixelFormat)
      {
         const struct stw_pixelformat_info * info = stw_pixelformat_get_info(iPixelFormat);

         //Initialize to DXGI_FORMAT_B8G8R8A8_UNORM
         if (info && 
            info->stvis.color_format == PIPE_FORMAT_B8G8R8A8_UNORM &&
            info->pfd.dwFlags & PFD_DOUBLEBUFFER)
         {
            break;
         }
      }
   }
   return iPixelFormat;
}

int DescribePixelFormat(
   HDC                     hdc,
   int                     iPixelFormat,
   UINT                    nBytes,
   LPPIXELFORMATDESCRIPTOR ppfd
)
{

   if (iPixelFormat > 0)
   {
      const struct stw_pixelformat_info * info = stw_pixelformat_get_info(iPixelFormat);
      if (info && ppfd)
      {
         *ppfd = info->pfd;
         return TRUE;
      }
   }
   return FALSE;
}

BOOL SetPixelFormat(
   HDC                         hdc,
   int                         format,
   const PIXELFORMATDESCRIPTOR* ppfd
)
{
   //hmmm, do we have to replace pfd?
   iPixelFormat = format;
   return TRUE;
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
