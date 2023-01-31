#include "nvk_descriptor_set_layout.h"

#include "nvk_descriptor_set.h"
#include "nvk_device.h"
#include "nvk_sampler.h"

#include "util/mesa-sha1.h"

static bool
binding_has_immutable_samplers(const VkDescriptorSetLayoutBinding *binding)
{
   switch (binding->descriptorType) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return binding->pImmutableSamplers != NULL;

   default:
      return false;
   }
}

void
nvk_descriptor_stride_align_for_type(VkDescriptorType type,
                                     const VkMutableDescriptorTypeListVALVE *type_list,
                                     uint32_t *stride, uint32_t *align)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      /* TODO: How do samplers work? */
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      *stride = *align = sizeof(struct nvk_image_descriptor);
      break;

   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      unreachable("TODO: Implement texel buffers");

   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      *stride = *align = sizeof(struct nvk_buffer_address);
      break;

   case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
      *stride = 1; /* Array size is bytes */
      *align = NVK_MIN_UBO_ALIGNMENT;
      break;

   case VK_DESCRIPTOR_TYPE_MUTABLE_VALVE:
      *stride = *align = 0;
      for (unsigned i = 0; i < type_list->descriptorTypeCount; i++) {
         /* This shouldn't recurse */
         assert(type_list->pDescriptorTypes[i] !=
                VK_DESCRIPTOR_TYPE_MUTABLE_VALVE);
         uint32_t desc_stride, desc_align;
         nvk_descriptor_stride_align_for_type(type_list->pDescriptorTypes[i],
                                              NULL, &desc_stride, &desc_align);
         *stride = MAX2(*stride, desc_stride);
         *align = MAX2(*align, desc_align);
      }
      *stride = ALIGN(*stride, *align);
      break;

   default:
      unreachable("Invalid descriptor type");
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDescriptorSetLayout(VkDevice _device,
                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(nvk_device, device, _device);

   uint32_t num_bindings = 0;
   uint32_t immutable_sampler_count = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[j];
      num_bindings = MAX2(num_bindings, binding->binding + 1);

      /* From the Vulkan 1.1.97 spec for VkDescriptorSetLayoutBinding:
     *
     *    "If descriptorType specifies a VK_DESCRIPTOR_TYPE_SAMPLER or
     *    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER type descriptor, then
     *    pImmutableSamplers can be used to initialize a set of immutable
     *    samplers. [...]  If descriptorType is not one of these descriptor
     *    types, then pImmutableSamplers is ignored.
     *
     * We need to be careful here and only parse pImmutableSamplers if we
     * have one of the right descriptor types.
     */
      if (binding_has_immutable_samplers(binding))
         immutable_sampler_count += binding->descriptorCount;
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct nvk_descriptor_set_layout, layout, 1);
   VK_MULTIALLOC_DECL(&ma, struct nvk_descriptor_set_binding_layout, bindings,
                      num_bindings);
   VK_MULTIALLOC_DECL(&ma, struct nvk_sampler *, samplers,
                      immutable_sampler_count);

   if (!vk_multialloc_zalloc(&ma, &device->vk.alloc,
                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &layout->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT);

   p_atomic_set(&layout->ref_cnt, 1);
   layout->binding_count = num_bindings;

   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[j];
      uint32_t b = binding->binding;
      /* We temporarily store pCreateInfo->pBindings[] index (plus one) in the
     * immutable_samplers pointer.  This provides us with a quick-and-dirty
     * way to sort the bindings by binding number.
     */
      layout->binding[b].immutable_samplers = (void *)(uintptr_t)(j + 1);
   }

   const VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
   const VkMutableDescriptorTypeCreateInfoVALVE *mutable_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_VALVE);

   uint32_t buffer_size = 0;
   for (uint32_t b = 0; b < num_bindings; b++) {
      /* We stashed the pCreateInfo->pBindings[] index (plus one) in the
     * immutable_samplers pointer.  Check for NULL (empty binding) and then
     * reset it and compute the index.
     */
      if (layout->binding[b].immutable_samplers == NULL)
         continue;
      const uint32_t info_idx =
         (uintptr_t)(void *)layout->binding[b].immutable_samplers - 1;
      layout->binding[b].immutable_samplers = NULL;

      const VkDescriptorSetLayoutBinding *binding =
         &pCreateInfo->pBindings[info_idx];

      if (binding->descriptorCount == 0)
         continue;

      layout->binding[b].type = binding->descriptorType;

      if (binding_flags_info && binding_flags_info->bindingCount > 0) {
         assert(binding_flags_info->bindingCount == pCreateInfo->bindingCount);
         layout->binding[b].flags = binding_flags_info->pBindingFlags[info_idx];
      }

      layout->binding[b].array_size = binding->descriptorCount;

      const VkMutableDescriptorTypeListVALVE *type_list = NULL;
      if (binding->descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_VALVE) {
         assert(mutable_info != NULL);
         assert(info_idx <= mutable_info->mutableDescriptorTypeListCount);
         type_list = &mutable_info->pMutableDescriptorTypeLists[info_idx];
      }

      uint32_t stride, align;
      nvk_descriptor_stride_align_for_type(binding->descriptorType, type_list,
                                           &stride, &align);

      layout->binding[b].offset = ALIGN_POT(buffer_size, align);
      layout->binding[b].stride = stride;
      buffer_size += stride * binding->descriptorCount;

      if (binding_has_immutable_samplers(binding)) {
         layout->binding[b].immutable_samplers = samplers;
         samplers += binding->descriptorCount;
         for (uint32_t i = 0; i < binding->descriptorCount; i++) {
            VK_FROM_HANDLE(nvk_sampler, sampler, binding->pImmutableSamplers[i]);
            layout->binding[b].immutable_samplers[i] = sampler;
         }
      }
   }

   layout->descriptor_buffer_size = buffer_size;

   struct mesa_sha1 sha1_ctx;
   _mesa_sha1_init(&sha1_ctx);

#define SHA1_UPDATE_VALUE(x) _mesa_sha1_update(&sha1_ctx, &(x), sizeof(x));
   SHA1_UPDATE_VALUE(layout->descriptor_buffer_size);
   SHA1_UPDATE_VALUE(layout->binding_count);

   for (uint32_t b = 0; b < num_bindings; b++) {
      SHA1_UPDATE_VALUE(layout->binding[b].type);
      SHA1_UPDATE_VALUE(layout->binding[b].flags);
      SHA1_UPDATE_VALUE(layout->binding[b].array_size);
      SHA1_UPDATE_VALUE(layout->binding[b].offset);
      SHA1_UPDATE_VALUE(layout->binding[b].stride);
      /* Immutable samplers are ignored for now */
   }
#undef SHA1_UPDATE_VALUE

   _mesa_sha1_final(&sha1_ctx, layout->sha1);

   *pSetLayout = nvk_descriptor_set_layout_to_handle(layout);

   return VK_SUCCESS;
}

void
nvk_descriptor_set_layout_destroy(
   struct nvk_device *device, struct nvk_descriptor_set_layout *layout)
{
   assert(layout->ref_cnt == 0);
   vk_object_free(&device->vk, NULL, layout);
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyDescriptorSetLayout(VkDevice _device,
                               VkDescriptorSetLayout descriptorSetLayout,
                               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_descriptor_set_layout, layout, descriptorSetLayout);

   if (!layout)
      return;

   nvk_descriptor_set_layout_unref(device, layout);
}
