#ifndef NVK_CMD_POOL_H
#define NVK_CMD_POOL_H

#include "nvk_private.h"

#include "vk_command_pool.h"

struct nvk_cmd_pool {
   struct vk_command_pool vk;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_cmd_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

static inline struct nvk_device *
nvk_cmd_pool_device(struct nvk_cmd_pool *pool)
{
   return (struct nvk_device *)pool->vk.base.device;
}

#endif /* NVK_CMD_POOL_H */
