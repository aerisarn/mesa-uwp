Graphics state
==============

The Mesa Vulkan runtime provides helpers for managing the numerous pieces
of graphics state associated with a ``VkPipeline`` or set dynamically on a
command buffer.  No such helpers are provided for compute or ray-tracing
because they have little or no state besides the shaders themselves.


Pipeline state
--------------

All (possibly dynamic) Vulkan graphics pipeline state is encapsulated into
a single :cpp:struct:`vk_graphics_pipeline_state` structure which contains
pointers to sub-structures for each of the different state categories.
Unlike :cpp:type:`VkGraphicsPipelineCreateInfo`, the pointers in
:cpp:struct:`vk_graphics_pipeline_state` are guaranteed to be either be
NULL or point to valid and properly populated memory.

When creating a pipeline, the
:cpp:func:`vk_graphics_pipeline_state_fill()` function can be used to
gather all of the state from the core structures as well as various `pNext`
chains into a single state structure.  Whenever an extension struct is
missing, a reasonable default value is provided whenever possible.  The
usual flow for creating a full graphics pipeline (not library) looks like
this:

.. code-block:: c

   struct vk_graphics_pipeline_state state = { };
   struct vk_graphics_pipeline_all_state all;
   vk_graphics_pipeline_state_fill(&device->vk, &state, pCreateInfo,
                                   NULL, &all, NULL, 0, NULL);

   /* Emit stuff using the state in `state` */

The :cpp:struct:`vk_graphics_pipeline_all_state` structure exists to allow
the state to sit on the stack instead of requiring a heap allocation.  This
is useful if you intend to use the state right away and don't need to store
it.  For pipeline libraries, it's likely more useful to use the dynamically
allocated version and store the dynamically allocated memory in the
library pipeline.

.. code-block:: c

   /* Assuming we have a vk_graphics_pipeline_state in pipeline */
   memset(&pipeline->state, 0, sizeof(pipeline->state));

   for (uint32_t i = 0; i < lib_info->libraryCount; i++) {
      VK_FROM_HANDLE(drv_graphics_pipeline_library, lib, lib_info->pLibraries[i]);
      vk_graphics_pipeline_state_merge(&pipeline->state, &lib->state);
   }

   /* This assumes you have a void **state_mem in pipeline */
   result = vk_graphics_pipeline_state_fill(&device->vk, &pipeline->state,
                                            pCreateInfo, NULL, NULL, pAllocator,
                                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
                                            &pipeline->state_mem);
   if (result != VK_SUCCESS)
      return result;

State from dependent libraries can be merged together using
:cpp:func:`vk_graphics_pipeline_state_merge`.
:cpp:func:`vk_graphics_pipeline_state_fill` will then only attempt to
populate missing fields.  You can also merge dependent pipeline libraries
together but store the final state on the stack for immediate consumption:

.. code-block:: c

   struct vk_graphics_pipeline_state state = { };

   for (uint32_t i = 0; i < lib_info->libraryCount; i++) {
      VK_FROM_HANDLE(drv_graphics_pipeline_library, lib, lib_info->pLibraries[i]);
      vk_graphics_pipeline_state_merge(&state, &lib->state);
   }

   struct vk_graphics_pipeline_all_state all;
   vk_graphics_pipeline_state_fill(&device->vk, &state, pCreateInfo,
                                   NULL, &all, NULL, 0, NULL);

.. doxygenfunction:: vk_graphics_pipeline_state_fill

.. doxygenfunction:: vk_graphics_pipeline_state_merge

Reference
---------

.. doxygenstruct:: vk_graphics_pipeline_state
   :members:

.. doxygenstruct:: vk_vertex_binding_state
   :members:

.. doxygenstruct:: vk_vertex_attribute_state
   :members:

.. doxygenstruct:: vk_vertex_input_state
   :members:

.. doxygenstruct:: vk_input_assembly_state
   :members:

.. doxygenstruct:: vk_tessellation_state
   :members:

.. doxygenstruct:: vk_viewport_state
   :members:

.. doxygenstruct:: vk_discard_rectangles_state
   :members:

.. doxygenstruct:: vk_rasterization_state
   :members:

.. doxygenstruct:: vk_fragment_shading_rate_state
   :members:

.. doxygenstruct:: vk_sample_locations_state
   :members:

.. doxygenstruct:: vk_multisample_state
   :members:

.. doxygenstruct:: vk_stencil_test_face_state
   :members:

.. doxygenstruct:: vk_depth_stencil_state
   :members:

.. doxygenstruct:: vk_color_blend_state
   :members:

.. doxygenstruct:: vk_render_pass_state
   :members:

