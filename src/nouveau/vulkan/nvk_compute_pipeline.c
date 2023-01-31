#include "nvk_private.h"
#include "nvk_device.h"
#include "nvk_pipeline.h"
#include "nvk_shader.h"
#include "vk_nir.h"
#include "vk_pipeline.h"

#include "nouveau_bo.h"

#include "compiler/spirv/nir_spirv.h"

#include "drf.h"
#include "cla0c0qmd.h"
#include "clc0c0qmd.h"
#include "clc3c0qmd.h"
#define NVA0C0_QMDV00_06_VAL_SET(p,a...) NVVAL_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVA0C0_QMDV00_06_DEF_SET(p,a...) NVDEF_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVC0C0_QMDV02_01_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC0C0_QMDV02_01_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC3C0_QMDV02_02_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC3C0, QMDV02_02, ##a)
#define NVC3C0_QMDV02_02_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC3C0, QMDV02_02, ##a)

static int
gv100_sm_config_smem_size(uint32_t size)
{
   if      (size > 64 * 1024) size = 96 * 1024;
   else if (size > 32 * 1024) size = 64 * 1024;
   else if (size > 16 * 1024) size = 32 * 1024;
   else if (size >  8 * 1024) size = 16 * 1024;
   else                       size =  8 * 1024;
   return (size / 4096) + 1;
}

static void
gv100_compute_setup_launch_desc_template(uint32_t *qmd,
                                         struct nvk_shader *shader)
{
   NVC3C0_QMDV02_02_VAL_SET(qmd, SM_GLOBAL_CACHING_ENABLE, 1);
   NVC3C0_QMDV02_02_DEF_SET(qmd, API_VISIBLE_CALL_LIMIT, NO_CHECK);
   NVC3C0_QMDV02_02_DEF_SET(qmd, SAMPLER_INDEX, INDEPENDENTLY);
   NVC3C0_QMDV02_02_VAL_SET(qmd, SHARED_MEMORY_SIZE,
                            align(shader->cp.smem_size, 0x100));
   NVC3C0_QMDV02_02_VAL_SET(qmd, SHADER_LOCAL_MEMORY_LOW_SIZE,
                            align(shader->slm_size, 0x10));
   NVC3C0_QMDV02_02_VAL_SET(qmd, SHADER_LOCAL_MEMORY_HIGH_SIZE, 0);
   NVC3C0_QMDV02_02_VAL_SET(qmd, MIN_SM_CONFIG_SHARED_MEM_SIZE,
                            gv100_sm_config_smem_size(8 * 1024));
   NVC3C0_QMDV02_02_VAL_SET(qmd, MAX_SM_CONFIG_SHARED_MEM_SIZE,
                            gv100_sm_config_smem_size(96 * 1024));
   NVC3C0_QMDV02_02_VAL_SET(qmd, QMD_VERSION, 2);
   NVC3C0_QMDV02_02_VAL_SET(qmd, QMD_MAJOR_VERSION, 2);
   NVC3C0_QMDV02_02_VAL_SET(qmd, TARGET_SM_CONFIG_SHARED_MEM_SIZE,
                            gv100_sm_config_smem_size(shader->cp.smem_size));

   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_THREAD_DIMENSION0, shader->cp.block_size[0]);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_THREAD_DIMENSION1, shader->cp.block_size[1]);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_THREAD_DIMENSION2, shader->cp.block_size[2]);
   NVC3C0_QMDV02_02_VAL_SET(qmd, REGISTER_COUNT_V, shader->num_gprs);
   NVC3C0_QMDV02_02_VAL_SET(qmd, BARRIER_COUNT, shader->num_barriers);

   uint64_t addr = nvk_shader_address(shader);
   NVC3C0_QMDV02_02_VAL_SET(qmd, PROGRAM_ADDRESS_LOWER, addr & 0xffffffff);
   NVC3C0_QMDV02_02_VAL_SET(qmd, PROGRAM_ADDRESS_UPPER, addr >> 32);

}

VkResult
nvk_compute_pipeline_create(struct nvk_device *device,
                            struct vk_pipeline_cache *cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipeline)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   struct nvk_physical_device *pdevice = nvk_device_physical(device);
   struct nvk_compute_pipeline *pipeline;
   VkResult result;

   pipeline = (void *)nvk_pipeline_zalloc(device, NVK_PIPELINE_COMPUTE,
                                          sizeof(*pipeline), pAllocator);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(pCreateInfo->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);

   struct vk_pipeline_robustness_state robustness;
   vk_pipeline_robustness_state_fill(&device->vk, &robustness,
                                     pCreateInfo->pNext,
                                     pCreateInfo->stage.pNext);

   const nir_shader_compiler_options *nir_options =
      nvk_physical_device_nir_options(pdevice, MESA_SHADER_COMPUTE);
   const struct spirv_to_nir_options spirv_options =
      nvk_physical_device_spirv_options(pdevice, &robustness);

   nir_shader *nir;
   result = vk_pipeline_shader_stage_to_nir(&device->vk,
                                            &pCreateInfo->stage,
                                            &spirv_options, nir_options,
                                            NULL, &nir);
   if (result != VK_SUCCESS)
      goto fail;

   nvk_lower_nir(device, nir, &robustness, pipeline_layout);

   result = nvk_compile_nir(pdevice, nir, &pipeline->base.shaders[MESA_SHADER_COMPUTE]);
   ralloc_free(nir);
   if (result != VK_SUCCESS)
      goto fail;

   result = nvk_shader_upload(device,
                              &pipeline->base.shaders[MESA_SHADER_COMPUTE]);
   if (result != VK_SUCCESS)
      goto fail;

   gv100_compute_setup_launch_desc_template(pipeline->qmd_template, &pipeline->base.shaders[MESA_SHADER_COMPUTE]);
   *pPipeline = nvk_pipeline_to_handle(&pipeline->base);
   return VK_SUCCESS;

fail:
   nvk_pipeline_free(device, &pipeline->base, pAllocator);
   return result;
}
