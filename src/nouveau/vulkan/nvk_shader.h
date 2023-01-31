#ifndef NVK_SHADER_H
#define NVK_SHADER_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "nir.h"
#include "nouveau_bo.h"

struct vk_shader_module;
struct nvk_device;
struct nvk_pipeline_layout;
struct nvk_physical_device;

#define GF100_SHADER_HEADER_SIZE (20 * 4)
#define TU102_SHADER_HEADER_SIZE (32 * 4)
#define NVC0_MAX_SHADER_HEADER_SIZE TU102_SHADER_HEADER_SIZE

struct nvk_shader {
   uint8_t *code_ptr;
   uint32_t code_size;

   uint32_t hdr[NVC0_MAX_SHADER_HEADER_SIZE/4];
   bool need_tls;
   uint8_t num_gprs;
   uint8_t num_barriers;
   struct {
      uint32_t lmem_size; /* local memory (TGSI PRIVATE resource) size */
      uint32_t smem_size; /* shared memory (TGSI LOCAL resource) size */
      uint32_t block_size[3];
   } cp;

   struct nouveau_ws_bo *bo;
};

static inline uint64_t
nvk_shader_address(const struct nvk_shader *shader)
{
   return shader->bo->offset;
}

const nir_shader_compiler_options *
nvk_physical_device_nir_options(const struct nvk_physical_device *pdevice,
                                gl_shader_stage stage);

const struct spirv_to_nir_options *
nvk_physical_device_spirv_options(const struct nvk_physical_device *pdevice);

void
nvk_lower_nir(struct nvk_device *device, nir_shader *nir,
              const struct nvk_pipeline_layout *layout);

VkResult
nvk_compile_nir(struct nvk_physical_device *device, nir_shader *nir,
                struct nvk_shader *shader);

void
nvk_shader_upload(struct nvk_physical_device *physical, struct nvk_shader *shader);
#endif
