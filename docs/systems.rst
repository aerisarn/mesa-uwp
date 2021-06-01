Platforms and Drivers
=====================

Mesa is primarily developed and used on Linux systems. But there's also
support for Windows, other flavors of Unix and other systems such as
Haiku. We're actively developing and maintaining several hardware and
software drivers.

The primary API is OpenGL but there's also support for OpenGL ES, Vulkan,
EGL, OpenMAX, OpenCL, VDPAU, VA-API and XvMC.

Hardware drivers include:

-  Intel GMA, HD Graphics, Iris. See `Intel's
   Website <https://01.org/linuxgraphics>`__
-  AMD Radeon series. See
   `RadeonFeature <https://www.x.org/wiki/RadeonFeature>`__
-  NVIDIA GPUs (Riva TNT and later). See `Nouveau
   Wiki <https://nouveau.freedesktop.org>`__
-  Qualcomm Adreno A2xx-A6xx. See `Freedreno
   Wiki <https://github.com/freedreno/freedreno/wiki>`__
-  Broadcom VideoCore 4, 5. See `This Week in
   V3D <https://anholt.github.io/twivc4/>`__
-  ARM Mali Utgard. See :doc:`Lima <drivers/lima>`
-  ARM Mali Midgard, Bifrost. See :doc:`Panfrost <drivers/panfrost>`
-  Vivante GCxxx. See `Etnaviv
   Wiki <https://github.com/laanwj/etna_viv/wiki>`__
-  NVIDIA Tegra (K1 and later).

Software drivers include:

-  :doc:`LLVMpipe <drivers/llvmpipe>` - uses LLVM for x86 JIT code generation
   and is multi-threaded
-  Softpipe - a reference Gallium driver
-  :doc:`SVGA3D <drivers/svga3d>` - driver for VMware virtual GPU
-  :doc:`OpenSWR <drivers/openswr>` - x86-optimized software renderer
   for visualization workloads
-  `VirGL <https://virgil3d.github.io/>`__ - research project for
   accelerated graphics for qemu guests

Additional driver information:

-  `DRI hardware drivers <https://dri.freedesktop.org/>`__ for the X
   Window System
-  :doc:`Xlib / swrast driver <xlibdriver>` for the X Window System
   and Unix-like operating systems

Deprecated Systems and Drivers
------------------------------

In the past there were other drivers for older GPUs and operating
systems. These have been removed from the Mesa source tree and
distribution. If anyone's interested though, the code can be found in
the Git repo. The list includes:

-  3dfx Glide
-  3DLABS Gamma
-  ATI Mach 64
-  ATI Rage 128
-  DEC OpenVMS
-  Intel i810
-  Linux fbdev
-  Matrox
-  MS-DOS
-  S3 Savage
-  Silicon Integrated Systems
-  swrast
-  VIA Unichrome
