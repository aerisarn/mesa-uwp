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
#include "zink_resource.h"
#include "zink_screen.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"


struct zink_descriptor_pool {
   struct pipe_reference reference;
   enum zink_descriptor_type type;
   struct hash_table *desc_sets;
   struct hash_table *free_desc_sets;
   struct util_dynarray alloc_desc_sets;
   VkDescriptorPool descpool;
   struct zink_descriptor_pool_key key;
   unsigned num_resources;
   unsigned num_sets_allocated;
   simple_mtx_t mtx;
};

struct zink_descriptor_set {
   struct zink_descriptor_pool *pool;
   struct pipe_reference reference; //incremented for batch usage
   VkDescriptorSet desc_set;
   uint32_t hash;
   bool invalid;
   bool punted;
   bool recycled;
   struct zink_descriptor_state_key key;
   struct zink_batch_usage batch_uses;
#ifndef NDEBUG
   /* for extra debug asserts */
   unsigned num_resources;
#endif
   union {
      struct zink_resource_object **res_objs;
      struct zink_image_view **image_views;
      struct {
         struct zink_sampler_view **sampler_views;
         struct zink_sampler_state **sampler_states;
      };
   };
};


struct zink_descriptor_data {
   struct zink_descriptor_state gfx_descriptor_states[ZINK_SHADER_COUNT]; // keep incremental hashes here
   struct zink_descriptor_state descriptor_states[2]; // gfx, compute
   struct hash_table *descriptor_pools[ZINK_DESCRIPTOR_TYPES];

   struct zink_descriptor_pool *push_pool[2]; //gfx, compute
   VkDescriptorSetLayout push_dsl[2]; //gfx, compute
   uint8_t last_push_usage[2];
   bool push_valid[2];
   uint32_t push_state[2];
   bool gfx_push_valid[ZINK_SHADER_COUNT];
   uint32_t gfx_push_state[ZINK_SHADER_COUNT];
   struct zink_descriptor_set *last_set[2];

   struct zink_descriptor_pool *dummy_pool;
   VkDescriptorSetLayout dummy_dsl;
   VkDescriptorSet dummy_set;
};

struct zink_program_descriptor_data {
   struct zink_descriptor_pool *pool[ZINK_DESCRIPTOR_TYPES];
   struct zink_descriptor_set *last_set[ZINK_DESCRIPTOR_TYPES];
   uint8_t push_usage;
};

struct zink_batch_descriptor_data {
   struct set *desc_sets;
};

static bool
batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds)
{
   if (!batch_ptr_add_usage(batch, batch->state->dd->desc_sets, zds, &zds->batch_uses))
      return false;
   pipe_reference(NULL, &zds->reference);
   return true;
}

static void
debug_describe_zink_descriptor_pool(char *buf, const struct zink_descriptor_pool *ptr)
{
   sprintf(buf, "zink_descriptor_pool");
}

static bool
desc_state_equal(const void *a, const void *b)
{
   const struct zink_descriptor_state_key *a_k = (void*)a;
   const struct zink_descriptor_state_key *b_k = (void*)b;

   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
      if (a_k->exists[i] != b_k->exists[i])
         return false;
      if (a_k->exists[i] && b_k->exists[i] &&
          a_k->state[i] != b_k->state[i])
         return false;
   }
   return true;
}

static uint32_t
desc_state_hash(const void *key)
{
   const struct zink_descriptor_state_key *d_key = (void*)key;
   uint32_t hash = 0;
   bool first = true;
   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
      if (d_key->exists[i]) {
         if (!first)
            hash = XXH32(&d_key->state[i], sizeof(uint32_t), hash);
         else
            hash = d_key->state[i];
         first = false;
      }
   }
   return hash;
}

static void
pop_desc_set_ref(struct zink_descriptor_set *zds, struct util_dynarray *refs)
{
   size_t size = sizeof(struct zink_descriptor_reference);
   unsigned num_elements = refs->size / size;
   for (unsigned i = 0; i < num_elements; i++) {
      struct zink_descriptor_reference *ref = util_dynarray_element(refs, struct zink_descriptor_reference, i);
      if (&zds->invalid == ref->invalid) {
         memcpy(util_dynarray_element(refs, struct zink_descriptor_reference, i),
                util_dynarray_pop_ptr(refs, struct zink_descriptor_reference), size);
         break;
      }
   }
}

static void
descriptor_set_invalidate(struct zink_descriptor_set *zds)
{
   zds->invalid = true;
   for (unsigned i = 0; i < zds->pool->key.layout->num_descriptors; i++) {
      switch (zds->pool->type) {
      case ZINK_DESCRIPTOR_TYPE_UBO:
      case ZINK_DESCRIPTOR_TYPE_SSBO:
         if (zds->res_objs[i])
            pop_desc_set_ref(zds, &zds->res_objs[i]->desc_set_refs.refs);
         zds->res_objs[i] = NULL;
         break;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         if (zds->image_views[i])
            pop_desc_set_ref(zds, &zds->image_views[i]->desc_set_refs.refs);
         zds->image_views[i] = NULL;
         break;
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         if (zds->sampler_views[i])
            pop_desc_set_ref(zds, &zds->sampler_views[i]->desc_set_refs.refs);
         zds->sampler_views[i] = NULL;
         if (zds->sampler_states[i])
            pop_desc_set_ref(zds, &zds->sampler_states[i]->desc_set_refs.refs);
         zds->sampler_states[i] = NULL;
         break;
      default:
         break;
      }
   }
}

#ifndef NDEBUG
static void
descriptor_pool_clear(struct hash_table *ht)
{
   _mesa_hash_table_clear(ht, NULL);
}
#endif

static void
descriptor_pool_free(struct zink_screen *screen, struct zink_descriptor_pool *pool)
{
   if (!pool)
      return;
   if (pool->descpool)
      vkDestroyDescriptorPool(screen->dev, pool->descpool, NULL);

   simple_mtx_lock(&pool->mtx);
#ifndef NDEBUG
   if (pool->desc_sets)
      descriptor_pool_clear(pool->desc_sets);
   if (pool->free_desc_sets)
      descriptor_pool_clear(pool->free_desc_sets);
#endif
   if (pool->desc_sets)
      _mesa_hash_table_destroy(pool->desc_sets, NULL);
   if (pool->free_desc_sets)
      _mesa_hash_table_destroy(pool->free_desc_sets, NULL);

   simple_mtx_unlock(&pool->mtx);
   util_dynarray_fini(&pool->alloc_desc_sets);
   simple_mtx_destroy(&pool->mtx);
   ralloc_free(pool);
}

static struct zink_descriptor_pool *
descriptor_pool_create(struct zink_screen *screen, enum zink_descriptor_type type,
                       struct zink_descriptor_layout_key *layout_key, VkDescriptorPoolSize *sizes, unsigned num_type_sizes)
{
   struct zink_descriptor_pool *pool = rzalloc(NULL, struct zink_descriptor_pool);
   if (!pool)
      return NULL;
   pipe_reference_init(&pool->reference, 1);
   pool->type = type;
   pool->key.layout = layout_key;
   pool->key.num_type_sizes = num_type_sizes;
   size_t types_size = num_type_sizes * sizeof(VkDescriptorPoolSize);
   pool->key.sizes = ralloc_size(pool, types_size);
   if (!pool->key.sizes) {
      ralloc_free(pool);
      return NULL;
   }
   memcpy(pool->key.sizes, sizes, types_size);
   simple_mtx_init(&pool->mtx, mtx_plain);
   for (unsigned i = 0; i < layout_key->num_descriptors; i++) {
       pool->num_resources += layout_key->bindings[i].descriptorCount;
   }
   pool->desc_sets = _mesa_hash_table_create(NULL, desc_state_hash, desc_state_equal);
   if (!pool->desc_sets)
      goto fail;

   pool->free_desc_sets = _mesa_hash_table_create(NULL, desc_state_hash, desc_state_equal);
   if (!pool->free_desc_sets)
      goto fail;

   util_dynarray_init(&pool->alloc_desc_sets, NULL);

   VkDescriptorPoolCreateInfo dpci = {0};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = num_type_sizes;
   dpci.flags = 0;
   dpci.maxSets = ZINK_DEFAULT_MAX_DESCS;
   if (vkCreateDescriptorPool(screen->dev, &dpci, 0, &pool->descpool) != VK_SUCCESS) {
      debug_printf("vkCreateDescriptorPool failed\n");
      goto fail;
   }

   return pool;
fail:
   descriptor_pool_free(screen, pool);
   return NULL;
}

static VkDescriptorSetLayout
descriptor_layout_create(struct zink_screen *screen, enum zink_descriptor_type t, VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings)
{
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dcslci = {0};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {0};
   VkDescriptorBindingFlags flags[num_bindings];
   if (screen->lazy_descriptors) {
      /* FIXME */
      dcslci.pNext = &fci;
      if (t == ZINK_DESCRIPTOR_TYPES)
         dcslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      fci.bindingCount = num_bindings;
      fci.pBindingFlags = flags;
      for (unsigned i = 0; i < num_bindings; i++) {
         flags[i] = 0;
      }
   }
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;
   VkDescriptorSetLayoutSupport supp;
   supp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
   supp.pNext = NULL;
   supp.supported = VK_FALSE;
   if (screen->vk.GetDescriptorSetLayoutSupport) {
      screen->vk.GetDescriptorSetLayoutSupport(screen->dev, &dcslci, &supp);
      if (supp.supported == VK_FALSE) {
         debug_printf("vkGetDescriptorSetLayoutSupport claims layout is unsupported\n");
         return VK_NULL_HANDLE;
      }
   }
   if (vkCreateDescriptorSetLayout(screen->dev, &dcslci, 0, &dsl) != VK_SUCCESS)
      debug_printf("vkCreateDescriptorSetLayout failed\n");
   return dsl;
}

static uint32_t
hash_descriptor_layout(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_layout_key *k = key;
   hash = XXH32(&k->num_descriptors, sizeof(unsigned), hash);
   hash = XXH32(k->bindings, k->num_descriptors * sizeof(VkDescriptorSetLayoutBinding), hash);

   return hash;
}

static bool
equals_descriptor_layout(const void *a, const void *b)
{
   const struct zink_descriptor_layout_key *a_k = a;
   const struct zink_descriptor_layout_key *b_k = b;
   return a_k->num_descriptors == b_k->num_descriptors &&
          !memcmp(a_k->bindings, b_k->bindings, a_k->num_descriptors * sizeof(VkDescriptorSetLayoutBinding));
}

VkDescriptorSetLayout
zink_descriptor_util_layout_get(struct zink_context *ctx, enum zink_descriptor_type type,
                      VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
                      struct zink_descriptor_layout_key **layout_key)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   uint32_t hash = 0;
   struct zink_descriptor_layout_key key = {
      .num_descriptors = num_bindings,
      .bindings = bindings,
   };

   VkDescriptorSetLayoutBinding null_binding;
   if (!bindings) {
      null_binding.binding = 0;
      null_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      null_binding.descriptorCount = 1;
      null_binding.pImmutableSamplers = NULL;
      null_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
      key.bindings = &null_binding;
   }

   if (type != ZINK_DESCRIPTOR_TYPES) {
      hash = hash_descriptor_layout(&key);
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(&ctx->desc_set_layouts[type], hash, &key);
      if (he) {
         *layout_key = (void*)he->key;
#if VK_USE_64_BIT_PTR_DEFINES == 1
         return (VkDescriptorSetLayout)he->data;
#else
         return *((VkDescriptorSetLayout*)he->data);
#endif
      }
   }

   VkDescriptorSetLayout dsl = descriptor_layout_create(screen, type, key.bindings, MAX2(num_bindings, 1));
   if (!dsl)
      return VK_NULL_HANDLE;

   struct zink_descriptor_layout_key *k = ralloc(ctx, struct zink_descriptor_layout_key);
   k->num_descriptors = num_bindings;
   size_t bindings_size = MAX2(num_bindings, 1) * sizeof(VkDescriptorSetLayoutBinding);
   k->bindings = ralloc_size(k, bindings_size);
   if (!k->bindings) {
      ralloc_free(k);
      vkDestroyDescriptorSetLayout(screen->dev, dsl, NULL);
      return VK_NULL_HANDLE;
   }
   memcpy(k->bindings, key.bindings, bindings_size);

   if (type != ZINK_DESCRIPTOR_TYPES) {
#if VK_USE_64_BIT_PTR_DEFINES == 1
      _mesa_hash_table_insert_pre_hashed(&ctx->desc_set_layouts[type], hash, k, dsl);
#else
      {
         VkDescriptorSetLayout *dsl_p = ralloc(NULL, VkDescriptorSetLayout);
         *dsl_p = dsl;
         _mesa_hash_table_insert_pre_hashed(&ctx->desc_set_layouts[type], hash, k, dsl_p);
      }
#endif
   }
   *layout_key = k;
   return dsl;
}

bool
zink_descriptor_util_push_layouts_get(struct zink_context *ctx, VkDescriptorSetLayout *dsls, struct zink_descriptor_layout_key **layout_keys)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[PIPE_SHADER_TYPES];
   for (unsigned i = 0; i < PIPE_SHADER_TYPES; i++) {
      bindings[i].binding = tgsi_processor_to_shader_stage(i);
      bindings[i].descriptorType = screen->lazy_descriptors ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = zink_shader_stage(i);
      bindings[i].pImmutableSamplers = NULL;
   }
   enum zink_descriptor_type dsl_type = screen->lazy_descriptors ? ZINK_DESCRIPTOR_TYPES : ZINK_DESCRIPTOR_TYPE_UBO;
   dsls[0] = zink_descriptor_util_layout_get(ctx, dsl_type, bindings, ZINK_SHADER_COUNT, &layout_keys[0]);
   dsls[1] = zink_descriptor_util_layout_get(ctx, dsl_type, &bindings[PIPE_SHADER_COMPUTE], 1, &layout_keys[1]);
   return dsls[0] && dsls[1];
}

void
zink_descriptor_util_init_null_set(struct zink_context *ctx, VkDescriptorSet desc_set)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorBufferInfo push_info;
   VkWriteDescriptorSet push_wd;
   push_wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   push_wd.pNext = NULL;
   push_wd.dstBinding = 0;
   push_wd.dstArrayElement = 0;
   push_wd.descriptorCount = 1;
   push_wd.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   push_wd.dstSet = desc_set;
   push_wd.pBufferInfo = &push_info;
   push_info.buffer = screen->info.rb2_feats.nullDescriptor ?
                      VK_NULL_HANDLE :
                      zink_resource(ctx->dummy_vertex_buffer)->obj->buffer;
   push_info.offset = 0;
   push_info.range = VK_WHOLE_SIZE;
   vkUpdateDescriptorSets(screen->dev, 1, &push_wd, 0, NULL);
}

static uint32_t
hash_descriptor_pool(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_pool_key *k = key;
   hash = XXH32(&k->num_type_sizes, sizeof(unsigned), hash);
   hash = XXH32(&k->layout, sizeof(k->layout), hash);
   hash = XXH32(k->sizes, k->num_type_sizes * sizeof(VkDescriptorPoolSize), hash);

   return hash;
}

static bool
equals_descriptor_pool(const void *a, const void *b)
{
   const struct zink_descriptor_pool_key *a_k = a;
   const struct zink_descriptor_pool_key *b_k = b;
   return a_k->num_type_sizes == b_k->num_type_sizes &&
          a_k->layout == b_k->layout &&
          !memcmp(a_k->sizes, b_k->sizes, a_k->num_type_sizes * sizeof(VkDescriptorPoolSize));
}

static struct zink_descriptor_pool *
descriptor_pool_get(struct zink_context *ctx, enum zink_descriptor_type type,
                    struct zink_descriptor_layout_key *layout_key, VkDescriptorPoolSize *sizes, unsigned num_type_sizes)
{
   uint32_t hash = 0;
   if (type != ZINK_DESCRIPTOR_TYPES) {
      struct zink_descriptor_pool_key key = {
         .layout = layout_key,
         .num_type_sizes = num_type_sizes,
         .sizes = sizes,
      };

      hash = hash_descriptor_pool(&key);
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(ctx->dd->descriptor_pools[type], hash, &key);
      if (he)
         return (void*)he->data;
   }
   struct zink_descriptor_pool *pool = descriptor_pool_create(zink_screen(ctx->base.screen), type, layout_key, sizes, num_type_sizes);
   if (type != ZINK_DESCRIPTOR_TYPES)
      _mesa_hash_table_insert_pre_hashed(ctx->dd->descriptor_pools[type], hash, &pool->key, pool);
   return pool;
}

static bool
get_invalidated_desc_set(struct zink_descriptor_set *zds)
{
   if (!zds->invalid)
      return false;
   return p_atomic_read(&zds->reference.count) == 1;
}

bool
zink_descriptor_util_alloc_sets(struct zink_screen *screen, VkDescriptorSetLayout dsl, VkDescriptorPool pool, VkDescriptorSet *sets, unsigned num_sets)
{
   VkDescriptorSetAllocateInfo dsai;
   VkDescriptorSetLayout layouts[num_sets];
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = pool;
   dsai.descriptorSetCount = num_sets;
   for (unsigned i = 0; i < num_sets; i ++)
      layouts[i] = dsl;
   dsai.pSetLayouts = layouts;

   if (vkAllocateDescriptorSets(screen->dev, &dsai, sets) != VK_SUCCESS) {
      debug_printf("ZINK: %" PRIu64 " failed to allocate descriptor set :/\n", (uint64_t)dsl);
      return false;
   }
   return true;
}

static struct zink_descriptor_set *
allocate_desc_set(struct zink_context *ctx, struct zink_program *pg, enum zink_descriptor_type type, unsigned descs_used, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   bool push_set = type == ZINK_DESCRIPTOR_TYPES;
   struct zink_descriptor_pool *pool = push_set ? ctx->dd->push_pool[is_compute] : pg->dd->pool[type];
#define DESC_BUCKET_FACTOR 10
   unsigned bucket_size = pool->key.layout->num_descriptors ? DESC_BUCKET_FACTOR : 1;
   if (pool->key.layout->num_descriptors) {
      for (unsigned desc_factor = DESC_BUCKET_FACTOR; desc_factor < descs_used; desc_factor *= DESC_BUCKET_FACTOR)
         bucket_size = desc_factor;
   }
   VkDescriptorSet desc_set[bucket_size];
   if (!zink_descriptor_util_alloc_sets(screen, push_set ? ctx->dd->push_dsl[is_compute] : pg->dsl[type + 1], pool->descpool, desc_set, bucket_size))
      return VK_NULL_HANDLE;

   struct zink_descriptor_set *alloc = ralloc_array(pool, struct zink_descriptor_set, bucket_size);
   assert(alloc);
   unsigned num_resources = pool->num_resources;
   struct zink_resource_object **res_objs = rzalloc_array(pool, struct zink_resource_object*, num_resources * bucket_size);
   assert(res_objs);
   void **samplers = NULL;
   if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
      samplers = rzalloc_array(pool, void*, num_resources * bucket_size);
      assert(samplers);
   }
   for (unsigned i = 0; i < bucket_size; i ++) {
      struct zink_descriptor_set *zds = &alloc[i];
      pipe_reference_init(&zds->reference, 1);
      zds->pool = pool;
      zds->hash = 0;
      zds->batch_uses.usage = 0;
      zds->invalid = true;
      zds->punted = zds->recycled = false;
#ifndef NDEBUG
      zds->num_resources = num_resources;
#endif
      if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
         zds->sampler_views = (struct zink_sampler_view**)&res_objs[i * pool->key.layout->num_descriptors];
         zds->sampler_states = (struct zink_sampler_state**)&samplers[i * pool->key.layout->num_descriptors];
      } else
         zds->res_objs = (struct zink_resource_object**)&res_objs[i * pool->key.layout->num_descriptors];
      zds->desc_set = desc_set[i];
      if (i > 0)
         util_dynarray_append(&pool->alloc_desc_sets, struct zink_descriptor_set *, zds);
   }
   pool->num_sets_allocated += bucket_size;
   return alloc;
}

static void
populate_zds_key(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute,
                 struct zink_descriptor_state_key *key, uint32_t push_usage)
{
   if (is_compute) {
      for (unsigned i = 1; i < ZINK_SHADER_COUNT; i++)
         key->exists[i] = false;
      key->exists[0] = true;
      if (type == ZINK_DESCRIPTOR_TYPES)
         key->state[0] = ctx->dd->push_state[is_compute];
      else
         key->state[0] = ctx->dd->descriptor_states[is_compute].state[type];
   } else if (type == ZINK_DESCRIPTOR_TYPES) {
      /* gfx only */
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         if (push_usage & BITFIELD_BIT(i)) {
            key->exists[i] = true;
            key->state[i] = ctx->dd->gfx_push_state[i];
         } else
            key->exists[i] = false;
      }
   } else {
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         key->exists[i] = ctx->dd->gfx_descriptor_states[i].valid[type];
         key->state[i] = ctx->dd->gfx_descriptor_states[i].state[type];
      }
   }
}

static void
punt_invalid_set(struct zink_descriptor_set *zds, struct hash_entry *he)
{
   /* this is no longer usable, so we punt it for now until it gets recycled */
   assert(!zds->recycled);
   if (!he)
      he = _mesa_hash_table_search_pre_hashed(zds->pool->desc_sets, zds->hash, &zds->key);
   _mesa_hash_table_remove(zds->pool->desc_sets, he);
   zds->punted = true;
}

static struct zink_descriptor_set *
zink_descriptor_set_get(struct zink_context *ctx,
                               enum zink_descriptor_type type,
                               bool is_compute,
                               bool *cache_hit)
{
   *cache_hit = false;
   struct zink_descriptor_set *zds;
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   struct zink_batch *batch = &ctx->batch;
   bool push_set = type == ZINK_DESCRIPTOR_TYPES;
   struct zink_descriptor_pool *pool = push_set ? ctx->dd->push_pool[is_compute] : pg->dd->pool[type];
   unsigned descs_used = 1;
   assert(type <= ZINK_DESCRIPTOR_TYPES);

   assert(pool->key.layout->num_descriptors);
   uint32_t hash = push_set ? ctx->dd->push_state[is_compute] :
                              ctx->dd->descriptor_states[is_compute].state[type];

   struct zink_descriptor_state_key key;
   populate_zds_key(ctx, type, is_compute, &key, pg->dd->push_usage);

   simple_mtx_lock(&pool->mtx);
   struct zink_descriptor_set *last_set = push_set ? ctx->dd->last_set[is_compute] : pg->dd->last_set[type];
   if (last_set && last_set->hash == hash && desc_state_equal(&last_set->key, &key)) {
      zds = last_set;
      *cache_hit = !zds->invalid;
      if (zds->recycled) {
         struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->free_desc_sets, hash, &key);
         if (he)
            _mesa_hash_table_remove(pool->free_desc_sets, he);
         zds->recycled = false;
      }
      if (zds->invalid) {
          if (zink_batch_usage_exists(&zds->batch_uses))
             punt_invalid_set(zds, NULL);
          else
             /* this set is guaranteed to be in pool->alloc_desc_sets */
             goto skip_hash_tables;
          zds = NULL;
      }
      if (zds)
         goto out;
   }

   struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->desc_sets, hash, &key);
   bool recycled = false, punted = false;
   if (he) {
       zds = (void*)he->data;
       if (zds->invalid && zink_batch_usage_exists(&zds->batch_uses)) {
          punt_invalid_set(zds, he);
          zds = NULL;
          punted = true;
       }
   }
   if (!he) {
      he = _mesa_hash_table_search_pre_hashed(pool->free_desc_sets, hash, &key);
      recycled = true;
   }
   if (he && !punted) {
      zds = (void*)he->data;
      *cache_hit = !zds->invalid;
      if (recycled) {
         /* need to migrate this entry back to the in-use hash */
         _mesa_hash_table_remove(pool->free_desc_sets, he);
         goto out;
      }
      goto quick_out;
   }
skip_hash_tables:
   if (util_dynarray_num_elements(&pool->alloc_desc_sets, struct zink_descriptor_set *)) {
      /* grab one off the allocated array */
      zds = util_dynarray_pop(&pool->alloc_desc_sets, struct zink_descriptor_set *);
      goto out;
   }

   if (_mesa_hash_table_num_entries(pool->free_desc_sets)) {
      /* try for an invalidated set first */
      unsigned count = 0;
      hash_table_foreach(pool->free_desc_sets, he) {
         struct zink_descriptor_set *tmp = he->data;
         if ((count++ >= 100 && tmp->reference.count == 1) || get_invalidated_desc_set(he->data)) {
            zds = tmp;
            assert(p_atomic_read(&zds->reference.count) == 1);
            descriptor_set_invalidate(zds);
            _mesa_hash_table_remove(pool->free_desc_sets, he);
            goto out;
         }
      }
   }

   if (pool->num_sets_allocated + pool->key.layout->num_descriptors > ZINK_DEFAULT_MAX_DESCS) {
      simple_mtx_unlock(&pool->mtx);
      zink_fence_wait(&ctx->base);
      zink_batch_reference_program(batch, pg);
      return zink_descriptor_set_get(ctx, type, is_compute, cache_hit);
   }

   zds = allocate_desc_set(ctx, pg, type, descs_used, is_compute);
out:
   zds->hash = hash;
   populate_zds_key(ctx, type, is_compute, &zds->key, pg->dd->push_usage);
   zds->recycled = false;
   _mesa_hash_table_insert_pre_hashed(pool->desc_sets, hash, &zds->key, zds);
quick_out:
   zds->punted = zds->invalid = false;
   if (batch_add_desc_set(batch, zds)) {
      batch->state->descs_used += pool->key.layout->num_descriptors;
   }
   if (push_set)
      ctx->dd->last_set[is_compute] = zds;
   else
      pg->dd->last_set[type] = zds;
   simple_mtx_unlock(&pool->mtx);

   return zds;
}

void
zink_descriptor_set_recycle(struct zink_descriptor_set *zds)
{
   struct zink_descriptor_pool *pool = zds->pool;
   /* if desc set is still in use by a batch, don't recache */
   uint32_t refcount = p_atomic_read(&zds->reference.count);
   if (refcount != 1)
      return;
   /* this is a null set */
   if (!pool->key.layout->num_descriptors)
      return;
   simple_mtx_lock(&pool->mtx);
   if (zds->punted)
      zds->invalid = true;
   else {
      /* if we've previously punted this set, then it won't have a hash or be in either of the tables */
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->desc_sets, zds->hash, &zds->key);
      if (!he) {
         /* desc sets can be used multiple times in the same batch */
         simple_mtx_unlock(&pool->mtx);
         return;
      }
      _mesa_hash_table_remove(pool->desc_sets, he);
   }

   if (zds->invalid) {
      descriptor_set_invalidate(zds);
      util_dynarray_append(&pool->alloc_desc_sets, struct zink_descriptor_set *, zds);
   } else {
      zds->recycled = true;
      _mesa_hash_table_insert_pre_hashed(pool->free_desc_sets, zds->hash, &zds->key, zds);
   }
   simple_mtx_unlock(&pool->mtx);
}


static void
desc_set_ref_add(struct zink_descriptor_set *zds, struct zink_descriptor_refs *refs, void **ref_ptr, void *ptr)
{
   struct zink_descriptor_reference ref = {ref_ptr, &zds->invalid};
   *ref_ptr = ptr;
   if (ptr)
      util_dynarray_append(&refs->refs, struct zink_descriptor_reference, ref);
}

static void
zink_image_view_desc_set_add(struct zink_image_view *image_view, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &image_view->desc_set_refs, (void**)&zds->image_views[idx], image_view);
}

static void
zink_sampler_state_desc_set_add(struct zink_sampler_state *sampler_state, struct zink_descriptor_set *zds, unsigned idx)
{
   if (sampler_state)
      desc_set_ref_add(zds, &sampler_state->desc_set_refs, (void**)&zds->sampler_states[idx], sampler_state);
   else
      zds->sampler_states[idx] = NULL;
}

static void
zink_sampler_view_desc_set_add(struct zink_sampler_view *sampler_view, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &sampler_view->desc_set_refs, (void**)&zds->sampler_views[idx], sampler_view);
}

static void
zink_resource_desc_set_add(struct zink_resource *res, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, res ? &res->obj->desc_set_refs : NULL, (void**)&zds->res_objs[idx], res ? res->obj : NULL);
}

void
zink_descriptor_set_refs_clear(struct zink_descriptor_refs *refs, void *ptr)
{
   util_dynarray_foreach(&refs->refs, struct zink_descriptor_reference, ref) {
      if (*ref->ref == ptr) {
         *ref->invalid = true;
         *ref->ref = NULL;
      }
   }
   util_dynarray_fini(&refs->refs);
}

static inline void
zink_descriptor_pool_reference(struct zink_screen *screen,
                               struct zink_descriptor_pool **dst,
                               struct zink_descriptor_pool *src)
{
   struct zink_descriptor_pool *old_dst = dst ? *dst : NULL;

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL, &src->reference,
                                (debug_reference_descriptor)debug_describe_zink_descriptor_pool))
      descriptor_pool_free(screen, old_dst);
   if (dst) *dst = src;
}

bool
zink_descriptor_program_init(struct zink_context *ctx, struct zink_program *pg)
{
   VkDescriptorSetLayoutBinding bindings[ZINK_DESCRIPTOR_TYPES][PIPE_SHADER_TYPES * 32];
   unsigned num_bindings[ZINK_DESCRIPTOR_TYPES] = {0};
   uint8_t push_usage = 0;

   VkDescriptorPoolSize sizes[6] = {0};
   int type_map[12];
   unsigned num_types = 0;
   memset(type_map, -1, sizeof(type_map));

   struct zink_shader **stages;
   if (pg->is_compute)
      stages = &((struct zink_compute_program*)pg)->shader;
   else
      stages = ((struct zink_gfx_program*)pg)->shaders;

   for (int i = 0; i < (pg->is_compute ? 1 : ZINK_SHADER_COUNT); i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      VkShaderStageFlagBits stage_flags = zink_shader_stage(pipe_shader_type_from_mesa(shader->nir->info.stage));
      for (int j = 0; j < ZINK_DESCRIPTOR_TYPES; j++) {
         for (int k = 0; k < shader->num_bindings[j]; k++) {
            assert(num_bindings[j] < ARRAY_SIZE(bindings[j]));
            if (shader->bindings[j][k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
               push_usage |= BITFIELD_BIT(stage);
               continue;
            }
            bindings[j][num_bindings[j]].binding = shader->bindings[j][k].binding;
            bindings[j][num_bindings[j]].descriptorType = shader->bindings[j][k].type;
            bindings[j][num_bindings[j]].descriptorCount = shader->bindings[j][k].size;
            bindings[j][num_bindings[j]].stageFlags = stage_flags;
            bindings[j][num_bindings[j]].pImmutableSamplers = NULL;
            if (type_map[shader->bindings[j][k].type] == -1) {
               type_map[shader->bindings[j][k].type] = num_types++;
               sizes[type_map[shader->bindings[j][k].type]].type = shader->bindings[j][k].type;
            }
            sizes[type_map[shader->bindings[j][k].type]].descriptorCount += shader->bindings[j][k].size;
            ++num_bindings[j];
         }
      }
   }

   unsigned total_descs = 0;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      total_descs += num_bindings[i];
   }

   if (total_descs || push_usage) {
      pg->dd = rzalloc(pg, struct zink_program_descriptor_data);
      if (!pg->dd)
         return false;

      pg->dd->push_usage = push_usage;
      pg->dsl[pg->num_dsl++] = push_usage ? ctx->dd->push_dsl[pg->is_compute] : ctx->dd->dummy_dsl;
   }
   if (!total_descs) {
      pg->layout = zink_pipeline_layout_create(zink_screen(ctx->base.screen), pg);
      return !!pg->layout;
   }

   for (int i = 0; i < num_types; i++)
      sizes[i].descriptorCount *= ZINK_DEFAULT_MAX_DESCS;

   bool found_descriptors = false;
   struct zink_descriptor_layout_key *layout_key[ZINK_DESCRIPTOR_TYPES] = {0};
   for (unsigned i = ZINK_DESCRIPTOR_TYPES - 1; i < ZINK_DESCRIPTOR_TYPES; i--) {
      if (!num_bindings[i]) {
         if (!found_descriptors)
            continue;
         pg->dsl[i + 1] = ctx->dd->dummy_dsl;
         /* pool is null here for detection during update */
         pg->num_dsl++;
         continue;
      }
      found_descriptors = true;

      VkDescriptorPoolSize type_sizes[2] = {0};
      int num_type_sizes = 0;
      switch (i) {
      case ZINK_DESCRIPTOR_TYPE_UBO:
         if (type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER]];
            num_type_sizes++;
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         if (type_map[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER]];
            num_type_sizes++;
         }
         if (type_map[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]];
            num_type_sizes++;
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_SSBO:
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] != -1) {
            num_type_sizes = 1;
            type_sizes[0] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER]];
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]];
            num_type_sizes++;
         }
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]];
            num_type_sizes++;
         }
         break;
      }
      pg->dsl[i + 1] = zink_descriptor_util_layout_get(ctx, i, bindings[i], num_bindings[i], &layout_key[i]);
      if (!pg->dsl[i + 1])
         return false;
      struct zink_descriptor_pool *pool = descriptor_pool_get(ctx, i, layout_key[i], type_sizes, num_type_sizes);
      if (!pool)
         return false;
      zink_descriptor_pool_reference(zink_screen(ctx->base.screen), &pg->dd->pool[i], pool);
      pg->num_dsl++;
   }

   pg->layout = zink_pipeline_layout_create(zink_screen(ctx->base.screen), pg);
   return !!pg->layout;
}

void
zink_descriptor_program_deinit(struct zink_screen *screen, struct zink_program *pg)
{
   if (!pg->dd)
      return;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++)
      zink_descriptor_pool_reference(screen, &pg->dd->pool[i], NULL);
}

static void
zink_descriptor_pool_deinit(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      hash_table_foreach(ctx->dd->descriptor_pools[i], entry) {
         struct zink_descriptor_pool *pool = (void*)entry->data;
         zink_descriptor_pool_reference(screen, &pool, NULL);
      }
      _mesa_hash_table_destroy(ctx->dd->descriptor_pools[i], NULL);
   }
   zink_descriptor_pool_reference(screen, &ctx->dd->dummy_pool, NULL);
}

static bool
zink_descriptor_pool_init(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      ctx->dd->descriptor_pools[i] = _mesa_hash_table_create(ctx, hash_descriptor_pool, equals_descriptor_pool);
      if (!ctx->dd->descriptor_pools[i])
         return false;
   }
   struct zink_descriptor_layout_key *layout_keys[2];
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (!zink_descriptor_util_push_layouts_get(ctx, ctx->dd->push_dsl, layout_keys))
      return false;
   VkDescriptorPoolSize sizes;
   sizes.type = screen->lazy_descriptors ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
   sizes.descriptorCount = ZINK_SHADER_COUNT * ZINK_DEFAULT_MAX_DESCS;
   ctx->dd->push_pool[0] = descriptor_pool_get(ctx, 0, layout_keys[0], &sizes, 1);
   sizes.descriptorCount = ZINK_DEFAULT_MAX_DESCS;
   ctx->dd->push_pool[1] = descriptor_pool_get(ctx, 0, layout_keys[1], &sizes, 1);
   if (!ctx->dd->push_pool[0] || !ctx->dd->push_pool[1])
      return false;

   ctx->dd->dummy_dsl = zink_descriptor_util_layout_get(ctx, 0, NULL, 0, &layout_keys[0]);
   if (!ctx->dd->dummy_dsl)
      return false;
   VkDescriptorPoolSize null_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   ctx->dd->dummy_pool = descriptor_pool_create(screen, 0, layout_keys[0], &null_size, 1);
   if (!ctx->dd->dummy_pool)
      return false;
   zink_descriptor_util_alloc_sets(screen, ctx->dd->dummy_dsl,
                                   ctx->dd->dummy_pool->descpool, &ctx->dd->dummy_set, 1);
   if (!ctx->dd->dummy_set)
      return false;
   zink_descriptor_util_init_null_set(ctx, ctx->dd->dummy_set);
   return true;
}


static void
desc_set_res_add(struct zink_descriptor_set *zds, struct zink_resource *res, unsigned int i, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
   assert(!cache_hit || zds->res_objs[i] == (res ? res->obj : NULL));
   if (!cache_hit)
      zink_resource_desc_set_add(res, zds, i);
}

static void
desc_set_sampler_add(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_sampler_view *sv,
                     struct zink_sampler_state *state, unsigned int i, bool is_buffer, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
#ifndef NDEBUG
   uint32_t cur_hash = zink_get_sampler_view_hash(ctx, zds->sampler_views[i], is_buffer);
   uint32_t new_hash = zink_get_sampler_view_hash(ctx, sv, is_buffer);
#endif
   assert(!cache_hit || cur_hash == new_hash);
   assert(!cache_hit || zds->sampler_states[i] == state);
   if (!cache_hit) {
      zink_sampler_view_desc_set_add(sv, zds, i);
      zink_sampler_state_desc_set_add(state, zds, i);
   }
}

static void
desc_set_image_add(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_image_view *image_view,
                   unsigned int i, bool is_buffer, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
#ifndef NDEBUG
   uint32_t cur_hash = zink_get_image_view_hash(ctx, zds->image_views[i], is_buffer);
   uint32_t new_hash = zink_get_image_view_hash(ctx, image_view, is_buffer);
#endif
   assert(!cache_hit || cur_hash == new_hash);
   if (!cache_hit)
      zink_image_view_desc_set_add(image_view, zds, i);
}

static int
cmp_dynamic_offset_binding(const void *a, const void *b)
{
   const uint32_t *binding_a = a, *binding_b = b;
   return *binding_a - *binding_b;
}

static void
write_descriptors(struct zink_context *ctx, unsigned num_wds, VkWriteDescriptorSet *wds, bool cache_hit)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   if (!cache_hit && num_wds)
      vkUpdateDescriptorSets(screen->dev, num_wds, wds, 0, NULL);
}

static unsigned
init_write_descriptor(struct zink_shader *shader, struct zink_descriptor_set *zds, enum zink_descriptor_type type, int idx, VkWriteDescriptorSet *wd, unsigned num_wds)
{
    wd->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd->pNext = NULL;
    wd->dstBinding = shader ? shader->bindings[type][idx].binding : idx;
    wd->dstArrayElement = 0;
    wd->descriptorCount = shader ? shader->bindings[type][idx].size : 1;
    wd->descriptorType = shader ? shader->bindings[type][idx].type : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wd->dstSet = zds->desc_set;
    return num_wds + 1;
}

static unsigned
update_push_ubo_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                            bool is_compute, bool cache_hit, uint32_t *dynamic_offsets)
{
   VkWriteDescriptorSet wds[ZINK_SHADER_COUNT];
   VkDescriptorBufferInfo buffer_infos[ZINK_SHADER_COUNT];
   struct zink_shader **stages;
   struct {
      uint32_t binding;
      uint32_t offset;
   } dynamic_buffers[ZINK_SHADER_COUNT];

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      enum pipe_shader_type pstage = shader ? pipe_shader_type_from_mesa(shader->nir->info.stage) : i;
      struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_UBO, pstage, 0);
      VkDescriptorBufferInfo *info = &ctx->di.ubos[pstage][0];

      dynamic_buffers[i].binding = tgsi_processor_to_shader_stage(pstage);
      dynamic_buffers[i].offset = info->offset;
      if (cache_hit)
         continue;
      init_write_descriptor(NULL, zds, ZINK_DESCRIPTOR_TYPE_UBO, tgsi_processor_to_shader_stage(pstage), &wds[i], 0);
      desc_set_res_add(zds, res, i, cache_hit);
      /* these are dynamic UBO descriptors, so we have to always set 0 as the descriptor offset */
      buffer_infos[i] = *info;
      buffer_infos[i].offset = 0;
      wds[i].pBufferInfo = &buffer_infos[i];
   }
   /* Values are taken from pDynamicOffsets in an order such that all entries for set N come before set N+1;
    * within a set, entries are ordered by the binding numbers in the descriptor set layouts
    * - vkCmdBindDescriptorSets spec
    *
    * because of this, we have to sort all the dynamic offsets by their associated binding to ensure they
    * match what the driver expects
    */
   qsort(dynamic_buffers, num_stages, sizeof(uint32_t) * 2, cmp_dynamic_offset_binding);
   for (int i = 0; i < num_stages; i++)
      dynamic_offsets[i] = dynamic_buffers[i].offset;

   write_descriptors(ctx, num_stages, wds, cache_hit);
   return num_stages;
}

static void
update_ubo_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                       bool is_compute, bool cache_hit)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   unsigned num_descriptors = pg->dd->pool[ZINK_DESCRIPTOR_TYPE_UBO]->key.layout->num_descriptors;
   unsigned num_bindings = zds->pool->num_resources;
   VkWriteDescriptorSet wds[num_descriptors];
   unsigned num_wds = 0;
   unsigned num_resources = 0;
   struct zink_shader **stages;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; !cache_hit && i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      for (int j = 0; j < shader->num_bindings[ZINK_DESCRIPTOR_TYPE_UBO]; j++) {
         int index = shader->bindings[ZINK_DESCRIPTOR_TYPE_UBO][j].index;
         VkDescriptorBufferInfo *info = &ctx->di.ubos[stage][index];
         /* skip push descriptors for general ubo set */
         if (!index)
            continue;
         assert(shader->bindings[ZINK_DESCRIPTOR_TYPE_UBO][j].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
         assert(ctx->ubos[stage][index].buffer_size <= screen->info.props.limits.maxUniformBufferRange);
         struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_UBO, stage, index);
         assert(!res || info->range > 0);
         assert(!res || info->buffer);
         assert(num_resources < num_bindings);
         desc_set_res_add(zds, res, num_resources++, cache_hit);
         wds[num_wds].pBufferInfo = info;

         num_wds = init_write_descriptor(shader, zds, ZINK_DESCRIPTOR_TYPE_UBO, j, &wds[num_wds], num_wds);
      }
   }

   write_descriptors(ctx, num_wds, wds, cache_hit);
}

static void
update_ssbo_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                        bool is_compute, bool cache_hit)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   unsigned num_descriptors = pg->dd->pool[ZINK_DESCRIPTOR_TYPE_SSBO]->key.layout->num_descriptors;
   unsigned num_bindings = zds->pool->num_resources;
   VkWriteDescriptorSet wds[num_descriptors];
   unsigned num_wds = 0;
   unsigned num_resources = 0;
   struct zink_shader **stages;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; !cache_hit && i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      for (int j = 0; j < shader->num_bindings[ZINK_DESCRIPTOR_TYPE_SSBO]; j++) {
         int index = shader->bindings[ZINK_DESCRIPTOR_TYPE_SSBO][j].index;
         VkDescriptorBufferInfo *info = &ctx->di.ssbos[stage][index];
         assert(shader->bindings[ZINK_DESCRIPTOR_TYPE_SSBO][j].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
         assert(num_resources < num_bindings);
         struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_SSBO, stage, index);
         desc_set_res_add(zds, res, num_resources++, cache_hit);
         wds[num_wds].pBufferInfo = info;

         num_wds = init_write_descriptor(shader, zds, ZINK_DESCRIPTOR_TYPE_SSBO, j, &wds[num_wds], num_wds);
      }
   }
   write_descriptors(ctx, num_wds, wds, cache_hit);
}

static void
update_sampler_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                           bool is_compute, bool cache_hit)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   unsigned num_descriptors = pg->dd->pool[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW]->key.layout->num_descriptors;
   unsigned num_bindings = zds->pool->num_resources;
   VkWriteDescriptorSet wds[num_descriptors];
   unsigned num_wds = 0;
   unsigned num_resources = 0;
   struct zink_shader **stages;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; !cache_hit && i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      for (int j = 0; j < shader->num_bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW]; j++) {
         int index = shader->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][j].index;
         assert(shader->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][j].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                shader->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][j].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

         for (unsigned k = 0; k < shader->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][j].size; k++) {
            struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, stage, index + k);
            VkDescriptorImageInfo *image_info = &ctx->di.textures[stage][index + k];
            VkBufferView *buffer_info = &ctx->di.tbos[stage][index + k];
            bool is_buffer = zink_shader_descriptor_is_buffer(shader, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, j);
            struct pipe_sampler_view *psampler_view = ctx->sampler_views[stage][index + k];
            struct zink_sampler_view *sampler_view = zink_sampler_view(psampler_view);
            struct zink_sampler_state *sampler = NULL;
            if (!is_buffer && res)
               sampler = ctx->sampler_states[stage][index + k];

            assert(num_resources < num_bindings);
            if (!k) {
               if (is_buffer)
                  wds[num_wds].pTexelBufferView = buffer_info;
               else
                  wds[num_wds].pImageInfo = image_info;
            }
            desc_set_sampler_add(ctx, zds, sampler_view, sampler, num_resources++,
                                 zink_shader_descriptor_is_buffer(shader, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, j),
                                 cache_hit);
         }
         assert(num_wds < num_descriptors);

         num_wds = init_write_descriptor(shader, zds, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, j, &wds[num_wds], num_wds);
      }
   }
   write_descriptors(ctx, num_wds, wds, cache_hit);
}

static void
update_image_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                         bool is_compute, bool cache_hit)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   unsigned num_descriptors = pg->dd->pool[ZINK_DESCRIPTOR_TYPE_IMAGE]->key.layout->num_descriptors;
   unsigned num_bindings = zds->pool->num_resources;
   VkWriteDescriptorSet wds[num_descriptors];
   unsigned num_wds = 0;
   unsigned num_resources = 0;
   struct zink_shader **stages;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; !cache_hit && i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      for (int j = 0; j < shader->num_bindings[ZINK_DESCRIPTOR_TYPE_IMAGE]; j++) {
         int index = shader->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][j].index;
         assert(shader->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][j].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                shader->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][j].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

         for (unsigned k = 0; k < shader->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][j].size; k++) {
            VkDescriptorImageInfo *image_info = &ctx->di.images[stage][index + k];
            VkBufferView *buffer_info = &ctx->di.texel_images[stage][index + k];
            bool is_buffer = zink_shader_descriptor_is_buffer(shader, ZINK_DESCRIPTOR_TYPE_IMAGE, j);
            struct zink_image_view *image_view = &ctx->image_views[stage][index + k];
            assert(num_resources < num_bindings);
            desc_set_image_add(ctx, zds, image_view, num_resources++,
                               zink_shader_descriptor_is_buffer(shader, ZINK_DESCRIPTOR_TYPE_IMAGE, j),
                               cache_hit);

            if (!k) {
               if (is_buffer)
                  wds[num_wds].pTexelBufferView = buffer_info;
               else
                  wds[num_wds].pImageInfo = image_info;
            }
         }
         assert(num_wds < num_descriptors);

         num_wds = init_write_descriptor(shader, zds, ZINK_DESCRIPTOR_TYPE_IMAGE, j, &wds[num_wds], num_wds);
      }
   }
   write_descriptors(ctx, num_wds, wds, cache_hit);
}

static void
zink_context_update_descriptor_states(struct zink_context *ctx, struct zink_program *pg);

void
zink_descriptors_update(struct zink_context *ctx, bool is_compute)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;

   zink_context_update_descriptor_states(ctx, pg);
   bool cache_hit[ZINK_DESCRIPTOR_TYPES + 1];
   VkDescriptorSet sets[ZINK_DESCRIPTOR_TYPES + 1];
   struct zink_descriptor_set *zds[ZINK_DESCRIPTOR_TYPES + 1];
   /* push set is indexed in vulkan as 0 but isn't in the general pool array */
   if (pg->dd->push_usage)
      zds[ZINK_DESCRIPTOR_TYPES] = zink_descriptor_set_get(ctx, ZINK_DESCRIPTOR_TYPES, is_compute, &cache_hit[ZINK_DESCRIPTOR_TYPES]);
   else {
      zds[ZINK_DESCRIPTOR_TYPES] = NULL;
      cache_hit[ZINK_DESCRIPTOR_TYPES] = false;
   }
   sets[0] = zds[ZINK_DESCRIPTOR_TYPES] ? zds[ZINK_DESCRIPTOR_TYPES]->desc_set : ctx->dd->dummy_set;
   for (int h = 0; h < ZINK_DESCRIPTOR_TYPES; h++) {
      if (pg->dsl[h + 1]) {
         /* null set has null pool */
         if (pg->dd->pool[h])
            zds[h] = zink_descriptor_set_get(ctx, h, is_compute, &cache_hit[h]);
         else
            zds[h] = NULL;
         /* reuse dummy set for bind */
         sets[h + 1] = zds[h] ? zds[h]->desc_set : ctx->dd->dummy_set;
      } else {
         zds[h] = NULL;
      }
   }
   struct zink_batch *batch = &ctx->batch;
   zink_batch_reference_program(batch, pg);

   uint32_t dynamic_offsets[PIPE_MAX_CONSTANT_BUFFERS];
   unsigned dynamic_offset_idx = 0;

   if (pg->dd->push_usage) // push set
      dynamic_offset_idx = update_push_ubo_descriptors(ctx, zds[ZINK_DESCRIPTOR_TYPES],
                                                       is_compute, cache_hit[ZINK_DESCRIPTOR_TYPES], dynamic_offsets);

   if (zds[ZINK_DESCRIPTOR_TYPE_UBO])
      update_ubo_descriptors(ctx, zds[ZINK_DESCRIPTOR_TYPE_UBO],
                                           is_compute, cache_hit[ZINK_DESCRIPTOR_TYPE_UBO]);
   if (zds[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW])
      update_sampler_descriptors(ctx, zds[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW],
                                               is_compute, cache_hit[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW]);
   if (zds[ZINK_DESCRIPTOR_TYPE_SSBO])
      update_ssbo_descriptors(ctx, zds[ZINK_DESCRIPTOR_TYPE_SSBO],
                                               is_compute, cache_hit[ZINK_DESCRIPTOR_TYPE_SSBO]);
   if (zds[ZINK_DESCRIPTOR_TYPE_IMAGE])
      update_image_descriptors(ctx, zds[ZINK_DESCRIPTOR_TYPE_IMAGE],
                                               is_compute, cache_hit[ZINK_DESCRIPTOR_TYPE_IMAGE]);

   vkCmdBindDescriptorSets(batch->state->cmdbuf, is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                           pg->layout, 0, pg->num_dsl, sets,
                           dynamic_offset_idx, dynamic_offsets);
}

void
zink_batch_descriptor_deinit(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs->dd)
      return;
   _mesa_set_destroy(bs->dd->desc_sets, NULL);
   ralloc_free(bs->dd);
}

void
zink_batch_descriptor_reset(struct zink_screen *screen, struct zink_batch_state *bs)
{
   set_foreach(bs->dd->desc_sets, entry) {
      struct zink_descriptor_set *zds = (void*)entry->key;
      zink_batch_usage_unset(&zds->batch_uses, bs->fence.batch_id);
      /* reset descriptor pools when no bs is using this program to avoid
       * having some inactive program hogging a billion descriptors
       */
      pipe_reference(&zds->reference, NULL);
      zink_descriptor_set_recycle(zds);
      _mesa_set_remove(bs->dd->desc_sets, entry);
   }
}

bool
zink_batch_descriptor_init(struct zink_screen *screen, struct zink_batch_state *bs)
{
   bs->dd = rzalloc(bs, struct zink_batch_descriptor_data);
   if (!bs->dd)
      return false;
   bs->dd->desc_sets = _mesa_pointer_set_create(bs);
   return !!bs->dd->desc_sets;
}

struct zink_resource *
zink_get_resource_for_descriptor(struct zink_context *ctx, enum zink_descriptor_type type, enum pipe_shader_type shader, int idx)
{
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_UBO:
      return zink_resource(ctx->ubos[shader][idx].buffer);
   case ZINK_DESCRIPTOR_TYPE_SSBO:
      return zink_resource(ctx->ssbos[shader][idx].buffer);
   case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
      return ctx->sampler_views[shader][idx] ? zink_resource(ctx->sampler_views[shader][idx]->texture) : NULL;
   case ZINK_DESCRIPTOR_TYPE_IMAGE:
      return zink_resource(ctx->image_views[shader][idx].base.resource);
   default:
      break;
   }
   unreachable("unknown descriptor type!");
   return NULL;
}

static uint32_t
calc_descriptor_state_hash_ubo(struct zink_context *ctx, enum pipe_shader_type shader, int idx, uint32_t hash, bool need_offset)
{
   struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_UBO, shader, idx);
   struct zink_resource_object *obj = res ? res->obj : NULL;
   hash = XXH32(&obj, sizeof(void*), hash);
   void *hash_data = &ctx->ubos[shader][idx].buffer_size;
   size_t data_size = sizeof(unsigned);
   hash = XXH32(hash_data, data_size, hash);
   if (need_offset)
      hash = XXH32(&ctx->ubos[shader][idx].buffer_offset, sizeof(unsigned), hash);
   return hash;
}

static uint32_t
calc_descriptor_state_hash_ssbo(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   struct zink_resource *res = zink_get_resource_for_descriptor(ctx, ZINK_DESCRIPTOR_TYPE_SSBO, shader, idx);
   struct zink_resource_object *obj = res ? res->obj : NULL;
   hash = XXH32(&obj, sizeof(void*), hash);
   if (obj) {
      struct pipe_shader_buffer *ssbo = &ctx->ssbos[shader][idx];
      hash = XXH32(&ssbo->buffer_offset, sizeof(ssbo->buffer_offset), hash);
      hash = XXH32(&ssbo->buffer_size, sizeof(ssbo->buffer_size), hash);
   }
   return hash;
}

static inline uint32_t
get_sampler_view_hash(const struct zink_sampler_view *sampler_view)
{
   if (!sampler_view)
      return 0;
   return sampler_view->base.target == PIPE_BUFFER ?
          sampler_view->buffer_view->hash : sampler_view->image_view->hash;
}

static inline uint32_t
get_image_view_hash(const struct zink_image_view *image_view)
{
   if (!image_view || !image_view->base.resource)
      return 0;
   return image_view->base.resource->target == PIPE_BUFFER ?
          image_view->buffer_view->hash : image_view->surface->hash;
}

uint32_t
zink_get_sampler_view_hash(struct zink_context *ctx, struct zink_sampler_view *sampler_view, bool is_buffer)
{
   return get_sampler_view_hash(sampler_view) ? get_sampler_view_hash(sampler_view) :
          (is_buffer ? zink_screen(ctx->base.screen)->null_descriptor_hashes.buffer_view :
                       zink_screen(ctx->base.screen)->null_descriptor_hashes.image_view);
}

uint32_t
zink_get_image_view_hash(struct zink_context *ctx, struct zink_image_view *image_view, bool is_buffer)
{
   return get_image_view_hash(image_view) ? get_image_view_hash(image_view) :
          (is_buffer ? zink_screen(ctx->base.screen)->null_descriptor_hashes.buffer_view :
                       zink_screen(ctx->base.screen)->null_descriptor_hashes.image_view);
}

static uint32_t
calc_descriptor_state_hash_sampler(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][i].size; k++) {
      struct zink_sampler_view *sampler_view = zink_sampler_view(ctx->sampler_views[shader][idx + k]);
      bool is_buffer = zink_shader_descriptor_is_buffer(zs, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, i);
      uint32_t val = zink_get_sampler_view_hash(ctx, sampler_view, is_buffer);
      hash = XXH32(&val, sizeof(uint32_t), hash);
      if (is_buffer)
         continue;

      struct zink_sampler_state *sampler_state = ctx->sampler_states[shader][idx + k];

      if (sampler_state)
         hash = XXH32(&sampler_state->hash, sizeof(uint32_t), hash);
   }
   return hash;
}

static uint32_t
calc_descriptor_state_hash_image(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][i].size; k++) {
      uint32_t val = zink_get_image_view_hash(ctx, &ctx->image_views[shader][idx + k],
                                     zink_shader_descriptor_is_buffer(zs, ZINK_DESCRIPTOR_TYPE_IMAGE, i));
      hash = XXH32(&val, sizeof(uint32_t), hash);
   }
   return hash;
}

static uint32_t
update_descriptor_stage_state(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type)
{
   struct zink_shader *zs = shader == PIPE_SHADER_COMPUTE ? ctx->compute_stage : ctx->gfx_stages[shader];

   uint32_t hash = 0;
   for (int i = 0; i < zs->num_bindings[type]; i++) {
      /* skip push set members */
      if (zs->bindings[type][i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
         continue;

      int idx = zs->bindings[type][i].index;
      switch (type) {
      case ZINK_DESCRIPTOR_TYPE_UBO:
         hash = calc_descriptor_state_hash_ubo(ctx, shader, idx, hash, true);
         break;
      case ZINK_DESCRIPTOR_TYPE_SSBO:
         hash = calc_descriptor_state_hash_ssbo(ctx, zs, shader, i, idx, hash);
         break;
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         hash = calc_descriptor_state_hash_sampler(ctx, zs, shader, i, idx, hash);
         break;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         hash = calc_descriptor_state_hash_image(ctx, zs, shader, i, idx, hash);
         break;
      default:
         unreachable("unknown descriptor type");
      }
   }
   return hash;
}

static void
update_descriptor_state(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute)
{
   /* we shouldn't be calling this if we don't have to */
   assert(!ctx->dd->descriptor_states[is_compute].valid[type]);
   bool has_any_usage = false;

   if (is_compute) {
      /* just update compute state */
      bool has_usage = zink_program_get_descriptor_usage(ctx, PIPE_SHADER_COMPUTE, type);
      if (has_usage)
         ctx->dd->descriptor_states[is_compute].state[type] = update_descriptor_stage_state(ctx, PIPE_SHADER_COMPUTE, type);
      else
         ctx->dd->descriptor_states[is_compute].state[type] = 0;
      has_any_usage = has_usage;
   } else {
      /* update all gfx states */
      bool first = true;
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         bool has_usage = false;
         /* this is the incremental update for the shader stage */
         if (!ctx->dd->gfx_descriptor_states[i].valid[type]) {
            ctx->dd->gfx_descriptor_states[i].state[type] = 0;
            if (ctx->gfx_stages[i]) {
               has_usage = zink_program_get_descriptor_usage(ctx, i, type);
               if (has_usage)
                  ctx->dd->gfx_descriptor_states[i].state[type] = update_descriptor_stage_state(ctx, i, type);
               ctx->dd->gfx_descriptor_states[i].valid[type] = has_usage;
            }
         }
         if (ctx->dd->gfx_descriptor_states[i].valid[type]) {
            /* this is the overall state update for the descriptor set hash */
            if (first) {
               /* no need to double hash the first state */
               ctx->dd->descriptor_states[is_compute].state[type] = ctx->dd->gfx_descriptor_states[i].state[type];
               first = false;
            } else {
               ctx->dd->descriptor_states[is_compute].state[type] = XXH32(&ctx->dd->gfx_descriptor_states[i].state[type],
                                                                      sizeof(uint32_t),
                                                                      ctx->dd->descriptor_states[is_compute].state[type]);
            }
         }
         has_any_usage |= has_usage;
      }
   }
   ctx->dd->descriptor_states[is_compute].valid[type] = has_any_usage;
}

static void
zink_context_update_descriptor_states(struct zink_context *ctx, struct zink_program *pg)
{
   if (pg->dd->push_usage && (!ctx->dd->push_valid[pg->is_compute] ||
                                           pg->dd->push_usage != ctx->dd->last_push_usage[pg->is_compute])) {
      uint32_t hash = 0;
      if (pg->is_compute) {
          hash = calc_descriptor_state_hash_ubo(ctx, PIPE_SHADER_COMPUTE, 0, 0, false);
      } else {
         bool first = true;
         u_foreach_bit(stage, pg->dd->push_usage) {
            if (!ctx->dd->gfx_push_valid[stage]) {
               ctx->dd->gfx_push_state[stage] = calc_descriptor_state_hash_ubo(ctx, stage, 0, 0, false);
               ctx->dd->gfx_push_valid[stage] = true;
            }
            if (first)
               hash = ctx->dd->gfx_push_state[stage];
            else
               hash = XXH32(&ctx->dd->gfx_push_state[stage], sizeof(uint32_t), hash);
            first = false;
         }
      }
      ctx->dd->push_state[pg->is_compute] = hash;
      ctx->dd->push_valid[pg->is_compute] = true;
      ctx->dd->last_push_usage[pg->is_compute] = pg->dd->push_usage;
   }
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!ctx->dd->descriptor_states[pg->is_compute].valid[i])
         update_descriptor_state(ctx, i, pg->is_compute);
   }
}

void
zink_context_invalidate_descriptor_state(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned start, unsigned count)
{
   if (type == ZINK_DESCRIPTOR_TYPE_UBO && !start) {
      /* ubo 0 is the push set */
      ctx->dd->push_state[shader == PIPE_SHADER_COMPUTE] = 0;
      ctx->dd->push_valid[shader == PIPE_SHADER_COMPUTE] = false;
      if (shader != PIPE_SHADER_COMPUTE) {
         ctx->dd->gfx_push_state[shader] = 0;
         ctx->dd->gfx_push_valid[shader] = false;
      }
      return;
   }
   if (shader != PIPE_SHADER_COMPUTE) {
      ctx->dd->gfx_descriptor_states[shader].valid[type] = false;
      ctx->dd->gfx_descriptor_states[shader].state[type] = 0;
   }
   ctx->dd->descriptor_states[shader == PIPE_SHADER_COMPUTE].valid[type] = false;
   ctx->dd->descriptor_states[shader == PIPE_SHADER_COMPUTE].state[type] = 0;
}

bool
zink_descriptors_init(struct zink_context *ctx)
{
   ctx->dd = rzalloc(ctx, struct zink_descriptor_data);
   if (!ctx->dd)
      return false;
   return zink_descriptor_pool_init(ctx);
}

void
zink_descriptors_deinit(struct zink_context *ctx)
{
   zink_descriptor_pool_deinit(ctx);
}

bool
zink_descriptor_layouts_init(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++)
      if (!_mesa_hash_table_init(&ctx->desc_set_layouts[i], ctx, hash_descriptor_layout, equals_descriptor_layout))
         return false;
   return true;
}

void
zink_descriptor_layouts_deinit(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      hash_table_foreach(&ctx->desc_set_layouts[i], he) {
#if VK_USE_64_BIT_PTR_DEFINES == 1
         vkDestroyDescriptorSetLayout(screen->dev, (VkDescriptorSetLayout)he->data, NULL);
#else
         VkDescriptorSetLayout *r = (VkDescriptorSetLayout *)(he->data);
         vkDestroyDescriptorSetLayout(screen->dev, *r, NULL);
         ralloc_free(r);
#endif
         _mesa_hash_table_remove(&ctx->desc_set_layouts[i], he);
      }
   }
}
