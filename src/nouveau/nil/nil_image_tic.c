#include "nil_image.h"

#include "nil_format.h"
#include "util/bitpack_helpers.h"

#include "clb097tex.h"
#include "drf.h"

ALWAYS_INLINE static void
__set_u32(uint32_t *o, uint32_t v, unsigned lo, unsigned hi)
{
   assert(lo <= hi && (lo / 32) == (hi / 32));
   o[lo / 32] |= util_bitpack_uint(v, lo % 32, hi % 32);
}

ALWAYS_INLINE static void
__set_i32(uint32_t *o, int32_t v, unsigned lo, unsigned hi)
{
   assert(lo <= hi && (lo / 32) == (hi / 32));
   o[lo / 32] |= util_bitpack_sint(v, lo % 32, hi % 32);
}

ALWAYS_INLINE static void
__set_bool(uint32_t *o, bool b, unsigned lo, unsigned hi)
{
   assert(lo == hi);
   o[lo / 32] |= util_bitpack_uint(b, lo % 32, hi % 32);
}

#define MW(x) x

#define TH_SET_U(o, NV, VER, FIELD, val) \
   __set_u32((o), (val), DRF_LO(NV##_TEXHEAD_##VER##_##FIELD),\
                         DRF_HI(NV##_TEXHEAD_##VER##_##FIELD))

#define TH_SET_I(o, NV, VER, FIELD, val) \
   __set_i32((o), (val), DRF_LO(NV##_TEXHEAD_##VER##_##FIELD),\
                         DRF_HI(NV##_TEXHEAD_##VER##_##FIELD))

#define TH_SET_B(o, NV, VER, FIELD, b) \
   __set_bool((o), (b), DRF_LO(NV##_TEXHEAD_##VER##_##FIELD),\
                        DRF_HI(NV##_TEXHEAD_##VER##_##FIELD))

#define TH_SET_E(o, NV, VER, FIELD, E) \
   TH_SET_U((o), NV, VER, FIELD, NV##_TEXHEAD_##VER##_##FIELD##_##E)

static inline uint32_t
nvb097_th_bl_source(const struct nil_tic_format *fmt,
                    enum pipe_swizzle swz, bool is_int)
{
   switch (swz) {
   case PIPE_SWIZZLE_X: return fmt->src_x;
   case PIPE_SWIZZLE_Y: return fmt->src_y;
   case PIPE_SWIZZLE_Z: return fmt->src_z;
   case PIPE_SWIZZLE_W: return fmt->src_w;
   case PIPE_SWIZZLE_0:
      return NVB097_TEXHEAD_BL_X_SOURCE_IN_ZERO;
   case PIPE_SWIZZLE_1:
      return is_int ? NVB097_TEXHEAD_BL_X_SOURCE_IN_ONE_INT :
                      NVB097_TEXHEAD_BL_X_SOURCE_IN_ONE_FLOAT;
   default:
      unreachable("Invalid component swizzle");
   }
}

static uint32_t
nvb097_th_bl_0(enum pipe_format format, const enum pipe_swizzle swizzle[4])
{
   const struct nil_tic_format *fmt = nil_tic_format_for_pipe(format);
   const bool is_int = util_format_is_pure_integer(format);

   uint32_t source[4];
   for (unsigned i = 0; i < 4; i++)
      source[i] = nvb097_th_bl_source(fmt, swizzle[i], is_int);

   uint32_t th_0 = 0;
   TH_SET_U(&th_0, NVB097, BL, COMPONENTS, fmt->comp_sizes);
   TH_SET_U(&th_0, NVB097, BL, R_DATA_TYPE, fmt->type_r);
   TH_SET_U(&th_0, NVB097, BL, G_DATA_TYPE, fmt->type_g);
   TH_SET_U(&th_0, NVB097, BL, B_DATA_TYPE, fmt->type_b);
   TH_SET_U(&th_0, NVB097, BL, A_DATA_TYPE, fmt->type_a);
   TH_SET_U(&th_0, NVB097, BL, X_SOURCE, source[0]);
   TH_SET_U(&th_0, NVB097, BL, Y_SOURCE, source[1]);
   TH_SET_U(&th_0, NVB097, BL, Z_SOURCE, source[2]);
   TH_SET_U(&th_0, NVB097, BL, W_SOURCE, source[3]);

   return th_0;
}

static uint32_t
nil_to_nvb097_texture_type(enum nil_view_type type)
{
#define CASE(NIL, NV) \
   case NIL_VIEW_TYPE_##NIL: return NVB097_TEXHEAD_BL_TEXTURE_TYPE_##NV;

   switch (type) {
   CASE(1D,             ONE_D);
   CASE(2D,             TWO_D);
   CASE(3D,             THREE_D);
   CASE(CUBE,           CUBEMAP);
   CASE(1D_ARRAY,       ONE_D_ARRAY);
   CASE(2D_ARRAY,       TWO_D_ARRAY);
   CASE(CUBE_ARRAY,     CUBEMAP_ARRAY);
   default: unreachable("Invalid image view type");
   }

#undef CASE
}

static uint32_t
uint_to_nvb097_multi_sample_count(uint32_t samples)
{
   switch (samples) {
   case 1:  return NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT_MODE_1X1;
   case 2:  return NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT_MODE_2X1;
   case 4:  return NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT_MODE_2X2;
   case 8:  return NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT_MODE_4X2;
   case 16: return NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT_MODE_4X4;
   default:
      unreachable("Unsupported sample count");
   }
}

void
nil_image_fill_tic(struct nouveau_ws_device *dev,
                   const struct nil_image *image,
                   const struct nil_view *view,
                   uint64_t base_address,
                   void *desc_out)
{
   assert(util_format_get_blocksize(image->format) ==
          util_format_get_blocksize(view->format));
   assert(view->base_level + view->num_levels <= image->num_levels);
   assert(view->base_array_layer + view->array_len <= image->extent_px.a);

   uint32_t th[8] = { };

   th[0] = nvb097_th_bl_0(view->format, view->swizzle);

   /* There's no base layer field in the texture header */
   const uint64_t layer_address =
      base_address + view->base_array_layer * image->array_stride_B;
   assert((layer_address & BITFIELD_MASK(9)) == 0);
   TH_SET_U(th, NVB097, BL, ADDRESS_BITS31TO9, (uint32_t)layer_address >> 9);
   TH_SET_U(th, NVB097, BL, ADDRESS_BITS47TO32, layer_address >> 32);

   TH_SET_E(th, NVB097, BL, HEADER_VERSION, SELECT_BLOCKLINEAR);

   const struct nil_tiling *tiling = &image->levels[0].tiling;
   assert(tiling->is_tiled);
   assert(tiling->gob_height_8);
   TH_SET_E(th, NVB097, BL, GOBS_PER_BLOCK_WIDTH, ONE_GOB);
   TH_SET_U(th, NVB097, BL, GOBS_PER_BLOCK_HEIGHT, tiling->y_log2);
   TH_SET_U(th, NVB097, BL, GOBS_PER_BLOCK_DEPTH, tiling->z_log2);

   TH_SET_B(th, NVB097, BL, LOD_ANISO_QUALITY2, true);
   TH_SET_E(th, NVB097, BL, LOD_ANISO_QUALITY, LOD_QUALITY_HIGH);
   TH_SET_E(th, NVB097, BL, LOD_ISO_QUALITY, LOD_QUALITY_HIGH);
   TH_SET_E(th, NVB097, BL, ANISO_COARSE_SPREAD_MODIFIER, SPREAD_MODIFIER_NONE);

   const uint32_t width = image->extent_px.width;
   const uint32_t height = image->extent_px.height;
   uint32_t depth;
   switch (view->type) {
   case NIL_VIEW_TYPE_1D:
   case NIL_VIEW_TYPE_1D_ARRAY:
   case NIL_VIEW_TYPE_2D:
   case NIL_VIEW_TYPE_2D_ARRAY:
      assert(image->extent_px.depth == 1);
      depth = view->array_len;
      break;
   case NIL_VIEW_TYPE_CUBE:
   case NIL_VIEW_TYPE_CUBE_ARRAY:
      assert(image->dim == NIL_IMAGE_DIM_2D);
      assert(view->array_len % 6 == 0);
      depth = view->array_len / 6;
      break;
   case NIL_VIEW_TYPE_3D:
      assert(image->dim == NIL_IMAGE_DIM_3D);
      depth = image->extent_px.depth;
      break;
   default:
      unreachable("Unsupported image view target");
   };

   TH_SET_U(th, NVB097, BL, WIDTH_MINUS_ONE, width - 1);
   TH_SET_U(th, NVB097, BL, HEIGHT_MINUS_ONE, height - 1);
   TH_SET_U(th, NVB097, BL, DEPTH_MINUS_ONE, depth - 1);

   if (view->type != NIL_VIEW_TYPE_3D && view->array_len == 1 &&
       view->base_level == 0 && view->num_levels == 1) {
      /* The Unnormalized coordinates bit in the sampler gets ignored if the
       * referenced image has more than one miplevel.  Fortunately, Vulkan has
       * restrictions requiring the view to be a single-layer single-LOD view
       * in order to use nonnormalizedCoordinates = VK_TRUE in the sampler.
       * From the Vulkan 1.3.255 spec:
       *
       *    "When unnormalizedCoordinates is VK_TRUE, images the sampler is
       *    used with in the shader have the following requirements:
       *
       *     - The viewType must be either VK_IMAGE_VIEW_TYPE_1D or
       *       VK_IMAGE_VIEW_TYPE_2D.
       *     - The image view must have a single layer and a single mip
       *       level."
       *
       * Under these conditions, the view is simply LOD 0 of a single array
       * slice so we don't need to care about aray stride between slices so
       * it's safe to set the number of miplevels to 0 regardless of how many
       * the image actually has.
       */
      TH_SET_U(th, NVB097, BL, MAX_MIP_LEVEL, 0);
   } else {
      TH_SET_U(th, NVB097, BL, MAX_MIP_LEVEL, image->num_levels - 1);
   }

   TH_SET_U(th, NVB097, BL, TEXTURE_TYPE,
            nil_to_nvb097_texture_type(view->type));

   TH_SET_B(th, NVB097, BL, S_R_G_B_CONVERSION,
            util_format_is_srgb(view->format));

   TH_SET_E(th, NVB097, BL, SECTOR_PROMOTION, PROMOTE_TO_2_V);
   TH_SET_E(th, NVB097, BL, BORDER_SIZE, BORDER_SAMPLER_COLOR);

   /* In the sampler, the two options for FLOAT_COORD_NORMALIZATION are:
    *
    *  - FORCE_UNNORMALIZED_COORDS
    *  - USE_HEADER_SETTING
    *
    * So we set it to normalized in the header and let the sampler select
    * that or force non-normalized.
    */
   TH_SET_B(th, NVB097, BL, NORMALIZED_COORDS, true);

   TH_SET_E(th, NVB097, BL, ANISO_FINE_SPREAD_FUNC, SPREAD_FUNC_TWO);
   TH_SET_E(th, NVB097, BL, ANISO_COARSE_SPREAD_FUNC, SPREAD_FUNC_TWO);

   TH_SET_U(th, NVB097, BL, RES_VIEW_MIN_MIP_LEVEL, view->base_level);
   TH_SET_U(th, NVB097, BL, RES_VIEW_MAX_MIP_LEVEL,
            view->num_levels + view->base_level - 1);

   TH_SET_U(th, NVB097, BL, MULTI_SAMPLE_COUNT,
            uint_to_nvb097_multi_sample_count(image->num_samples));

   memcpy(desc_out, th, sizeof(th));
}

void
nil_buffer_fill_tic(struct nouveau_ws_device *dev,
                    uint64_t base_address,
                    enum pipe_format format,
                    uint32_t num_elements,
                    void *desc_out)
{
   static const enum pipe_swizzle identity_swizzle[4] = {
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z,
      PIPE_SWIZZLE_W,
   };
   uint32_t th[8] = { };

   assert(!util_format_is_compressed(format));
   th[0] = nvb097_th_bl_0(format, identity_swizzle);

   TH_SET_U(th, NVB097, 1D, ADDRESS_BITS31TO0, base_address);
   TH_SET_U(th, NVB097, 1D, ADDRESS_BITS47TO32, base_address >> 32);
   TH_SET_E(th, NVB097, 1D, HEADER_VERSION, SELECT_ONE_D_BUFFER);

   TH_SET_U(th, NVB097, 1D, WIDTH_MINUS_ONE_BITS15TO0,
            (num_elements - 1) & 0xffff);
   TH_SET_U(th, NVB097, 1D, WIDTH_MINUS_ONE_BITS31TO16,
            (num_elements - 1) >> 16);

   TH_SET_E(th, NVB097, 1D, TEXTURE_TYPE, ONE_D_BUFFER);

   /* TODO: Do we need this? */
   TH_SET_E(th, NVB097, 1D, SECTOR_PROMOTION, PROMOTE_TO_2_V);

   memcpy(desc_out, th, sizeof(th));
}
