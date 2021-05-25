Virtio-GPU Venus
================

Venus is a Virtio-GPU protocol for Vulkan command serialization.  The protocol
definition and codegen are hosted at `venus-protocol
<https://gitlab.freedesktop.org/olv/venus-protocol>`__.  The renderer is
hosted at `virglrenderer
<https://gitlab.freedesktop.org/virgl/virglrenderer>`__.

The protocol is still under development.  This driver and the renderer are
both considered experimental.

Requirements
------------

The Venus renderer requires

- Vulkan 1.1
- ``VK_EXT_external_memory_dma_buf``
- ``VK_EXT_image_drm_format_modifier``
- ``VK_EXT_queue_family_foreign``

from the host driver.  However, it violates the spec in some places currently
and also relies on implementation-defined behaviors in others.  It is not
expected to work on all drivers meeting the requirements.  It has only been
tested with

- ANV 21.1 or later
- RADV 21.1 or later (the host kernel must have
  ``CONFIG_TRANSPARENT_HUGEPAGE`` disabled because of this `KVM issue
  <https://github.com/google/security-research/security/advisories/GHSA-7wq5-phmq-m584>`__)

The Venus driver requires supports for

- ``VIRTGPU_PARAM_RESOURCE_BLOB``
- ``VIRTGPU_PARAM_HOST_VISIBLE``
- ``VIRTGPU_PARAM_CROSS_DEVICE``
- ``VIRTGPU_PARAM_CONTEXT_INIT``

from the virtio-gpu kernel driver, unless vtest is used.  Currently, this
means the `context-init
<https://gitlab.freedesktop.org/virgl/drm-misc-next/-/tree/context-init>`__
kernel branch paired with `crosvm
<https://chromium.googlesource.com/chromiumos/platform/crosvm>`__.

vtest
-----

The simplest way to test Venus is to use virglrenderer's vtest server.  To
build virglrenderer with Venus support and to start the vtest server,

.. code-block:: console

    $ git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git
    $ cd virglrenderer
    $ meson out -Dvenus-experimental=true
    $ ninja -C out
    $ ./out/vtest/virgl_test_server --venus

In another shell,

.. code-block:: console

    $ export VK_ICD_FILENAMES=<path-to-virtio_icd.x86_64.json>
    $ export VN_DEBUG=vtest
    $ vulkaninfo
    $ vkcube

If the host driver of the system is not new enough, it is a good idea to build
the host driver as well when building the Venus driver.  Just remember to set
:envvar:`VK_ICD_FILENAMES` when starting the vtest server so that the vtest
server finds the locally built host driver.

Virtio-GPU
----------

Because the driver requires ``VIRTGPU_PARAM_CONTEXT_INIT`` from the virtio-gpu
kernel driver, one must make sure the guest kernel includes the changes from
the `context-init
<https://gitlab.freedesktop.org/virgl/drm-misc-next/-/tree/context-init>`__
branch.

To build crosvm,

.. code-block:: console

 $ mkdir crosvm
 $ cd crosvm
 $ wget https://storage.googleapis.com/git-repo-downloads/repo
 $ chmod +x repo
 $ ./repo init -g crosvm -u https://chromium.googlesource.com/chromiumos/manifest.git
 $ ./repo sync
 $ cd src/platform/crosvm
 $ RUSTFLAGS="-L<path-to-virglrenderer>/out/src" cargo build \
       --features "x virgl_renderer virgl_renderer_next default-no-sandbox"

Note that crosvm must be built with ``default-no-sandbox`` or started with
``--disable-sandbox`` in this setup.

This is how one might want to start crosvm

.. code-block:: console

 $ sudo LD_LIBRARY_PATH=<...> VK_ICD_FILENAMES=<...> ./target/debug/crosvm run \
       --gpu vulkan=true \
       --display-window-keyboard \
       --display-window-mouse \
       --host_ip 192.168.0.1 \
       --netmask 255.255.255.0 \
       --mac 12:34:56:78:9a:bc \
       --rwdisk disk.qcow2 \
       -p root=/dev/vda1 \
       <path-to-bzImage>

assuming a working system is installed to partition 1 of ``disk.qcow2``.
``sudo`` or ``CAP_NET_ADMIN`` is needed to set up the TAP network device.

Virtio-GPU and Virtio-WL
------------------------

In this setup, the guest userspace uses Xwayland and a special Wayland
compositor to connect guest X11/Wayland clients to the host Wayland
compositor, using Virtio-WL as the transport.  This setup is more tedious, but
that should hopefully change over time.

For now, the guest kernel must be built from the ``chromeos-5.10`` branch of
the `Chrome OS kernel
<https://chromium.googlesource.com/chromiumos/third_party/kernel>`__.  crosvm
should also be built with ``wl-dmabuf`` feature rather than ``x`` feature.

To build minigbm and to enable minigbm support in virglrenderer,

.. code-block:: console

 $ git clone https://chromium.googlesource.com/chromiumos/platform/minigbm
 $ cd minigbm
 $ CFLAGS=-DDRV_<I915-or-your-driver> OUT=out DESTDIR=out/install make install
 $ cd ../virglrenderer
 $ meson configure out -Dminigbm_allocation=true
 $ ninja -C out

Make sure a host Wayland compositor is running.  Replace
``--display-window-keyboard --display-window-mouse`` by
``--wayland-sock=<path-to-wayland-socket>`` when starting crosvm.

In the guest, build and start sommelier, the special Wayland compositor,

.. code-block:: console

 $ git clone https://chromium.googlesource.com/chromiumos/platform2
 $ cd platform2/vm_tools/sommelier
 $ meson out -Dxwayland_path=/usr/bin/Xwayland -Dxwayland_gl_driver_path=/usr/lib/dri
 $ ninja -C out
 $ sudo chmod 777 /dev/wl0
 $ ./out/sommelier -X --glamor
       --xwayland-gl-driver-path=<path-to-locally-built-gl-driver> \
       sleep infinity

sommelier requires ``xdg-shell-unstable-v6`` rather than the stable
``xdg-shell`` from the host compositor.  One must make sure the host
compositor still supports the older extension.
