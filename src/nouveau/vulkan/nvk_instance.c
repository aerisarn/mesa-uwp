#include "nvk_instance.h"

#include "nvk_physical_device.h"

static const struct vk_instance_extension_table instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .EXT_debug_report = true,
   .EXT_debug_utils = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceExtensionProperties(const char *pLayerName,
   uint32_t *pPropertyCount,
   VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &instance_extensions, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkInstance *pInstance)
{
   struct nvk_instance *instance;
   VkResult result;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = vk_alloc(pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &nvk_instance_entrypoints, true);

   result = vk_instance_init(
      &instance->vk, &instance_extensions, &dispatch_table, pCreateInfo, pAllocator);

   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return result;
   }

   instance->physical_devices_enumerated = false;
   list_inithead(&instance->physical_devices);

   *pInstance = nvk_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);

   if (!instance)
      return;

   list_for_each_entry_safe(struct nvk_physical_device, pdevice, &instance->physical_devices, link)
      nvk_physical_device_destroy(pdevice);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk, &nvk_instance_entrypoints, pName);
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return nvk_GetInstanceProcAddr(instance, pName);
}
