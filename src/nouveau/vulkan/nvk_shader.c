
#include "nvk_device.h"
#include "nvk_shader.h"
#include "nvk_physical_device.h"
#include "nvk_pipeline_layout.h"
#include "nvk_nir.h"

#include "nouveau_bo.h"
#include "vk_shader_module.h"

#include "nir.h"
#include "compiler/spirv/nir_spirv.h"

#include "nv50_ir_driver.h"

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

static inline enum pipe_shader_type
pipe_shader_type_from_mesa(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return PIPE_SHADER_VERTEX;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_FRAGMENT:
      return PIPE_SHADER_FRAGMENT;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return PIPE_SHADER_COMPUTE;
   default:
      unreachable("bad shader stage");
   }
}

VkResult
nvk_shader_compile_to_nir(struct nvk_device *device,
                          struct vk_shader_module *module,
                          const char *entrypoint_name,
                          gl_shader_stage stage,
                          const VkSpecializationInfo *spec_info,
                          const struct nvk_pipeline_layout *layout,
                          nir_shader **nir_out)
{
   struct nvk_physical_device *pdevice = nvk_device_physical(device);
   const nir_shader_compiler_options *nir_options =
      nv50_ir_nir_shader_compiler_options(pdevice->dev->chipset,
                                          pipe_shader_type_from_mesa(stage));

   const struct spirv_to_nir_options spirv_options = {
      .caps =
      {
      },
      .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
      .ubo_addr_format = nir_address_format_64bit_global_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
   };

   nir_shader *nir;
   VkResult result = vk_shader_module_to_nir(&device->vk, module, stage,
                                             entrypoint_name, spec_info,
                                             &spirv_options, nir_options,
                                             NULL, &nir);
   if (result != VK_SUCCESS)
      return result;

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);

   NIR_PASS(_, nir, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_lower_system_values);

   nir_lower_compute_system_values_options csv_options = {
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   NIR_PASS(_, nir, nvk_nir_lower_descriptors, layout, true);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            spirv_options.ssbo_addr_format);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            spirv_options.ubo_addr_format);

   if (!nir->info.shared_memory_explicit_layout) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, shared_var_info);
   }
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   *nir_out = nir;

   return VK_SUCCESS;
}

VkResult
nvk_compile_nir(struct nvk_physical_device *device, nir_shader *nir,
                struct nvk_shader *shader)
{
   struct nv50_ir_prog_info *info;
   struct nv50_ir_prog_info_out info_out = {};
   int ret;

   info = CALLOC_STRUCT(nv50_ir_prog_info);
   if (!info)
      return false;

   info->type = pipe_shader_type_from_mesa(nir->info.stage);
   info->target = device->dev->chipset;
   info->bin.nir = nir;

   for (unsigned i = 0; i < 3; i++)
      shader->cp.block_size[i] = nir->info.workgroup_size[i];

   info->bin.smemSize = shader->cp.smem_size;
   info->dbgFlags = debug_get_num_option("NV50_PROG_DEBUG", 0);
   info->optLevel = debug_get_num_option("NV50_PROG_OPTIMIZE", 3);
   info->io.auxCBSlot = 15;
   info->io.uboInfoBase = 0;
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      info->io.auxCBSlot = 1;
      info->prop.cp.gridInfoBase = 0;
   }
   ret = nv50_ir_generate_code(info, &info_out);
   if (ret)
      return VK_ERROR_UNKNOWN;

   shader->code_ptr = (uint8_t *)info_out.bin.code;
   shader->code_size = info_out.bin.codeSize;

   if (info_out.target >= NVISA_GV100_CHIPSET)
      shader->num_gprs = MIN2(info_out.bin.maxGPR + 5, 256); //XXX: why?
   else
      shader->num_gprs = MAX2(4, (info_out.bin.maxGPR + 1));
   shader->cp.smem_size = info_out.bin.smemSize;
   shader->num_barriers = info_out.numBarriers;

   if (info_out.bin.tlsSpace) {
      assert(info_out.bin.tlsSpace < (1 << 24));
      shader->hdr[0] |= 1 << 26;
      shader->hdr[1] |= align(info_out.bin.tlsSpace, 0x10); /* l[] size */
      shader->need_tls = true;
   }

   if (info_out.io.globalAccess)
      shader->hdr[0] |= 1 << 26;
   if (info_out.io.globalAccess & 0x2)
      shader->hdr[0] |= 1 << 16;
   if (info_out.io.fp64)
      shader->hdr[0] |= 1 << 27;

   ralloc_free(nir);
   return VK_SUCCESS;
}

void
nvk_shader_upload(struct nvk_physical_device *physical, struct nvk_shader *shader)
{
   void *ptr;
   shader->bo = nouveau_ws_bo_new(physical->dev, shader->code_size, 256,
                                  NOUVEAU_WS_BO_LOCAL | NOUVEAU_WS_BO_MAP);

   ptr = nouveau_ws_bo_map(shader->bo, NOUVEAU_WS_BO_WR);

   memcpy(ptr, shader->code_ptr, shader->code_size);
}
