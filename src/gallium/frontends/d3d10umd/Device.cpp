/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
 */


#include "Draw.h"
#include "Dxgi.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "Format.h"

#include "Debug.h"

#include "util/u_sampler.h"


static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   return sizeof(Device);
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();

   if (0) {
      DebugPrintf("hAdapter = %p\n", hAdapter);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("p10_1DeviceFuncs = %p\n", pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice);
      DebugPrintf("DXGIBaseDDI = %p\n", pCreateData->DXGIBaseDDI);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer);
      DebugPrintf("pUMCallbacks = %p\n", pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __FUNCTION__, pCreateData->Interface);
      return E_FAIL;
   }

   Adapter *pAdapter = CastAdapter(hAdapter);

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);

   struct pipe_screen *screen = pAdapter->screen;
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   pDevice->pipe = pipe;

   pDevice->empty_vs = CreateEmptyShader(pDevice, PIPE_SHADER_VERTEX);
   pDevice->empty_fs = CreateEmptyShader(pDevice, PIPE_SHADER_FRAGMENT);

   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->get_param(screen, PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS);

   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;

   pDevice->draw_so_target = NULL;

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
      pCreateData->pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP =
         ResourceUpdateSubResourceUP;
      pCreateData->pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
      pCreateData->pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
      pCreateData->pDeviceFuncs->pfnPsSetShader = PsSetShader;
      pCreateData->pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
      pCreateData->pDeviceFuncs->pfnVsSetShader = VsSetShader;
      pCreateData->pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
      pCreateData->pDeviceFuncs->pfnDraw = Draw;
      pCreateData->pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite =
         ResourceMap;
      pCreateData->pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
      pCreateData->pDeviceFuncs->pfnDynamicConstantBufferMapDiscard =
         ResourceMap;
      pCreateData->pDeviceFuncs->pfnDynamicIABufferMapDiscard =
         ResourceMap;
      pCreateData->pDeviceFuncs->pfnDynamicConstantBufferUnmap =
         ResourceUnmap;
      pCreateData->pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
      pCreateData->pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
      pCreateData->pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
      pCreateData->pDeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
      pCreateData->pDeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
      pCreateData->pDeviceFuncs->pfnDrawInstanced = DrawInstanced;
      pCreateData->pDeviceFuncs->pfnDynamicResourceMapDiscard =
         ResourceMap;
      pCreateData->pDeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
      pCreateData->pDeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
      pCreateData->pDeviceFuncs->pfnGsSetShader = GsSetShader;
      pCreateData->pDeviceFuncs->pfnIaSetTopology = IaSetTopology;
      pCreateData->pDeviceFuncs->pfnStagingResourceMap = ResourceMap;
      pCreateData->pDeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
      pCreateData->pDeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
      pCreateData->pDeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
      pCreateData->pDeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
      pCreateData->pDeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
      pCreateData->pDeviceFuncs->pfnSetRenderTargets = SetRenderTargets;
      pCreateData->pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard =
         ShaderResourceViewReadAfterWriteHazard;
      pCreateData->pDeviceFuncs->pfnResourceReadAfterWriteHazard =
         ResourceReadAfterWriteHazard;
      pCreateData->pDeviceFuncs->pfnSetBlendState = SetBlendState;
      pCreateData->pDeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
      pCreateData->pDeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
      pCreateData->pDeviceFuncs->pfnQueryEnd = QueryEnd;
      pCreateData->pDeviceFuncs->pfnQueryBegin = QueryBegin;
      pCreateData->pDeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
      pCreateData->pDeviceFuncs->pfnResourceUpdateSubresourceUP =
         ResourceUpdateSubResourceUP;
      pCreateData->pDeviceFuncs->pfnSoSetTargets = SoSetTargets;
      pCreateData->pDeviceFuncs->pfnDrawAuto = DrawAuto;
      pCreateData->pDeviceFuncs->pfnSetViewports = SetViewports;
      pCreateData->pDeviceFuncs->pfnSetScissorRects = SetScissorRects;
      pCreateData->pDeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
      pCreateData->pDeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
      pCreateData->pDeviceFuncs->pfnSetPredication = SetPredication;
      pCreateData->pDeviceFuncs->pfnQueryGetData = QueryGetData;
      pCreateData->pDeviceFuncs->pfnFlush = Flush;
      pCreateData->pDeviceFuncs->pfnGenMips = GenMips;
      pCreateData->pDeviceFuncs->pfnResourceCopy = ResourceCopy;
      pCreateData->pDeviceFuncs->pfnResourceResolveSubresource =
         ResourceResolveSubResource;
      pCreateData->pDeviceFuncs->pfnResourceMap = ResourceMap;
      pCreateData->pDeviceFuncs->pfnResourceUnmap = ResourceUnmap;
      pCreateData->pDeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
      pCreateData->pDeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
      pCreateData->pDeviceFuncs->pfnCalcPrivateResourceSize =
         CalcPrivateResourceSize;
      pCreateData->pDeviceFuncs->pfnCalcPrivateOpenedResourceSize =
         CalcPrivateOpenedResourceSize;
      pCreateData->pDeviceFuncs->pfnCreateResource = CreateResource;
      pCreateData->pDeviceFuncs->pfnOpenResource = OpenResource;
      pCreateData->pDeviceFuncs->pfnDestroyResource = DestroyResource;
      pCreateData->pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize =
         CalcPrivateShaderResourceViewSize;
      pCreateData->pDeviceFuncs->pfnCreateShaderResourceView =
         CreateShaderResourceView;
      pCreateData->pDeviceFuncs->pfnDestroyShaderResourceView =
         DestroyShaderResourceView;
      pCreateData->pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize =
         CalcPrivateRenderTargetViewSize;
      pCreateData->pDeviceFuncs->pfnCreateRenderTargetView =
         CreateRenderTargetView;
      pCreateData->pDeviceFuncs->pfnDestroyRenderTargetView =
         DestroyRenderTargetView;
      pCreateData->pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize =
         CalcPrivateDepthStencilViewSize;
      pCreateData->pDeviceFuncs->pfnCreateDepthStencilView =
         CreateDepthStencilView;
      pCreateData->pDeviceFuncs->pfnDestroyDepthStencilView =
         DestroyDepthStencilView;
      pCreateData->pDeviceFuncs->pfnCalcPrivateElementLayoutSize =
         CalcPrivateElementLayoutSize;
      pCreateData->pDeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
      pCreateData->pDeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
      pCreateData->pDeviceFuncs->pfnCalcPrivateBlendStateSize =
         CalcPrivateBlendStateSize;
      pCreateData->pDeviceFuncs->pfnCreateBlendState = CreateBlendState;
      pCreateData->pDeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
      pCreateData->pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize =
         CalcPrivateDepthStencilStateSize;
      pCreateData->pDeviceFuncs->pfnCreateDepthStencilState =
         CreateDepthStencilState;
      pCreateData->pDeviceFuncs->pfnDestroyDepthStencilState =
         DestroyDepthStencilState;
      pCreateData->pDeviceFuncs->pfnCalcPrivateRasterizerStateSize =
         CalcPrivateRasterizerStateSize;
      pCreateData->pDeviceFuncs->pfnCreateRasterizerState =
         CreateRasterizerState;
      pCreateData->pDeviceFuncs->pfnDestroyRasterizerState =
         DestroyRasterizerState;
      pCreateData->pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
      pCreateData->pDeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
      pCreateData->pDeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
      pCreateData->pDeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
      pCreateData->pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput =
         CalcPrivateGeometryShaderWithStreamOutput;
      pCreateData->pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput =
         CreateGeometryShaderWithStreamOutput;
      pCreateData->pDeviceFuncs->pfnDestroyShader = DestroyShader;
      pCreateData->pDeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
      pCreateData->pDeviceFuncs->pfnCreateSampler = CreateSampler;
      pCreateData->pDeviceFuncs->pfnDestroySampler = DestroySampler;
      pCreateData->pDeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
      pCreateData->pDeviceFuncs->pfnCreateQuery = CreateQuery;
      pCreateData->pDeviceFuncs->pfnDestroyQuery = DestroyQuery;
      pCreateData->pDeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
      pCreateData->pDeviceFuncs->pfnCheckMultisampleQualityLevels =
         CheckMultisampleQualityLevels;
      pCreateData->pDeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
      pCreateData->pDeviceFuncs->pfnCheckCounter = CheckCounter;
      pCreateData->pDeviceFuncs->pfnDestroyDevice = DestroyDevice;
      pCreateData->pDeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;
      break;
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
      pCreateData->p10_1DeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP =
         ResourceUpdateSubResourceUP;
      pCreateData->p10_1DeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
      pCreateData->p10_1DeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
      pCreateData->p10_1DeviceFuncs->pfnPsSetShader = PsSetShader;
      pCreateData->p10_1DeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
      pCreateData->p10_1DeviceFuncs->pfnVsSetShader = VsSetShader;
      pCreateData->p10_1DeviceFuncs->pfnDrawIndexed = DrawIndexed;
      pCreateData->p10_1DeviceFuncs->pfnDraw = Draw;
      pCreateData->p10_1DeviceFuncs->pfnDynamicIABufferMapNoOverwrite =
         ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
      pCreateData->p10_1DeviceFuncs->pfnDynamicConstantBufferMapDiscard =
         ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnDynamicIABufferMapDiscard =
         ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnDynamicConstantBufferUnmap =
         ResourceUnmap;
      pCreateData->p10_1DeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
      pCreateData->p10_1DeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
      pCreateData->p10_1DeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
      pCreateData->p10_1DeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
      pCreateData->p10_1DeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
      pCreateData->p10_1DeviceFuncs->pfnDrawInstanced = DrawInstanced;
      pCreateData->p10_1DeviceFuncs->pfnDynamicResourceMapDiscard =
         ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
      pCreateData->p10_1DeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
      pCreateData->p10_1DeviceFuncs->pfnGsSetShader = GsSetShader;
      pCreateData->p10_1DeviceFuncs->pfnIaSetTopology = IaSetTopology;
      pCreateData->p10_1DeviceFuncs->pfnStagingResourceMap = ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
      pCreateData->p10_1DeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
      pCreateData->p10_1DeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
      pCreateData->p10_1DeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
      pCreateData->p10_1DeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
      pCreateData->p10_1DeviceFuncs->pfnSetRenderTargets = SetRenderTargets;
      pCreateData->p10_1DeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard =
         ShaderResourceViewReadAfterWriteHazard;
      pCreateData->p10_1DeviceFuncs->pfnResourceReadAfterWriteHazard =
         ResourceReadAfterWriteHazard;
      pCreateData->p10_1DeviceFuncs->pfnSetBlendState = SetBlendState;
      pCreateData->p10_1DeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
      pCreateData->p10_1DeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
      pCreateData->p10_1DeviceFuncs->pfnQueryEnd = QueryEnd;
      pCreateData->p10_1DeviceFuncs->pfnQueryBegin = QueryBegin;
      pCreateData->p10_1DeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
      pCreateData->p10_1DeviceFuncs->pfnResourceUpdateSubresourceUP =
         ResourceUpdateSubResourceUP;
      pCreateData->p10_1DeviceFuncs->pfnSoSetTargets = SoSetTargets;
      pCreateData->p10_1DeviceFuncs->pfnDrawAuto = DrawAuto;
      pCreateData->p10_1DeviceFuncs->pfnSetViewports = SetViewports;
      pCreateData->p10_1DeviceFuncs->pfnSetScissorRects = SetScissorRects;
      pCreateData->p10_1DeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
      pCreateData->p10_1DeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
      pCreateData->p10_1DeviceFuncs->pfnSetPredication = SetPredication;
      pCreateData->p10_1DeviceFuncs->pfnQueryGetData = QueryGetData;
      pCreateData->p10_1DeviceFuncs->pfnFlush = Flush;
      pCreateData->p10_1DeviceFuncs->pfnGenMips = GenMips;
      pCreateData->p10_1DeviceFuncs->pfnResourceCopy = ResourceCopy;
      pCreateData->p10_1DeviceFuncs->pfnResourceResolveSubresource =
         ResourceResolveSubResource;
      pCreateData->p10_1DeviceFuncs->pfnResourceMap = ResourceMap;
      pCreateData->p10_1DeviceFuncs->pfnResourceUnmap = ResourceUnmap;
      pCreateData->p10_1DeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
      pCreateData->p10_1DeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs1;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateResourceSize =
         CalcPrivateResourceSize;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateOpenedResourceSize =
         CalcPrivateOpenedResourceSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateResource = CreateResource;
      pCreateData->p10_1DeviceFuncs->pfnOpenResource = OpenResource;
      pCreateData->p10_1DeviceFuncs->pfnDestroyResource = DestroyResource;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize =
         CalcPrivateShaderResourceViewSize1;
      pCreateData->p10_1DeviceFuncs->pfnCreateShaderResourceView =
         CreateShaderResourceView1;
      pCreateData->p10_1DeviceFuncs->pfnDestroyShaderResourceView =
         DestroyShaderResourceView;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateRenderTargetViewSize =
         CalcPrivateRenderTargetViewSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateRenderTargetView =
         CreateRenderTargetView;
      pCreateData->p10_1DeviceFuncs->pfnDestroyRenderTargetView =
         DestroyRenderTargetView;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateDepthStencilViewSize =
         CalcPrivateDepthStencilViewSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateDepthStencilView =
         CreateDepthStencilView;
      pCreateData->p10_1DeviceFuncs->pfnDestroyDepthStencilView =
         DestroyDepthStencilView;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateElementLayoutSize =
         CalcPrivateElementLayoutSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
      pCreateData->p10_1DeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize =
         CalcPrivateBlendStateSize1;
      pCreateData->p10_1DeviceFuncs->pfnCreateBlendState = CreateBlendState1;
      pCreateData->p10_1DeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateDepthStencilStateSize =
         CalcPrivateDepthStencilStateSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateDepthStencilState =
         CreateDepthStencilState;
      pCreateData->p10_1DeviceFuncs->pfnDestroyDepthStencilState =
         DestroyDepthStencilState;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateRasterizerStateSize =
         CalcPrivateRasterizerStateSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateRasterizerState =
         CreateRasterizerState;
      pCreateData->p10_1DeviceFuncs->pfnDestroyRasterizerState =
         DestroyRasterizerState;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
      pCreateData->p10_1DeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
      pCreateData->p10_1DeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput =
         CalcPrivateGeometryShaderWithStreamOutput;
      pCreateData->p10_1DeviceFuncs->pfnCreateGeometryShaderWithStreamOutput =
         CreateGeometryShaderWithStreamOutput;
      pCreateData->p10_1DeviceFuncs->pfnDestroyShader = DestroyShader;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
      pCreateData->p10_1DeviceFuncs->pfnCreateSampler = CreateSampler;
      pCreateData->p10_1DeviceFuncs->pfnDestroySampler = DestroySampler;
      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
      pCreateData->p10_1DeviceFuncs->pfnCreateQuery = CreateQuery;
      pCreateData->p10_1DeviceFuncs->pfnDestroyQuery = DestroyQuery;
      pCreateData->p10_1DeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
      pCreateData->p10_1DeviceFuncs->pfnCheckMultisampleQualityLevels =
         CheckMultisampleQualityLevels;
      pCreateData->p10_1DeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
      pCreateData->p10_1DeviceFuncs->pfnCheckCounter = CheckCounter;
      pCreateData->p10_1DeviceFuncs->pfnDestroyDevice = DestroyDevice;
      pCreateData->p10_1DeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;

      pCreateData->p10_1DeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs1;

      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize =
         CalcPrivateShaderResourceViewSize1;
      pCreateData->p10_1DeviceFuncs->pfnCreateShaderResourceView =
         CreateShaderResourceView1;

      pCreateData->p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize =
         CalcPrivateBlendStateSize1;
      pCreateData->p10_1DeviceFuncs->pfnCreateBlendState = CreateBlendState1;

      pCreateData->p10_1DeviceFuncs->pfnResourceConvert =
         pCreateData->p10_1DeviceFuncs->pfnResourceCopy;
      pCreateData->p10_1DeviceFuncs->pfnResourceConvertRegion =
         pCreateData->p10_1DeviceFuncs->pfnResourceCopyRegion;

      break;
   default:
      assert(0);
      break;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      _Present;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      _GetGammaCaps;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      _SetDisplayMode;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      _SetResourcePriority;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      _QueryResourceResidency;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      _RotateResourceIdentities;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      _Blt;

   if (0) {
      return S_OK;
   } else {
      // Tell DXGI to not use the shared resource presentation path when
      // communicating with DWM:
      // http://msdn.microsoft.com/en-us/library/windows/hardware/ff569887(v=vs.85).aspx
      return DXGI_STATUS_NO_REDIRECTION;
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   pipe->flush(pipe, NULL, 0);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);

   DeleteEmptyShader(pDevice, PIPE_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, PIPE_SHADER_VERTEX, pDevice->empty_vs);

   pipe_surface_reference(&pDevice->fb.zsbuf, NULL);
   for (i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
      pipe_surface_reference(&pDevice->fb.cbufs[i], NULL);
   }

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   memset(sampler_views, 0, sizeof sampler_views);
   pipe->set_sampler_views(pipe, PIPE_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, PIPE_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, PIPE_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

   pipe->destroy(pipe);
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);

   pipe->flush(pipe, NULL, 0);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   //LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, FALSE);
   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      }
#endif
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
#endif
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   /* XXX: Disable MSAA */
   *pNumQualityLevels = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}
