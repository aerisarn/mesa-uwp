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

#include "dzn_private.h"

#define D3D12_IGNORE_SDK_LAYERS
#define COBJMACROS
#include <directx/d3d12.h>

#include <vulkan/vulkan.h>

#include "util/format/u_format.h"
#include "util/log.h"

#include <directx/d3d12sdklayers.h>
#include <dxgi1_4.h>

static const DXGI_FORMAT formats[PIPE_FORMAT_COUNT] = {
#define MAP_FORMAT_NORM(FMT) \
   [PIPE_FORMAT_ ## FMT ## _UNORM] = DXGI_FORMAT_ ## FMT ## _UNORM, \
   [PIPE_FORMAT_ ## FMT ## _SNORM] = DXGI_FORMAT_ ## FMT ## _SNORM,

#define MAP_FORMAT_INT(FMT) \
   [PIPE_FORMAT_ ## FMT ## _UINT] = DXGI_FORMAT_ ## FMT ## _UINT, \
   [PIPE_FORMAT_ ## FMT ## _SINT] = DXGI_FORMAT_ ## FMT ## _SINT,

#define MAP_FORMAT_SRGB(FMT) \
   [PIPE_FORMAT_ ## FMT ## _SRGB] = DXGI_FORMAT_ ## FMT ## _UNORM_SRGB,

#define MAP_FORMAT_FLOAT(FMT) \
   [PIPE_FORMAT_ ## FMT ## _FLOAT] = DXGI_FORMAT_ ## FMT ## _FLOAT,

#define MAP_EMU_FORMAT_NO_ALPHA(BITS, TYPE) \
   [PIPE_FORMAT_L ## BITS ## _ ## TYPE] = DXGI_FORMAT_R ## BITS ## _ ## TYPE, \
   [PIPE_FORMAT_I ## BITS ## _ ## TYPE] = DXGI_FORMAT_R ## BITS ## _ ## TYPE, \
   [PIPE_FORMAT_L ## BITS ## A ## BITS ## _ ## TYPE] = \
          DXGI_FORMAT_R ## BITS ## G ## BITS ## _ ## TYPE,

#define MAP_EMU_FORMAT(BITS, TYPE) \
   [PIPE_FORMAT_A ## BITS ## _ ## TYPE] = DXGI_FORMAT_R ## BITS ## _ ## TYPE, \
   MAP_EMU_FORMAT_NO_ALPHA(BITS, TYPE)

   MAP_FORMAT_NORM(R8)
   MAP_FORMAT_INT(R8)

   MAP_FORMAT_NORM(R8G8)
   MAP_FORMAT_INT(R8G8)

   MAP_FORMAT_NORM(R8G8B8A8)
   MAP_FORMAT_INT(R8G8B8A8)
   MAP_FORMAT_SRGB(R8G8B8A8)

   [PIPE_FORMAT_B8G8R8X8_UNORM] = DXGI_FORMAT_B8G8R8X8_UNORM,
   [PIPE_FORMAT_B8G8R8A8_UNORM] = DXGI_FORMAT_B8G8R8A8_UNORM,
   [PIPE_FORMAT_B4G4R4A4_UNORM] = DXGI_FORMAT_B4G4R4A4_UNORM,
   [PIPE_FORMAT_A4R4G4B4_UNORM] = DXGI_FORMAT_B4G4R4A4_UNORM,
   [PIPE_FORMAT_B5G6R5_UNORM] = DXGI_FORMAT_B5G6R5_UNORM,
   [PIPE_FORMAT_B5G5R5A1_UNORM] = DXGI_FORMAT_B5G5R5A1_UNORM,

   MAP_FORMAT_SRGB(B8G8R8A8)

   MAP_FORMAT_INT(R32)
   MAP_FORMAT_FLOAT(R32)
   MAP_FORMAT_INT(R32G32)
   MAP_FORMAT_FLOAT(R32G32)
   MAP_FORMAT_INT(R32G32B32)
   MAP_FORMAT_FLOAT(R32G32B32)
   MAP_FORMAT_INT(R32G32B32A32)
   MAP_FORMAT_FLOAT(R32G32B32A32)

   MAP_FORMAT_NORM(R16)
   MAP_FORMAT_INT(R16)
   MAP_FORMAT_FLOAT(R16)

   MAP_FORMAT_NORM(R16G16)
   MAP_FORMAT_INT(R16G16)
   MAP_FORMAT_FLOAT(R16G16)

   MAP_FORMAT_NORM(R16G16B16A16)
   MAP_FORMAT_INT(R16G16B16A16)
   MAP_FORMAT_FLOAT(R16G16B16A16)

   [PIPE_FORMAT_A8_UNORM] = DXGI_FORMAT_A8_UNORM,
   MAP_EMU_FORMAT_NO_ALPHA(8, UNORM)
   MAP_EMU_FORMAT(8, SNORM)
   MAP_EMU_FORMAT(8, SINT)
   MAP_EMU_FORMAT(8, UINT)
   MAP_EMU_FORMAT(16, UNORM)
   MAP_EMU_FORMAT(16, SNORM)
   MAP_EMU_FORMAT(16, SINT)
   MAP_EMU_FORMAT(16, UINT)
   MAP_EMU_FORMAT(16, FLOAT)
   MAP_EMU_FORMAT(32, SINT)
   MAP_EMU_FORMAT(32, UINT)
   MAP_EMU_FORMAT(32, FLOAT)

   [PIPE_FORMAT_R9G9B9E5_FLOAT] = DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
   [PIPE_FORMAT_R11G11B10_FLOAT] = DXGI_FORMAT_R11G11B10_FLOAT,
   [PIPE_FORMAT_R10G10B10A2_UINT] = DXGI_FORMAT_R10G10B10A2_UINT,
   [PIPE_FORMAT_R10G10B10A2_UNORM] = DXGI_FORMAT_R10G10B10A2_UNORM,

   [PIPE_FORMAT_DXT1_RGB] = DXGI_FORMAT_BC1_UNORM,
   [PIPE_FORMAT_DXT1_RGBA] = DXGI_FORMAT_BC1_UNORM,
   [PIPE_FORMAT_DXT3_RGBA] = DXGI_FORMAT_BC2_UNORM,
   [PIPE_FORMAT_DXT5_RGBA] = DXGI_FORMAT_BC3_UNORM,

   [PIPE_FORMAT_DXT1_SRGB] = DXGI_FORMAT_BC1_UNORM_SRGB,
   [PIPE_FORMAT_DXT1_SRGBA] = DXGI_FORMAT_BC1_UNORM_SRGB,
   [PIPE_FORMAT_DXT3_SRGBA] = DXGI_FORMAT_BC2_UNORM_SRGB,
   [PIPE_FORMAT_DXT5_SRGBA] = DXGI_FORMAT_BC3_UNORM_SRGB,

   [PIPE_FORMAT_RGTC1_UNORM] = DXGI_FORMAT_BC4_UNORM,
   [PIPE_FORMAT_RGTC1_SNORM] = DXGI_FORMAT_BC4_SNORM,
   [PIPE_FORMAT_RGTC2_UNORM] = DXGI_FORMAT_BC5_UNORM,
   [PIPE_FORMAT_RGTC2_SNORM] = DXGI_FORMAT_BC5_SNORM,

   [PIPE_FORMAT_BPTC_RGB_UFLOAT] = DXGI_FORMAT_BC6H_UF16,
   [PIPE_FORMAT_BPTC_RGB_FLOAT] = DXGI_FORMAT_BC6H_SF16,
   [PIPE_FORMAT_BPTC_RGBA_UNORM] = DXGI_FORMAT_BC7_UNORM,
   [PIPE_FORMAT_BPTC_SRGBA] = DXGI_FORMAT_BC7_UNORM_SRGB,

   [PIPE_FORMAT_Z32_FLOAT] = DXGI_FORMAT_R32_TYPELESS,
   [PIPE_FORMAT_Z16_UNORM] = DXGI_FORMAT_R16_TYPELESS,
   [PIPE_FORMAT_Z24X8_UNORM] = DXGI_FORMAT_R24G8_TYPELESS,
   [PIPE_FORMAT_X24S8_UINT] = DXGI_FORMAT_R24G8_TYPELESS,

   [PIPE_FORMAT_Z24_UNORM_S8_UINT] = DXGI_FORMAT_R24G8_TYPELESS,
   [PIPE_FORMAT_Z32_FLOAT_S8X24_UINT] = DXGI_FORMAT_R32G8X24_TYPELESS,
   [PIPE_FORMAT_X32_S8X24_UINT] = DXGI_FORMAT_R32G8X24_TYPELESS,
};

DXGI_FORMAT
dzn_pipe_to_dxgi_format(enum pipe_format in)
{
   return formats[in];
}

struct dzn_sampler_filter_info {
   VkFilter min, mag;
   VkSamplerMipmapMode mipmap;
};

#define FILTER(__min, __mag, __mipmap) \
{ \
   .min = VK_FILTER_ ## __min, \
   .mag = VK_FILTER_ ## __mag, \
   .mipmap = VK_SAMPLER_MIPMAP_MODE_ ## __mipmap, \
}

static const struct dzn_sampler_filter_info filter_table[] = {
   [D3D12_FILTER_MIN_MAG_MIP_POINT] = FILTER(NEAREST, NEAREST, NEAREST),
   [D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR] = FILTER(NEAREST, NEAREST, LINEAR),
   [D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT] = FILTER(NEAREST, LINEAR, NEAREST),
   [D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR] = FILTER(NEAREST, LINEAR, LINEAR),
   [D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT] = FILTER(LINEAR, NEAREST, NEAREST),
   [D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR] = FILTER(LINEAR, NEAREST, LINEAR),
   [D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT] = FILTER(LINEAR, LINEAR, NEAREST),
   [D3D12_FILTER_MIN_MAG_MIP_LINEAR] = FILTER(LINEAR, LINEAR, LINEAR),
};

D3D12_FILTER
dzn_translate_sampler_filter(const VkSamplerCreateInfo *create_info)
{
   D3D12_FILTER filter;

   if (!create_info->anisotropyEnable) {
      unsigned i;
      for (i = 0; i < ARRAY_SIZE(filter_table); i++) {
         if (create_info->minFilter == filter_table[i].min &&
             create_info->magFilter == filter_table[i].mag &&
             create_info->mipmapMode == filter_table[i].mipmap) {
            filter = (D3D12_FILTER)i;
            break;
         }
      }

      assert(i < ARRAY_SIZE(filter_table));
   } else {
      filter = D3D12_FILTER_ANISOTROPIC;
   }

   if (create_info->compareEnable)
      filter = (D3D12_FILTER)(filter + D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT);

   return filter;
}

D3D12_COMPARISON_FUNC
dzn_translate_compare_op(VkCompareOp in)
{
   switch (in) {
   case VK_COMPARE_OP_NEVER: return D3D12_COMPARISON_FUNC_NEVER;
   case VK_COMPARE_OP_LESS: return D3D12_COMPARISON_FUNC_LESS;
   case VK_COMPARE_OP_EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
   case VK_COMPARE_OP_GREATER: return D3D12_COMPARISON_FUNC_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
   case VK_COMPARE_OP_ALWAYS: return D3D12_COMPARISON_FUNC_ALWAYS;
   default: unreachable("Invalid compare op");
   }
}

void
dzn_translate_viewport(D3D12_VIEWPORT *out,
                       const VkViewport *in)
{
   out->TopLeftX = in->x;
   out->TopLeftY = in->height < 0 ? in->height + in->y : in->y;
   out->Width = in->width;
   out->Height = abs(in->height);
   out->MinDepth = MIN2(in->minDepth, in->maxDepth);
   out->MaxDepth = MAX2(in->maxDepth, in->minDepth);
}

void
dzn_translate_rect(D3D12_RECT *out,
                   const VkRect2D *in)
{
   out->left = in->offset.x;
   out->top = in->offset.y;
   out->right = in->offset.x + in->extent.width;
   out->bottom = in->offset.y + in->extent.height;
}

IDXGIFactory4 *
dxgi_get_factory(bool debug)
{
   static const GUID IID_IDXGIFactory4 = {
      0x1bc6ea02, 0xef36, 0x464f,
      { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a }
   };

   HMODULE dxgi_mod = LoadLibraryA("DXGI.DLL");
   if (!dxgi_mod) {
      mesa_loge("failed to load DXGI.DLL\n");
      return NULL;
   }

   typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT flags, REFIID riid, void **ppFactory);
   PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2;

   CreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(dxgi_mod, "CreateDXGIFactory2");
   if (!CreateDXGIFactory2) {
      mesa_loge("failed to load CreateDXGIFactory2 from DXGI.DLL\n");
      return NULL;
   }

   UINT flags = 0;
   if (debug)
      flags |= DXGI_CREATE_FACTORY_DEBUG;

   IDXGIFactory4 *factory;
   HRESULT hr = CreateDXGIFactory2(flags, &IID_IDXGIFactory4, (void **)&factory);
   if (FAILED(hr)) {
      mesa_loge("CreateDXGIFactory2 failed: %08x\n", hr);
      return NULL;
   }

   return factory;
}

static ID3D12Debug *
get_debug_interface()
{
   typedef HRESULT(WINAPI *PFN_D3D12_GET_DEBUG_INTERFACE)(REFIID riid, void **ppFactory);
   PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterface;

   HMODULE d3d12_mod = LoadLibraryA("D3D12.DLL");
   if (!d3d12_mod) {
      mesa_loge("failed to load D3D12.DLL\n");
      return NULL;
   }

   D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(d3d12_mod, "D3D12GetDebugInterface");
   if (!D3D12GetDebugInterface) {
      mesa_loge("failed to load D3D12GetDebugInterface from D3D12.DLL\n");
      return NULL;
   }

   ID3D12Debug *debug;
   if (FAILED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug))) {
      mesa_loge("D3D12GetDebugInterface failed\n");
      return NULL;
   }

   return debug;
}

void
d3d12_enable_debug_layer(void)
{
   ID3D12Debug *debug = get_debug_interface();
   if (debug) {
      ID3D12Debug_EnableDebugLayer(debug);
      ID3D12Debug_Release(debug);
   }
}

void
d3d12_enable_gpu_validation(void)
{
   ID3D12Debug *debug = get_debug_interface();
   if (debug) {
      ID3D12Debug3 *debug3;
      if (SUCCEEDED(ID3D12Debug_QueryInterface(debug,
                                               &IID_ID3D12Debug3,
                                               (void **)&debug3))) {
         ID3D12Debug3_SetEnableGPUBasedValidation(debug3, true);
         ID3D12Debug3_Release(debug3);
      }
      ID3D12Debug_Release(debug);
   }
}

ID3D12Device2 *
d3d12_create_device(IDXGIAdapter1 *adapter, bool experimental_features)
{
   typedef HRESULT(WINAPI *PFN_D3D12CREATEDEVICE)(IDXGIAdapter1 *, D3D_FEATURE_LEVEL, REFIID, void **);
   PFN_D3D12CREATEDEVICE D3D12CreateDevice;

   HMODULE d3d12_mod = LoadLibraryA("D3D12.DLL");
   if (!d3d12_mod) {
      mesa_loge("failed to load D3D12.DLL\n");
      return NULL;
   }

#ifdef _WIN32
   if (experimental_features)
#endif
   {
      typedef HRESULT(WINAPI *PFN_D3D12ENABLEEXPERIMENTALFEATURES)(UINT, const IID *, void *, UINT *);
      PFN_D3D12ENABLEEXPERIMENTALFEATURES D3D12EnableExperimentalFeatures =
         (PFN_D3D12ENABLEEXPERIMENTALFEATURES)GetProcAddress(d3d12_mod, "D3D12EnableExperimentalFeatures");
      if (FAILED(D3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, NULL, NULL))) {
         mesa_loge("failed to enable experimental shader models\n");
         return NULL;
      }
   }

   D3D12CreateDevice = (PFN_D3D12CREATEDEVICE)GetProcAddress(d3d12_mod, "D3D12CreateDevice");
   if (!D3D12CreateDevice) {
      mesa_loge("failed to load D3D12CreateDevice from D3D12.DLL\n");
      return NULL;
   }

   ID3D12Device2 *dev;
   if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                 &IID_ID3D12Device2,
                 (void **)&dev)))
      return dev;

   mesa_loge("D3D12CreateDevice failed\n");
   return NULL;
}

PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE
d3d12_get_serialize_root_sig(void)
{
   HMODULE d3d12_mod = LoadLibraryA("d3d12.dll");
   if (!d3d12_mod) {
      mesa_loge("failed to load d3d12.dll\n");
      return NULL;
   }

   return (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)
      GetProcAddress(d3d12_mod, "D3D12SerializeVersionedRootSignature");
}
