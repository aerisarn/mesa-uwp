#ifndef NVK_SHADER_H
#define NVK_SHADER_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "nir.h"
#include "nouveau_bo.h"

struct vk_shader_module;
struct nvk_device;
struct nvk_physical_device;

#define GF100_SHADER_HEADER_SIZE (20 * 4)
#define TU102_SHADER_HEADER_SIZE (32 * 4)
#define NVC0_MAX_SHADER_HEADER_SIZE TU102_SHADER_HEADER_SIZE

struct nvk_shader {
   gl_shader_stage stage;

   uint8_t *code_ptr;
   uint32_t code_size;

   uint8_t num_gprs;
   uint8_t num_barriers;
   uint32_t slm_size;

   uint32_t hdr[NVC0_MAX_SHADER_HEADER_SIZE/4];
   uint32_t flags[2];

   struct {
      uint32_t clip_mode; /* clip/cull selection */
      uint8_t clip_enable; /* mask of defined clip planes */
      uint8_t cull_enable; /* mask of defined cull distances */
      uint8_t num_ucps; /* also set to max if ClipDistance is used */
      uint8_t edgeflag; /* attribute index of edgeflag input */
      bool need_vertex_id;
      bool need_draw_parameters;
      bool layer_viewport_relative; /* also applies go gp and tp */
   } vs;

   struct {
      uint8_t early_z;
      uint8_t colors;
      uint8_t color_interp[2];
      bool sample_mask_in;
      bool force_persample_interp;
      bool flatshade;
      bool reads_framebuffer;
      bool post_depth_coverage;
      bool msaa;
   } fs;

   struct {
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
              const struct vk_pipeline_layout *layout);

VkResult
nvk_compile_nir(struct nvk_physical_device *device, nir_shader *nir,
                struct nvk_shader *shader);

void
nvk_shader_upload(struct nvk_device *dev, struct nvk_shader *shader);
#endif
