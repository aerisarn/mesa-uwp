#ifndef NVK_PRIVATE_H
#define NVK_PRIVATE_H 1

#include "nvk_entrypoints.h"

#include "util/log.h"
#include "util/u_memory.h"
#include "vulkan/runtime/vk_log.h"
#include "vulkan/util/vk_alloc.h"
#include "vulkan/util/vk_util.h"

#include <fcntl.h>
#include <xf86drm.h>

#define NVK_MIN_UBO_ALIGNMENT 16

/**
 * Warn on ignored extension structs.
 *
 * The Vulkan spec requires us to ignore unsupported or unknown structs in
 * a pNext chain.  In debug mode, emitting warnings for ignored structs may
 * help us discover structs that we should not have ignored.
 *
 *
 * From the Vulkan 1.0.38 spec:
 *
 *    Any component of the implementation (the loader, any enabled layers,
 *    and drivers) must skip over, without processing (other than reading the
 *    sType and pNext members) any chained structures with sType values not
 *    defined by extensions supported by that component.
 */
#define nvk_debug_ignored_stype(sType)                                                             \
   mesa_logd("%s: ignored VkStructureType %u\n", __func__, (sType))

#endif
