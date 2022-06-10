#ifndef NVK_SHADER_H
#define NVK_SHADER_H 1

#include "nir.h"

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

VkResult
nvk_shader_compile_to_nir(struct nvk_device *device,
                          struct vk_shader_module *module,
                          const char *entrypoint_name,
                          gl_shader_stage stage,
                          const VkSpecializationInfo *spec_info,
                          const struct nvk_pipeline_layout *layout,
                          nir_shader **nir_out);

VkResult
nvk_compile_nir(struct nvk_physical_device *device, nir_shader *nir,
                struct nvk_shader *shader);

void
nvk_shader_upload(struct nvk_physical_device *physical, struct nvk_shader *shader);
#endif
