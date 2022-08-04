/*
 * Copyright © 2020 Mike Blumenkrantz
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#include "tgsi/tgsi_from_mesa.h"



#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

static VkDescriptorSetLayout
descriptor_layout_create(struct zink_screen *screen, enum zink_descriptor_type t, VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings)
{
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dcslci = {0};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {0};
   VkDescriptorBindingFlags flags[ZINK_MAX_DESCRIPTORS_PER_TYPE];
   dcslci.pNext = &fci;
   if (t == ZINK_DESCRIPTOR_TYPES)
      dcslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
   fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
   fci.bindingCount = num_bindings;
   fci.pBindingFlags = flags;
   for (unsigned i = 0; i < num_bindings; i++) {
      flags[i] = 0;
   }
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;
   VkDescriptorSetLayoutSupport supp;
   supp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
   supp.pNext = NULL;
   supp.supported = VK_FALSE;
   if (VKSCR(GetDescriptorSetLayoutSupport)) {
      VKSCR(GetDescriptorSetLayoutSupport)(screen->dev, &dcslci, &supp);
      if (supp.supported == VK_FALSE) {
         debug_printf("vkGetDescriptorSetLayoutSupport claims layout is unsupported\n");
         return VK_NULL_HANDLE;
      }
   }
   VkResult result = VKSCR(CreateDescriptorSetLayout)(screen->dev, &dcslci, 0, &dsl);
   if (result != VK_SUCCESS)
      mesa_loge("ZINK: vkCreateDescriptorSetLayout failed (%s)", vk_Result_to_str(result));
   return dsl;
}

static uint32_t
hash_descriptor_layout(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_layout_key *k = key;
   hash = XXH32(&k->num_bindings, sizeof(unsigned), hash);
   /* only hash first 3 members: no holes and the rest are always constant */
   for (unsigned i = 0; i < k->num_bindings; i++)
      hash = XXH32(&k->bindings[i], offsetof(VkDescriptorSetLayoutBinding, stageFlags), hash);

   return hash;
}

static bool
equals_descriptor_layout(const void *a, const void *b)
{
   const struct zink_descriptor_layout_key *a_k = a;
   const struct zink_descriptor_layout_key *b_k = b;
   return a_k->num_bindings == b_k->num_bindings &&
          !memcmp(a_k->bindings, b_k->bindings, a_k->num_bindings * sizeof(VkDescriptorSetLayoutBinding));
}

static struct zink_descriptor_layout *
create_layout(struct zink_context *ctx, enum zink_descriptor_type type,
              VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
              struct zink_descriptor_layout_key **layout_key)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayout dsl = descriptor_layout_create(screen, type, bindings, num_bindings);
   if (!dsl)
      return NULL;

   struct zink_descriptor_layout_key *k = ralloc(ctx, struct zink_descriptor_layout_key);
   k->num_bindings = num_bindings;
   if (num_bindings) {
      size_t bindings_size = num_bindings * sizeof(VkDescriptorSetLayoutBinding);
      k->bindings = ralloc_size(k, bindings_size);
      if (!k->bindings) {
         ralloc_free(k);
         VKSCR(DestroyDescriptorSetLayout)(screen->dev, dsl, NULL);
         return NULL;
      }
      memcpy(k->bindings, bindings, bindings_size);
   }

   struct zink_descriptor_layout *layout = rzalloc(ctx, struct zink_descriptor_layout);
   layout->layout = dsl;
   *layout_key = k;
   return layout;
}

struct zink_descriptor_layout *
zink_descriptor_util_layout_get(struct zink_context *ctx, enum zink_descriptor_type type,
                      VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
                      struct zink_descriptor_layout_key **layout_key)
{
   uint32_t hash = 0;
   struct zink_descriptor_layout_key key = {
      .num_bindings = num_bindings,
      .bindings = bindings,
   };

   if (type != ZINK_DESCRIPTOR_TYPES) {
      hash = hash_descriptor_layout(&key);
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(&ctx->desc_set_layouts[type], hash, &key);
      if (he) {
         *layout_key = (void*)he->key;
         return he->data;
      }
   }

   struct zink_descriptor_layout *layout = create_layout(ctx, type, bindings, num_bindings, layout_key);
   if (layout && type != ZINK_DESCRIPTOR_TYPES) {
      _mesa_hash_table_insert_pre_hashed(&ctx->desc_set_layouts[type], hash, *layout_key, layout);
   }
   return layout;
}


static uint32_t
hash_descriptor_pool_key(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_pool_key *k = key;
   hash = XXH32(&k->layout, sizeof(void*), hash);
   for (unsigned i = 0; i < k->num_type_sizes; i++)
      hash = XXH32(&k->sizes[i], sizeof(VkDescriptorPoolSize), hash);

   return hash;
}

static bool
equals_descriptor_pool_key(const void *a, const void *b)
{
   const struct zink_descriptor_pool_key *a_k = a;
   const struct zink_descriptor_pool_key *b_k = b;
   const unsigned a_num_type_sizes = a_k->num_type_sizes;
   const unsigned b_num_type_sizes = b_k->num_type_sizes;
   return a_k->layout == b_k->layout &&
          a_num_type_sizes == b_num_type_sizes &&
          !memcmp(a_k->sizes, b_k->sizes, b_num_type_sizes * sizeof(VkDescriptorPoolSize));
}

struct zink_descriptor_pool_key *
zink_descriptor_util_pool_key_get(struct zink_context *ctx, enum zink_descriptor_type type,
                                  struct zink_descriptor_layout_key *layout_key,
                                  VkDescriptorPoolSize *sizes, unsigned num_type_sizes)
{
   uint32_t hash = 0;
   struct zink_descriptor_pool_key key;
   key.num_type_sizes = num_type_sizes;
   if (type != ZINK_DESCRIPTOR_TYPES) {
      key.layout = layout_key;
      memcpy(key.sizes, sizes, num_type_sizes * sizeof(VkDescriptorPoolSize));
      hash = hash_descriptor_pool_key(&key);
      struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->desc_pool_keys[type], hash, &key);
      if (he)
         return (void*)he->key;
   }

   struct zink_descriptor_pool_key *pool_key = rzalloc(ctx, struct zink_descriptor_pool_key);
   pool_key->layout = layout_key;
   pool_key->num_type_sizes = num_type_sizes;
   assert(pool_key->num_type_sizes);
   memcpy(pool_key->sizes, sizes, num_type_sizes * sizeof(VkDescriptorPoolSize));
   if (type != ZINK_DESCRIPTOR_TYPES)
      _mesa_set_add_pre_hashed(&ctx->desc_pool_keys[type], hash, pool_key);
   return pool_key;
}

static void
init_push_binding(VkDescriptorSetLayoutBinding *binding, unsigned i, VkDescriptorType type)
{
   binding->binding = tgsi_processor_to_shader_stage(i);
   binding->descriptorType = type;
   binding->descriptorCount = 1;
   binding->stageFlags = zink_shader_stage(i);
   binding->pImmutableSamplers = NULL;
}

static VkDescriptorType
get_push_types(struct zink_screen *screen, enum zink_descriptor_type *dsl_type)
{
   *dsl_type = screen->info.have_KHR_push_descriptor ? ZINK_DESCRIPTOR_TYPES : ZINK_DESCRIPTOR_TYPE_UBO;
   return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

static struct zink_descriptor_layout *
create_gfx_layout(struct zink_context *ctx, struct zink_descriptor_layout_key **layout_key, bool fbfetch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[PIPE_SHADER_TYPES];
   enum zink_descriptor_type dsl_type;
   VkDescriptorType vktype = get_push_types(screen, &dsl_type);
   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++)
      init_push_binding(&bindings[i], i, vktype);
   if (fbfetch) {
      bindings[ZINK_SHADER_COUNT].binding = ZINK_FBFETCH_BINDING;
      bindings[ZINK_SHADER_COUNT].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      bindings[ZINK_SHADER_COUNT].descriptorCount = 1;
      bindings[ZINK_SHADER_COUNT].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      bindings[ZINK_SHADER_COUNT].pImmutableSamplers = NULL;
   }
   return create_layout(ctx, dsl_type, bindings, fbfetch ? ARRAY_SIZE(bindings) : ARRAY_SIZE(bindings) - 1, layout_key);
}

bool
zink_descriptor_util_push_layouts_get(struct zink_context *ctx, struct zink_descriptor_layout **dsls, struct zink_descriptor_layout_key **layout_keys)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding compute_binding;
   enum zink_descriptor_type dsl_type;
   VkDescriptorType vktype = get_push_types(screen, &dsl_type);
   init_push_binding(&compute_binding, PIPE_SHADER_COMPUTE, vktype);
   dsls[0] = create_gfx_layout(ctx, &layout_keys[0], false);
   dsls[1] = create_layout(ctx, dsl_type, &compute_binding, 1, &layout_keys[1]);
   return dsls[0] && dsls[1];
}

VkImageLayout
zink_descriptor_util_image_layout_eval(const struct zink_context *ctx, const struct zink_resource *res, bool is_compute)
{
   if (res->bindless[0] || res->bindless[1]) {
      /* bindless needs most permissive layout */
      if (res->image_bind_count[0] || res->image_bind_count[1])
         return VK_IMAGE_LAYOUT_GENERAL;
      return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   }
   if (res->image_bind_count[is_compute])
      return VK_IMAGE_LAYOUT_GENERAL;
   if (res->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      if (!is_compute && res->fb_binds &&
          ctx->gfx_pipeline_state.render_pass && ctx->gfx_pipeline_state.render_pass->state.rts[ctx->fb_state.nr_cbufs].mixed_zs)
         return VK_IMAGE_LAYOUT_GENERAL;
      if (res->obj->vkusage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
         return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   }
   return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool
zink_descriptor_util_alloc_sets(struct zink_screen *screen, VkDescriptorSetLayout dsl, VkDescriptorPool pool, VkDescriptorSet *sets, unsigned num_sets)
{
   VkDescriptorSetAllocateInfo dsai;
   VkDescriptorSetLayout *layouts = alloca(sizeof(*layouts) * num_sets);
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = pool;
   dsai.descriptorSetCount = num_sets;
   for (unsigned i = 0; i < num_sets; i ++)
      layouts[i] = dsl;
   dsai.pSetLayouts = layouts;

   VkResult result = VKSCR(AllocateDescriptorSets)(screen->dev, &dsai, sets);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: %" PRIu64 " failed to allocate descriptor set :/ (%s)", (uint64_t)dsl, vk_Result_to_str(result));
      return false;
   }
   return true;
}

bool
zink_descriptor_layouts_init(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!_mesa_hash_table_init(&ctx->desc_set_layouts[i], ctx, hash_descriptor_layout, equals_descriptor_layout))
         return false;
      if (!_mesa_set_init(&ctx->desc_pool_keys[i], ctx, hash_descriptor_pool_key, equals_descriptor_pool_key))
         return false;
   }
   return true;
}

void
zink_descriptor_layouts_deinit(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      hash_table_foreach(&ctx->desc_set_layouts[i], he) {
         struct zink_descriptor_layout *layout = he->data;
         VKSCR(DestroyDescriptorSetLayout)(screen->dev, layout->layout, NULL);
         ralloc_free(layout);
         _mesa_hash_table_remove(&ctx->desc_set_layouts[i], he);
      }
   }
}


void
zink_descriptor_util_init_fbfetch(struct zink_context *ctx)
{
   if (ctx->dd->has_fbfetch)
      return;

   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VKSCR(DestroyDescriptorSetLayout)(screen->dev, ctx->dd->push_dsl[0]->layout, NULL);
   //don't free these now, let ralloc free on teardown to avoid invalid access
   //ralloc_free(ctx->dd->push_dsl[0]);
   //ralloc_free(ctx->dd->push_layout_keys[0]);
   ctx->dd->push_dsl[0] = create_gfx_layout(ctx, &ctx->dd->push_layout_keys[0], true);
   ctx->dd->has_fbfetch = true;
}

ALWAYS_INLINE static VkDescriptorType
type_from_bindless_index(unsigned idx)
{
   switch (idx) {
   case 0: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   case 1: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   case 2: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   case 3: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
   default:
      unreachable("unknown index");
   }
}

void
zink_descriptors_init_bindless(struct zink_context *ctx)
{
   if (ctx->dd->bindless_set)
      return;

   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[4];
   const unsigned num_bindings = 4;
   VkDescriptorSetLayoutCreateInfo dcslci = {0};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {0};
   VkDescriptorBindingFlags flags[4];
   dcslci.pNext = &fci;
   dcslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
   fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
   fci.bindingCount = num_bindings;
   fci.pBindingFlags = flags;
   for (unsigned i = 0; i < num_bindings; i++) {
      flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
   }
   for (unsigned i = 0; i < num_bindings; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = type_from_bindless_index(i);
      bindings[i].descriptorCount = ZINK_MAX_BINDLESS_HANDLES;
      bindings[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
      bindings[i].pImmutableSamplers = NULL;
   }
   
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;
   VkResult result = VKSCR(CreateDescriptorSetLayout)(screen->dev, &dcslci, 0, &ctx->dd->bindless_layout);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateDescriptorSetLayout failed (%s)", vk_Result_to_str(result));
      return;
   }

   VkDescriptorPoolCreateInfo dpci = {0};
   VkDescriptorPoolSize sizes[4];
   for (unsigned i = 0; i < 4; i++) {
      sizes[i].type = type_from_bindless_index(i);
      sizes[i].descriptorCount = ZINK_MAX_BINDLESS_HANDLES;
   }
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = 4;
   dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
   dpci.maxSets = 1;
   result = VKSCR(CreateDescriptorPool)(screen->dev, &dpci, 0, &ctx->dd->bindless_pool);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateDescriptorPool failed (%s)", vk_Result_to_str(result));
      return;
   }

   zink_descriptor_util_alloc_sets(screen, ctx->dd->bindless_layout, ctx->dd->bindless_pool, &ctx->dd->bindless_set, 1);
}

void
zink_descriptors_deinit_bindless(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (ctx->dd->bindless_layout)
      VKSCR(DestroyDescriptorSetLayout)(screen->dev, ctx->dd->bindless_layout, NULL);
   if (ctx->dd->bindless_pool)
      VKSCR(DestroyDescriptorPool)(screen->dev, ctx->dd->bindless_pool, NULL);
}

void
zink_descriptors_update_bindless(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < 2; i++) {
      if (!ctx->di.bindless_dirty[i])
         continue;
      while (util_dynarray_contains(&ctx->di.bindless[i].updates, uint32_t)) {
         uint32_t handle = util_dynarray_pop(&ctx->di.bindless[i].updates, uint32_t);
         bool is_buffer = ZINK_BINDLESS_IS_BUFFER(handle);
         VkWriteDescriptorSet wd;
         wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         wd.pNext = NULL;
         wd.dstSet = ctx->dd->bindless_set;
         wd.dstBinding = is_buffer ? i * 2 + 1: i * 2;
         wd.dstArrayElement = is_buffer ? handle - ZINK_MAX_BINDLESS_HANDLES : handle;
         wd.descriptorCount = 1;
         wd.descriptorType = type_from_bindless_index(wd.dstBinding);
         if (is_buffer)
            wd.pTexelBufferView = &ctx->di.bindless[i].buffer_infos[wd.dstArrayElement];
         else
            wd.pImageInfo = &ctx->di.bindless[i].img_infos[handle];
         VKSCR(UpdateDescriptorSets)(screen->dev, 1, &wd, 0, NULL);
      }
   }
   ctx->di.any_bindless_dirty = 0;
}
