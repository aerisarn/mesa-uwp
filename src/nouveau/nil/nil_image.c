#include "nil_image.h"

#include "util/u_math.h"

static struct nil_extent4d
nil_minify_extent4d(struct nil_extent4d extent, uint32_t level)
{
   return (struct nil_extent4d) {
      .w = u_minify(extent.w, level),
      .h = u_minify(extent.h, level),
      .d = u_minify(extent.d, level),
      .a = extent.a,
   };
}

static struct nil_extent4d
nil_extent4d_div_round_up(struct nil_extent4d num, struct nil_extent4d denom)
{
   return (struct nil_extent4d) {
      .w = DIV_ROUND_UP(num.w, denom.w),
      .h = DIV_ROUND_UP(num.h, denom.h),
      .d = DIV_ROUND_UP(num.d, denom.d),
      .a = DIV_ROUND_UP(num.a, denom.a),
   };
}

static struct nil_extent4d
nil_extent4d_align(struct nil_extent4d ext, struct nil_extent4d align)
{
   return (struct nil_extent4d) {
      .w = ALIGN_POT(ext.w, align.w),
      .h = ALIGN_POT(ext.h, align.h),
      .d = ALIGN_POT(ext.d, align.d),
      .a = ALIGN_POT(ext.a, align.a),
   };
}

static struct nil_extent4d
nil_extent4d_px_to_el(struct nil_extent4d extent_px,
                      enum pipe_format format)
{
   const struct util_format_description *fmt =
      util_format_description(format);

   const struct nil_extent4d block_extent_px = {
      .w = fmt->block.width,
      .h = fmt->block.height,
      .d = fmt->block.depth,
      .a = 1,
   };

   return nil_extent4d_div_round_up(extent_px, block_extent_px);
}

static struct nil_extent4d
nil_extent4d_el_to_B(struct nil_extent4d extent_el,
                     uint32_t B_per_el)
{
   struct nil_extent4d extent_B = extent_el;
   extent_B.w *= B_per_el;
   return extent_B;
}

static struct nil_extent4d
nil_extent4d_B_to_GOB(struct nil_extent4d extent_B,
                      bool gob_height_8)
{
   const struct nil_extent4d gob_extent_B = {
      .w = NIL_GOB_WIDTH_B,
      .h = NIL_GOB_HEIGHT(gob_height_8),
      .d = NIL_GOB_DEPTH,
      .a = 1,
   };

   return nil_extent4d_div_round_up(extent_B, gob_extent_B);
}

static struct nil_extent4d
nil_tiling_extent_B(struct nil_tiling tiling)
{
   if (tiling.is_tiled) {
      return (struct nil_extent4d) {
         .w = NIL_GOB_WIDTH_B, /* Tiles are always 1 GOB wide */
         .h = NIL_GOB_HEIGHT(tiling.gob_height_8) << tiling.y_log2,
         .d = NIL_GOB_DEPTH << tiling.z_log2,
         .a = 1,
      };
   } else {
      return nil_extent4d(1, 1, 1, 1);
   }
}

static struct nil_tiling
choose_tiling(struct nil_extent4d extent_B,
              enum nil_image_usage_flags usage)
{
   struct nil_tiling tiling = {
      .is_tiled = true,
      .gob_height_8 = true,
   };

   const struct nil_extent4d extent_GOB =
      nil_extent4d_B_to_GOB(extent_B, tiling.gob_height_8);

   const uint32_t height_log2 = util_logbase2_ceil(extent_GOB.height);
   const uint32_t depth_log2 = util_logbase2_ceil(extent_GOB.depth);

   tiling.y_log2 = MIN2(height_log2, 5);
   tiling.z_log2 = MIN2(depth_log2, 5);

   if (usage & NIL_IMAGE_USAGE_2D_VIEW_BIT)
      tiling.z_log2 = 0;

   return tiling;
}

static uint32_t
nil_tiling_size_B(struct nil_tiling tiling)
{
   const struct nil_extent4d extent_B = nil_tiling_extent_B(tiling);
   return extent_B.w * extent_B.h * extent_B.d * extent_B.a;
}

static struct nil_extent4d
nil_extent4d_B_to_tl(struct nil_extent4d extent_B,
                     struct nil_tiling tiling)
{
   return nil_extent4d_div_round_up(extent_B, nil_tiling_extent_B(tiling));
}

static struct nil_extent4d
image_level_extent_B(const struct nil_image *image, uint32_t level)
{
   const struct nil_extent4d level_extent_px =
      nil_minify_extent4d(image->extent_px, level);
   const struct nil_extent4d level_extent_el =
      nil_extent4d_px_to_el(level_extent_px, image->format);
   const uint32_t B_per_el = util_format_get_blocksize(image->format);
   return nil_extent4d_el_to_B(level_extent_el, B_per_el);
}

bool
nil_image_init(struct nouveau_ws_device *dev,
               struct nil_image *image,
               const struct nil_image_init_info *restrict info)
{
   switch (info->dim) {
   case NIL_IMAGE_DIM_1D:
      assert(info->extent_px.h == 1);
      assert(info->extent_px.d == 1);
      assert(info->samples == 1);
      break;
   case NIL_IMAGE_DIM_2D:
      assert(info->extent_px.d == 1);
      break;
   case NIL_IMAGE_DIM_3D:
      assert(info->extent_px.a == 1);
      assert(info->samples == 1);
      break;
   }

   *image = (struct nil_image) {
      .dim = info->dim,
      .format = info->format,
      .extent_px = info->extent_px,
      .num_levels = info->levels,
      .num_samples = info->samples,
   };

   uint64_t layer_size_B = 0;
   for (uint32_t l = 0; l < info->levels; l++) {
      struct nil_extent4d lvl_ext_B = image_level_extent_B(image, l);

      /* Tiling is chosen per-level with LOD0 acting as a maximum */
      struct nil_tiling lvl_tiling = choose_tiling(lvl_ext_B, info->usage);

      /* Align the size to tiles */
      struct nil_extent4d lvl_tiling_ext_B = nil_tiling_extent_B(lvl_tiling);
      lvl_ext_B = nil_extent4d_align(lvl_ext_B, lvl_tiling_ext_B);

      image->levels[l] = (struct nil_image_level) {
         .offset_B = layer_size_B,
         .tiling = lvl_tiling,
         .row_stride_B = lvl_ext_B.width,
      };
      layer_size_B += (uint64_t)lvl_ext_B.w *
                      (uint64_t)lvl_ext_B.h *
                      (uint64_t)lvl_ext_B.d;
   }

   /* Align the image and array stride to a single level0 tile */
   image->align_B = nil_tiling_size_B(image->levels[0].tiling);

   /* I have no idea why but hardware seems to align layer strides */
   image->array_stride_B = ALIGN(layer_size_B, image->align_B);

   image->size_B = (uint64_t)image->array_stride_B * image->extent_px.a;

   return true;
}

uint64_t
nil_image_level_size_B(const struct nil_image *image, uint32_t level)
{
   assert(level < image->num_levels);

   /* See the nil_image::levels[] computations */
   struct nil_extent4d lvl_ext_B = image_level_extent_B(image, level);
   struct nil_extent4d lvl_tiling_ext_B =
      nil_tiling_extent_B(image->levels[level].tiling);
   lvl_ext_B = nil_extent4d_align(lvl_ext_B, lvl_tiling_ext_B);

   return (uint64_t)lvl_ext_B.w *
          (uint64_t)lvl_ext_B.h *
          (uint64_t)lvl_ext_B.d;
}

uint64_t
nil_image_level_depth_stride_B(const struct nil_image *image, uint32_t level)
{
   assert(level < image->num_levels);

   /* See the nil_image::levels[] computations */
   struct nil_extent4d lvl_ext_B = image_level_extent_B(image, level);
   struct nil_extent4d lvl_tiling_ext_B =
      nil_tiling_extent_B(image->levels[level].tiling);
   lvl_ext_B = nil_extent4d_align(lvl_ext_B, lvl_tiling_ext_B);

   return (uint64_t)lvl_ext_B.w * (uint64_t)lvl_ext_B.h;
}
