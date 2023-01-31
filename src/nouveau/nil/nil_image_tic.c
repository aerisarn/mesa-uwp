#include "nil_image.h"

#include "nil_format.h"
#include "util/u_math.h"

#include "gallium/drivers/nouveau/nv50/g80_defs.xml.h"
#include "gallium/drivers/nouveau/nv50/g80_texture.xml.h"
#include "gallium/drivers/nouveau/nvc0/gm107_texture.xml.h"

static inline uint32_t
tic_source(const struct nil_tic_format *fmt,
           enum pipe_swizzle swz, bool is_int)
{
   switch (swz) {
   case PIPE_SWIZZLE_X: return fmt->src_x;
   case PIPE_SWIZZLE_Y: return fmt->src_y;
   case PIPE_SWIZZLE_Z: return fmt->src_z;
   case PIPE_SWIZZLE_W: return fmt->src_w;
   case PIPE_SWIZZLE_0:
      return G80_TIC_SOURCE_ZERO;
   case PIPE_SWIZZLE_1:
      return is_int ? G80_TIC_SOURCE_ONE_INT : G80_TIC_SOURCE_ONE_FLOAT;
   default:
      unreachable("Invalid component swizzle");
   }
}

static uint32_t
gm107_tic2_0_format(enum pipe_format format,
                    const enum pipe_swizzle swizzle[4])
{
   const struct nil_tic_format *fmt = nil_tic_format_for_pipe(format);
   const bool is_int = util_format_is_pure_integer(format);

   uint32_t source[4];
   for (unsigned i = 0; i < 4; i++)
      source[i] = tic_source(fmt, swizzle[i], is_int);

   uint32_t tic;
   tic  = fmt->comp_sizes << GM107_TIC2_0_COMPONENTS_SIZES__SHIFT;
   tic |= fmt->type_r << GM107_TIC2_0_R_DATA_TYPE__SHIFT;
   tic |= fmt->type_g << GM107_TIC2_0_G_DATA_TYPE__SHIFT;
   tic |= fmt->type_b << GM107_TIC2_0_B_DATA_TYPE__SHIFT;
   tic |= fmt->type_a << GM107_TIC2_0_A_DATA_TYPE__SHIFT;
   tic |= source[0] << GM107_TIC2_0_X_SOURCE__SHIFT;
   tic |= source[1] << GM107_TIC2_0_Y_SOURCE__SHIFT;
   tic |= source[2] << GM107_TIC2_0_Z_SOURCE__SHIFT;
   tic |= source[3] << GM107_TIC2_0_W_SOURCE__SHIFT;

   return tic;
}

static uint32_t
gm107_tic2_4_view_type(enum nil_view_type type)
{
   switch (type) {
   case NIL_VIEW_TYPE_1D:
      return GM107_TIC2_4_TEXTURE_TYPE_ONE_D;
   case NIL_VIEW_TYPE_2D:
      return GM107_TIC2_4_TEXTURE_TYPE_TWO_D;
   case NIL_VIEW_TYPE_3D:
      return GM107_TIC2_4_TEXTURE_TYPE_THREE_D;
   case NIL_VIEW_TYPE_CUBE:
      return GM107_TIC2_4_TEXTURE_TYPE_CUBEMAP;
   case NIL_VIEW_TYPE_1D_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_ONE_D_ARRAY;
   case NIL_VIEW_TYPE_2D_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_TWO_D_ARRAY;
   case NIL_VIEW_TYPE_CUBE_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_CUBE_ARRAY;
   default:
      unreachable("Invalid image view type");
   }
}

static uint32_t
gm107_tic7_4_multi_sample_count(uint32_t samples)
{
   switch (samples) {
   case 1:  return GM107_TIC2_7_MULTI_SAMPLE_COUNT_1X1;
   case 2:  return GM107_TIC2_7_MULTI_SAMPLE_COUNT_2X1;
   case 4:  return GM107_TIC2_7_MULTI_SAMPLE_COUNT_2X2;
   case 8:  return GM107_TIC2_7_MULTI_SAMPLE_COUNT_4X2;
   case 16: return GM107_TIC2_7_MULTI_SAMPLE_COUNT_4X4;
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

   uint32_t tic[8] = { };

   tic[0] = gm107_tic2_0_format(view->format, view->swizzle);

   tic[3] |= GM107_TIC2_3_LOD_ANISO_QUALITY_2;
   tic[4] |= GM107_TIC2_4_SECTOR_PROMOTION_PROMOTE_TO_2_V;
   tic[4] |= GM107_TIC2_4_BORDER_SIZE_SAMPLER_COLOR;

   if (util_format_is_srgb(view->format))
      tic[4] |= GM107_TIC2_4_SRGB_CONVERSION;

   /* TODO: Unnormalized? */
   tic[5] |= GM107_TIC2_5_NORMALIZED_COORDS;

   tic[2] |= GM107_TIC2_2_HEADER_VERSION_BLOCKLINEAR;
   assert(image->levels[0].tiling.gob_height_8);
   tic[3] |= (uint32_t)image->levels[0].tiling.y_log2 <<
             GM107_TIC2_3_GOBS_PER_BLOCK_HEIGHT__SHIFT;
   tic[3] |= (uint32_t)image->levels[0].tiling.z_log2 <<
             GM107_TIC2_3_GOBS_PER_BLOCK_DEPTH__SHIFT;

   /* There doesn't seem to be a base layer field in TIC */
   const uint64_t layer_address =
      base_address + view->base_array_layer * image->array_stride_B;
   tic[1]  = layer_address;
   tic[2] |= layer_address >> 32;

   tic[4] |= gm107_tic2_4_view_type(view->type);

   /* TODO: NV50_TEXVIEW_FILTER_MSAA8 */
   tic[3] |= GM107_TIC2_3_LOD_ANISO_QUALITY_HIGH |
             GM107_TIC2_3_LOD_ISO_QUALITY_HIGH;

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

   tic[4] |= (width - 1) << GM107_TIC2_4_WIDTH_MINUS_ONE__SHIFT;
   tic[5] |= (height - 1) << GM107_TIC2_5_HEIGHT_MINUS_ONE__SHIFT;
   tic[5] |= (depth - 1) << GM107_TIC2_5_DEPTH_MINUS_ONE__SHIFT;

   const uint32_t last_level = view->num_levels + view->base_level - 1;
   tic[3] |= last_level << GM107_TIC2_3_MAX_MIP_LEVEL__SHIFT;

   tic[6] |= GM107_TIC2_6_ANISO_FINE_SPREAD_FUNC_TWO;
   tic[6] |= GM107_TIC2_6_ANISO_COARSE_SPREAD_FUNC_ONE;

   tic[7] |= (last_level << 4) | view->base_level;
   tic[7] |= gm107_tic7_4_multi_sample_count(image->num_samples);

   memcpy(desc_out, tic, sizeof(tic));
}
