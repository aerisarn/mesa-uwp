/*
 * Copyright © 2021 Valve Corporation
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
#include "zink_compiler.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_resource.h"
#include "zink_screen.h"

struct zink_descriptor_data {
   VkDescriptorSetLayout push_dsl[2]; //gfx, compute
   VkDescriptorSetLayout dummy_dsl;
   VkDescriptorPool dummy_pool;
   VkDescriptorSet dummy_set;
   VkDescriptorUpdateTemplateEntry push_entries[PIPE_SHADER_TYPES];
   bool push_state_changed[2]; //gfx, compute
   bool state_changed[2]; //gfx, compute
   VkDescriptorSetLayout dsl[2]; //gfx, compute
};

struct zink_program_descriptor_data {
   unsigned num_type_sizes;
   VkDescriptorPoolSize sizes[6];
   unsigned has_descriptors_mask[ZINK_SHADER_COUNT];
   struct zink_descriptor_layout_key *layout_key;
   unsigned push_usage;
   VkDescriptorUpdateTemplateKHR templates[2];
};

struct zink_descriptor_pool {
   VkDescriptorPool pool;
   VkDescriptorSet sets[ZINK_DEFAULT_MAX_DESCS];
   unsigned set_idx;
   unsigned sets_alloc;
};

struct zink_batch_descriptor_data {
   struct hash_table pools;
   struct zink_descriptor_pool *push_pool[2];
   struct zink_program *pg[2]; //gfx, compute
   bool have_descriptor_refs[2]; //gfx, compute
};

static void
init_template_entry(struct zink_shader *shader, enum zink_descriptor_type type,
                    unsigned idx, unsigned offset, VkDescriptorUpdateTemplateEntry *entry, unsigned *entry_idx)
{
    int index = shader->bindings[type][idx].index;
    enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
    entry->dstArrayElement = 0;
    entry->dstBinding = shader->bindings[type][idx].binding;
    if (shader->bindings[type][idx].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
       /* filter out DYNAMIC type here */
       entry->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    else
       entry->descriptorType = shader->bindings[type][idx].type;
    switch (shader->bindings[type][idx].type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
       entry->descriptorCount = 1;
       entry->offset = offsetof(struct zink_context, di.ubos[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorBufferInfo);
       break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.textures[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorImageInfo);
       break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.tbos[stage][index + offset]);
       entry->stride = sizeof(VkBufferView);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
       entry->descriptorCount = 1;
       entry->offset = offsetof(struct zink_context, di.ssbos[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorBufferInfo);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.images[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorImageInfo);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.texel_images[stage][index + offset]);
       entry->stride = sizeof(VkBufferView);
       break;
    default:
       unreachable("unknown type");
    }
    (*entry_idx)++;
}

bool
zink_descriptor_program_init_lazy(struct zink_context *ctx, struct zink_program *pg)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[ZINK_DESCRIPTOR_TYPES * PIPE_SHADER_TYPES * 32];
   VkDescriptorUpdateTemplateEntry entries[ZINK_DESCRIPTOR_TYPES * PIPE_SHADER_TYPES * 32];
   unsigned num_bindings = 0;

   int type_map[12];
   unsigned num_types = 0;
   memset(type_map, -1, sizeof(type_map));

   struct zink_shader **stages;
   if (pg->is_compute)
      stages = &((struct zink_compute_program*)pg)->shader;
   else
      stages = ((struct zink_gfx_program*)pg)->shaders;


   if (!pg->dd)
      pg->dd = rzalloc(pg, struct zink_program_descriptor_data);
   if (!pg->dd)
      return false;

   unsigned push_count = 0;
   unsigned entry_idx = 0;

   unsigned num_shaders = pg->is_compute ? 1 : ZINK_SHADER_COUNT;
   bool have_push = screen->info.have_KHR_push_descriptor;
   for (int i = 0; i < num_shaders; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;

      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
      VkShaderStageFlagBits stage_flags = zink_shader_stage(stage);
      for (int j = 0; j < ZINK_DESCRIPTOR_TYPES; j++) {
         for (int k = 0; k < shader->num_bindings[j]; k++) {
            pg->dd->has_descriptors_mask[stage] |= BITFIELD64_BIT(j);
            /* dynamic ubos handled in push */
            if (shader->bindings[j][k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
               pg->dd->push_usage |= BITFIELD64_BIT(stage);

               push_count++;
               continue;
            }

            assert(num_bindings < ARRAY_SIZE(bindings));
            bindings[num_bindings].binding = shader->bindings[j][k].binding;
            bindings[num_bindings].descriptorType = shader->bindings[j][k].type;
            bindings[num_bindings].descriptorCount = shader->bindings[j][k].size;
            bindings[num_bindings].stageFlags = stage_flags;
            bindings[num_bindings].pImmutableSamplers = NULL;
            if (type_map[shader->bindings[j][k].type] == -1) {
               type_map[shader->bindings[j][k].type] = num_types++;
               pg->dd->sizes[type_map[shader->bindings[j][k].type]].type = shader->bindings[j][k].type;
            }
            pg->dd->sizes[type_map[shader->bindings[j][k].type]].descriptorCount += shader->bindings[j][k].size;
            switch (shader->bindings[j][k].type) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
               init_template_entry(shader, j, k, 0, &entries[entry_idx], &entry_idx);
               break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
               init_template_entry(shader, j, k, 0, &entries[entry_idx], &entry_idx);
               break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
               init_template_entry(shader, j, k, 0, &entries[entry_idx], &entry_idx);
               break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               init_template_entry(shader, j, k, 0, &entries[entry_idx], &entry_idx);
               break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
               for (unsigned l = 0; l < shader->bindings[j][k].size; l++)
                  init_template_entry(shader, j, k, l, &entries[entry_idx], &entry_idx);
               break;
            default:
               break;
            }
            ++num_bindings;
         }
      }
   }

   if (!num_bindings && !push_count) {
      ralloc_free(pg->dd);
      pg->dd = NULL;

      pg->layout = zink_pipeline_layout_create(screen, pg);
      return !!pg->layout;
   }

   pg->num_dsl = 1;
   if (num_bindings) {
      pg->dsl[0] = zink_descriptor_util_layout_get(ctx, 0, bindings, num_bindings, &pg->dd->layout_key);
      pg->dd->num_type_sizes = num_types;
      for (unsigned i = 0; i < num_types; i++)
         pg->dd->sizes[i].descriptorCount *= ZINK_DEFAULT_MAX_DESCS;
   } else
      pg->dsl[0] = ctx->dd->dummy_dsl;

   if (push_count) {
      pg->dsl[1] = ctx->dd->push_dsl[pg->is_compute];
      pg->num_dsl++;
   }

   pg->layout = zink_pipeline_layout_create(screen, pg);
   if (!pg->layout)
      return false;

   if (!num_bindings && !push_count)
      return true;

   VkDescriptorUpdateTemplateCreateInfo template[2] = {};
   VkDescriptorUpdateTemplateType types[2] = {
      VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
      have_push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET
   };
   unsigned wd_count[2] = {
      pg->dd->layout_key ? pg->dd->layout_key->num_descriptors : 0,
      pg->is_compute ? 1 : ZINK_SHADER_COUNT
   };
   VkDescriptorUpdateTemplateEntry *push_entries[2] = {
      ctx->dd->push_entries,
      &ctx->dd->push_entries[PIPE_SHADER_COMPUTE],
   };
   for (unsigned i = !num_bindings; i < 1 + !!push_count; i++) {
      template[i].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
      template[i].descriptorUpdateEntryCount = wd_count[i];
      template[i].pDescriptorUpdateEntries = i ? push_entries[pg->is_compute] : entries;
      template[i].templateType = types[i];
      template[i].descriptorSetLayout = pg->dsl[i];
      template[i].pipelineBindPoint = pg->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
      template[i].pipelineLayout = pg->layout;
      template[i].set = i;
      if (screen->vk.CreateDescriptorUpdateTemplate(screen->dev, &template[i], NULL, &pg->dd->templates[i]) != VK_SUCCESS)
         return false;
   }
   return true;
}

void
zink_descriptor_program_deinit_lazy(struct zink_screen *screen, struct zink_program *pg)
{
   if (!pg->dd)
      return;
   for (unsigned i = 0; i < 1 + !!pg->dd->push_usage; i++) {
      if (pg->dd->templates[i])
         screen->vk.DestroyDescriptorUpdateTemplate(screen->dev, pg->dd->templates[i], NULL);
   }
   ralloc_free(pg->dd);
}

static VkDescriptorPool
create_pool(struct zink_screen *screen, unsigned num_type_sizes, VkDescriptorPoolSize *sizes, unsigned flags)
{
   VkDescriptorPool pool;
   VkDescriptorPoolCreateInfo dpci = {};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = num_type_sizes;
   dpci.flags = flags;
   dpci.maxSets = ZINK_DEFAULT_MAX_DESCS;
   if (vkCreateDescriptorPool(screen->dev, &dpci, 0, &pool) != VK_SUCCESS) {
      debug_printf("vkCreateDescriptorPool failed\n");
      return VK_NULL_HANDLE;
   }
   return pool;
}

static struct zink_descriptor_pool *
get_descriptor_pool_lazy(struct zink_context *ctx, struct zink_program *pg, struct zink_batch_state *bs)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct hash_entry *he = _mesa_hash_table_search(&bs->dd->pools, pg->dd->layout_key);
   if (he)
      return he->data;
   struct zink_descriptor_pool *pool = rzalloc(bs, struct zink_descriptor_pool);
   if (!pool)
      return NULL;

   pool->pool = create_pool(screen, pg->dd->num_type_sizes, pg->dd->sizes, 0);
   if (!pool->pool) {
      ralloc_free(pool);
      return NULL;
   }
   _mesa_hash_table_insert(&bs->dd->pools, pg->dd->layout_key, pool);
   return pool;
}

static VkDescriptorSet
get_descriptor_set_lazy(struct zink_context *ctx, struct zink_program *pg, struct zink_descriptor_pool *pool, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (!pool)
      return VK_NULL_HANDLE;

   if (pool->set_idx < pool->sets_alloc)
      return pool->sets[pool->set_idx++];

   /* allocate up to $current * 10, e.g., 10 -> 100 or 100 -> 1000 */
   unsigned sets_to_alloc = MIN2(MAX2(pool->sets_alloc * 10, 10), ZINK_DEFAULT_MAX_DESCS) - pool->sets_alloc;
   if (!sets_to_alloc) {//pool full
      zink_fence_wait(&ctx->base);
      return get_descriptor_set_lazy(ctx, pg, pool, is_compute);
   }
   if (!zink_descriptor_util_alloc_sets(screen, pg ? pg->dsl[0] : ctx->dd->push_dsl[is_compute],
                                        pool->pool, &pool->sets[pool->sets_alloc], sets_to_alloc))
      return VK_NULL_HANDLE;
   pool->sets_alloc += sets_to_alloc;
   return pool->sets[pool->set_idx++];
}

void
zink_descriptors_update_lazy(struct zink_context *ctx, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch *batch = &ctx->batch;
   struct zink_batch_state *bs = ctx->batch.state;
   struct zink_program *pg = is_compute ? &ctx->curr_compute->base : &ctx->curr_program->base;

   bool batch_changed = bs->dd->pg[is_compute] != pg;
   bool dsl_changed = ctx->dd->dsl[is_compute] != pg->dsl[0];
   /* program change on same batch guarantees descriptor refs */
   if (dsl_changed && !batch_changed)
      bs->dd->have_descriptor_refs[is_compute] = true;

   if (pg->dd->layout_key &&
       (ctx->dd->state_changed[is_compute] || batch_changed)) {
      struct zink_descriptor_pool *pool = get_descriptor_pool_lazy(ctx, pg, bs);
      VkDescriptorSet desc_set = get_descriptor_set_lazy(ctx, pg, pool, is_compute);
      /* may have flushed */
      bs = ctx->batch.state;
      batch_changed |= bs->dd->pg[is_compute] != pg;

      assert(pg->dd->layout_key->num_descriptors);
      screen->vk.UpdateDescriptorSetWithTemplate(screen->dev, desc_set, pg->dd->templates[0], ctx);
      if (pg->dd->layout_key)
         vkCmdBindDescriptorSets(batch->state->cmdbuf,
                                 is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pg->layout, 0, 1, &desc_set,
                                 0, NULL);
   }

   if (pg->dd->push_usage &&
       (ctx->dd->push_state_changed[is_compute] || batch_changed)) {
      if (!pg->dd->layout_key) {
         vkCmdBindDescriptorSets(batch->state->cmdbuf,
                                 is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pg->layout, 0, 1, &ctx->dd->dummy_set,
                                 0, NULL);
      }
      if (screen->info.have_KHR_push_descriptor)
         screen->vk.CmdPushDescriptorSetWithTemplateKHR(batch->state->cmdbuf, pg->dd->templates[1],
                                                     pg->layout, 1, ctx);
      else {
         struct zink_descriptor_pool *pool = bs->dd->push_pool[is_compute];
         VkDescriptorSet desc_set = get_descriptor_set_lazy(ctx, NULL, pool, is_compute);
         bs = ctx->batch.state;
         screen->vk.UpdateDescriptorSetWithTemplate(screen->dev, desc_set, pg->dd->templates[1], ctx);
         vkCmdBindDescriptorSets(batch->state->cmdbuf,
                                 is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pg->layout, 1, 1, &desc_set,
                                 0, NULL);
      }
      ctx->dd->push_state_changed[is_compute] = false;
   }
   bs->dd->have_descriptor_refs[is_compute] = true;
   bs->dd->pg[is_compute] = pg;
   ctx->dd->dsl[is_compute] = pg->dsl[0];
}

void
zink_context_invalidate_descriptor_state_lazy(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned start, unsigned count)
{
   if (type == ZINK_DESCRIPTOR_TYPE_UBO && !start)
      ctx->dd->push_state_changed[shader == PIPE_SHADER_COMPUTE] = true;
   else
      ctx->dd->state_changed[shader == PIPE_SHADER_COMPUTE] = true;
}

void
zink_batch_descriptor_deinit_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs->dd)
      return;
   hash_table_foreach(&bs->dd->pools, entry) {
      struct zink_descriptor_pool *pool = (void*)entry->data;
      vkDestroyDescriptorPool(screen->dev, pool->pool, NULL);
   }
   if (bs->dd->push_pool[0])
      vkDestroyDescriptorPool(screen->dev, bs->dd->push_pool[0]->pool, NULL);
   if (bs->dd->push_pool[1])
      vkDestroyDescriptorPool(screen->dev, bs->dd->push_pool[1]->pool, NULL);
   ralloc_free(bs->dd);
}

void
zink_batch_descriptor_reset_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   hash_table_foreach(&bs->dd->pools, entry) {
      struct zink_descriptor_pool *pool = (void*)entry->data;
      pool->set_idx = 0;
   }
   for (unsigned i = 0; i < 2; i++) {
      bs->dd->pg[i] = NULL;
      bs->dd->have_descriptor_refs[i] = false;
      if (bs->dd->push_pool[i])
         bs->dd->push_pool[i]->set_idx = 0;
   }
}

bool
zink_batch_descriptor_init_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   bs->dd = rzalloc(bs, struct zink_batch_descriptor_data);
   if (!bs->dd)
      return false;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!_mesa_hash_table_init(&bs->dd->pools, bs->dd, _mesa_hash_pointer, _mesa_key_pointer_equal))
         return false;
   }
   if (!screen->info.have_KHR_push_descriptor) {
      VkDescriptorPoolSize sizes;
      sizes.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes.descriptorCount  = ZINK_SHADER_COUNT * ZINK_DEFAULT_MAX_DESCS;
      bs->dd->push_pool[0] = rzalloc(bs, struct zink_descriptor_pool);
      bs->dd->push_pool[0]->pool = create_pool(screen, 1, &sizes, 0);
      sizes.descriptorCount  = ZINK_DEFAULT_MAX_DESCS;
      bs->dd->push_pool[1] = rzalloc(bs, struct zink_descriptor_pool);
      bs->dd->push_pool[1]->pool = create_pool(screen, 1, &sizes, 0);
   }
   return true;
}

bool
zink_descriptors_init_lazy(struct zink_context *ctx)
{
   ctx->dd = rzalloc(ctx, struct zink_descriptor_data);
   if (!ctx->dd)
      return false;

   VkDescriptorSetLayoutBinding bindings[PIPE_SHADER_TYPES];
   for (unsigned i = 0; i < PIPE_SHADER_TYPES; i++) {
      VkDescriptorUpdateTemplateEntry *entry = &ctx->dd->push_entries[i];
      entry->dstBinding = tgsi_processor_to_shader_stage(i);
      entry->descriptorCount = 1;
      entry->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      entry->offset = offsetof(struct zink_context, di.ubos[i][0]);
      entry->stride = sizeof(VkDescriptorBufferInfo);

      bindings[i].binding = tgsi_processor_to_shader_stage(i);
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = zink_shader_stage(i);
      bindings[i].pImmutableSamplers = NULL;
   }
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_descriptor_layout_key *layout_key;
   bool have_push = screen->info.have_KHR_push_descriptor;
   ctx->dd->push_dsl[0] = zink_descriptor_util_layout_get(ctx, have_push, bindings, ZINK_SHADER_COUNT, &layout_key);
   ctx->dd->push_dsl[1] = zink_descriptor_util_layout_get(ctx, have_push, &bindings[PIPE_SHADER_COMPUTE], 1, &layout_key);
   if (!ctx->dd->push_dsl[0] || !ctx->dd->push_dsl[1])
      return false;

   ctx->dd->dummy_dsl = zink_descriptor_util_layout_get(ctx, 2, bindings, 1, &layout_key);
   VkDescriptorPoolSize null_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   ctx->dd->dummy_pool = create_pool(screen, 1, &null_size, 0);
   zink_descriptor_util_alloc_sets(screen, ctx->dd->dummy_dsl,
                                   ctx->dd->dummy_pool, &ctx->dd->dummy_set, 1);
   VkDescriptorBufferInfo push_info;
   VkWriteDescriptorSet push_wd;
   push_wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   push_wd.pNext = NULL;
   push_wd.dstBinding = 0;
   push_wd.dstArrayElement = 0;
   push_wd.descriptorCount = 1;
   push_wd.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   push_wd.dstSet = ctx->dd->dummy_set;
   push_wd.pBufferInfo = &push_info;
   push_info.buffer = screen->info.rb2_feats.nullDescriptor ?
                      VK_NULL_HANDLE :
                      zink_resource(ctx->dummy_vertex_buffer)->obj->buffer;
   push_info.offset = 0;
   push_info.range = VK_WHOLE_SIZE;
   vkUpdateDescriptorSets(screen->dev, 1, &push_wd, 0, NULL);

   return !!ctx->dd->dummy_dsl;
}

void
zink_descriptors_deinit_lazy(struct zink_context *ctx)
{
   if (ctx->dd && ctx->dd->dummy_pool)
      vkDestroyDescriptorPool(zink_screen(ctx->base.screen)->dev, ctx->dd->dummy_pool, NULL);
   ralloc_free(ctx->dd);
}
