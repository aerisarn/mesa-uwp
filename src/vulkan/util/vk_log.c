/*
 * Copyright © 2021 Intel Corporation
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
 */

#include "vk_log.h"
#include "vk_debug_utils.h"
#include "vk_debug_report.h"

#include "vk_command_buffer.h"
#include "vk_queue.h"
#include "vk_device.h"
#include "vk_physical_device.h"

#include "ralloc.h"

#include "log.h"

void
__vk_log_impl(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT types,
              int object_count,
              const void **objects_or_instance,
              const char *file,
              int line,
              const char *format,
              ...)
{
   struct vk_instance *instance = NULL;
   struct vk_object_base **objects = NULL;
   if (object_count == 0) {
      instance = (struct vk_instance *) objects_or_instance;
   } else {
      objects = (struct vk_object_base **) objects_or_instance;
      instance = objects[0]->device->physical->instance;
   }

#ifndef DEBUG
   if (unlikely(!instance) ||
       (likely(list_is_empty(&instance->debug_utils.callbacks)) &&
        likely(list_is_empty(&instance->debug_report.callbacks))))
      return;
#endif

   va_list va;
   char *message = NULL;

   va_start(va, format);
   message = ralloc_vasprintf(NULL, format, va);
   va_end(va);

   char *message_idname = ralloc_asprintf(NULL, "%s:%d", file, line);

#if DEBUG
   switch (severity) {
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
      mesa_logd("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
      mesa_logi("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
      if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
         mesa_logw("%s: PERF: %s", message_idname, message);
      else
         mesa_logw("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
      mesa_loge("%s: %s", message_idname, message);
      break;
   default:
      unreachable("Invalid debug message severity");
      break;
   }

   if (!instance) {
      ralloc_free(message);
      ralloc_free(message_idname);
      return;
   }
#endif

   /* If VK_EXT_debug_utils messengers have been set up, form the
    * message */
   if (!list_is_empty(&instance->debug_utils.callbacks)) {
      VkDebugUtilsMessengerCallbackDataEXT cb_data = {
         .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
         .pMessageIdName = message_idname,
         .messageIdNumber = 0,
         .pMessage = message,
      };

      VkDebugUtilsObjectNameInfoEXT *object_name_infos =
         ralloc_array(NULL, VkDebugUtilsObjectNameInfoEXT, object_count);

      ASSERTED int cmdbuf_n = 0, queue_n = 0;
      for (int i = 0; i < object_count; i++) {
         struct vk_object_base *base = objects[i];

         switch (base->type) {
         case VK_OBJECT_TYPE_COMMAND_BUFFER: {
            /* We allow at most one command buffer to be submitted at a time */
            assert(++cmdbuf_n <= 1);
            struct vk_command_buffer *cmd_buffer =
               (struct vk_command_buffer *)base;
            if (cmd_buffer->labels.size > 0) {
               cb_data.cmdBufLabelCount = util_dynarray_num_elements(
                  &cmd_buffer->labels, VkDebugUtilsLabelEXT);
               cb_data.pCmdBufLabels = cmd_buffer->labels.data;
            }
            break;
         }

         case VK_OBJECT_TYPE_QUEUE: {
            /* We allow at most one queue to be submitted at a time */
            assert(++queue_n <= 1);
            struct vk_queue *queue = (struct vk_queue *)base;
            if (queue->labels.size > 0) {
               cb_data.queueLabelCount =
                  util_dynarray_num_elements(&queue->labels, VkDebugUtilsLabelEXT);
               cb_data.pQueueLabels = queue->labels.data;
            }
            break;
         }
         default:
            break;
         }

         object_name_infos[i] = (VkDebugUtilsObjectNameInfoEXT){
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .pNext = NULL,
            .objectType = base->type,
            .objectHandle = (uint64_t)(uintptr_t)base,
            .pObjectName = base->object_name,
         };
      }
      cb_data.objectCount = object_count;
      cb_data.pObjects = object_name_infos;

      vk_debug_message(instance, severity, types, &cb_data);

      ralloc_free(object_name_infos);
   }

   /* If VK_EXT_debug_report callbacks also have been set up, forward
    * the message there as well */
   if (!list_is_empty(&instance->debug_report.callbacks)) {
      VkDebugReportFlagsEXT flags = 0;

      switch (severity) {
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
         flags |= VK_DEBUG_REPORT_DEBUG_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
         flags |= VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
         if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
            flags |= VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
         else
            flags |= VK_DEBUG_REPORT_WARNING_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
         flags |= VK_DEBUG_REPORT_ERROR_BIT_EXT;
         break;
      default:
         unreachable("Invalid debug message severity");
         break;
      }

      /* VK_EXT_debug_report-provided callback accepts only one object
       * related to the message. Since they are given to us in
       * decreasing order of importance, we're forwarding the first
       * one.
       */
      vk_debug_report(instance, flags, object_count ? objects[0] : NULL, 0,
                      0, message_idname, message);
   }

   ralloc_free(message);
   ralloc_free(message_idname);
}
