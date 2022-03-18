Base object structs
===================

The Vulkan runtime code provides a set of base object structs which must be
used if you want your driver to take advantage of any of the runtime code.
There are other base structs for various things which are not covered here
but those are optional.  The ones covered here are the bare minimum set
which form the core of the Vulkan runtime code:

.. contents::
   :local:


vk_object_base
--------------

The root base struct for all Vulkan objects is
:cpp:struct:`vk_object_base`.  Every object exposed to the client through
the Vulkan API *must* inherit from :cpp:struct:`vk_object_base` by having a
:cpp:struct:`vk_object_base` or some struct that inherits from
:cpp:struct:`vk_object_base` as the driver struct's first member.  Even
though we have `container_of()` and use it liberally, the
:cpp:struct:`vk_object_base` should be the first member as there are a few
places, particularly in the logging framework, where we use void pointers
to avoid casting and this only works if the address of the driver struct is
the same as the address of the :cpp:struct:`vk_object_base`.

The standard pattern for defining a Vulkan object inside a driver looks
something like this:

.. code-block:: c

   struct drv_sampler {
      struct vk_object_base base;

      /* Driver fields */
   };

   VK_DEFINE_NONDISP_HANDLE_CASTS(drv_sampler, base, VkSampler,
                                  VK_OBJECT_TYPE_SAMPLER);

Then, to the object in a Vulkan entrypoint,

.. code-block:: c

   VKAPI_ATTR void VKAPI_CALL drv_DestroySampler(
       VkDevice                                    _device,
       VkSampler                                   _sampler,
       const VkAllocationCallbacks*                pAllocator)
   {
      VK_FROM_HANDLE(drv_device, device, _device);
      VK_FROM_HANDLE(drv_sampler, sampler, _sampler);

      if (!sampler)
         return;

      /* Tear down the sampler */

      vk_object_free(&device->vk, pAllocator, sampler);
   }

The :cpp:any:`VK_DEFINE_NONDISP_HANDLE_CASTS()` macro defines a set of
type-safe cast functions called ``drv_sampler_from_handle()`` and
``drv_sampler_to_handle()`` which cast a :cpp:type:`VkSampler` to and from a
``struct drv_sampler *``.  Because compile-time type checking with Vulkan
handle types doesn't always work in C, the ``_from_handle()`` helper uses the
provided :cpp:type:`VkObjectType` to assert at runtime that the provided
handle is the correct type of object.  Both cast helpers properly handle
``NULL`` and ``VK_NULL_HANDLE`` as inputs.  The :cpp:any:`VK_FROM_HANDLE()`
macro provides a convenient way to declare a ``drv_foo`` pointer and
initialize it from a ``VkFoo`` handle in one smooth motion.

.. doxygenstruct:: vk_object_base
   :members:

.. doxygenfunction:: vk_object_base_init
.. doxygenfunction:: vk_object_base_finish

.. doxygendefine:: VK_DEFINE_HANDLE_CASTS

.. doxygendefine:: VK_DEFINE_NONDISP_HANDLE_CASTS

.. doxygendefine:: VK_FROM_HANDLE
