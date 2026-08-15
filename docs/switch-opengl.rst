Native graphics on Nintendo Switch
==================================

This branch supports the Nintendo Switch GM20B GPU through Mesa's Nouveau
drivers.  It provides EGL, desktop OpenGL, OpenGL ES 1/2/3, and loaderless
NVK Vulkan libraries for Horizon applications.

Architecture
------------

``src/nouveau/horizon`` owns the libnx GPU device, address space, memory,
channel submission, synchronization, cache maintenance, and error handling.
Gallium/NVC0 and NVK use adapters over that shared backend.  The port does not
require the former external ``switch-libdrm_nouveau`` library or a patched
libnx ABI.

Horizon is a unified-memory platform.  CPU-visible allocations use explicit
cache policy and synchronization, while GPU work is ordered with native
syncpoint fences.  Unknown GPU completion fails closed: resources remain
owned or quarantined instead of being reused without completion proof.

Builds
------

The supported MSYS2 build requires devkitA64, libnx, libelf, expat, zlib,
zstd, Meson, Ninja, and a Rust toolchain containing the
``aarch64-unknown-linux-gnu`` standard library.

Build and stage only EGL/OpenGL/OpenGL ES with:

.. code-block:: sh

   ./build-opengl.sh

Build and stage only NVK Vulkan with:

.. code-block:: sh

   ./build-switch.sh

Build one release SDK containing both APIs with:

.. code-block:: sh

   ./build-unified.sh

The unified script installs static GL, GLES, EGL, and Vulkan libraries,
Khronos headers, pkg-config metadata, and CMake package files below one Switch
portlibs prefix.  It rejects a dirty checkout by default and creates a
deterministic SDK ZIP in ``dist``.  Set ``ALLOW_DIRTY=1`` only for a local
development build.

The installed CMake packages export ``OpenGL::GL``, ``OpenGL::EGL``,
``OpenGL::GLES1``, ``OpenGL::GLES2``, ``Vulkan::Headers``, and
``Vulkan::Vulkan``.  The Vulkan archive is loaderless and is linked directly
by the application.

The unified SDK also embeds Zink in ``libEGL``.  It calls the loaderless NVK
entrypoints directly, so no Vulkan loader or second driver package is needed.
NVC0 remains the default OpenGL backend.  Select Zink before
``eglInitialize`` with either:

.. code-block:: sh

   MESA_SWITCH_GL_DRIVER=zink
   MESA_LOADER_DRIVER_OVERRIDE=zink

``MESA_SWITCH_GL_DRIVER=nvc0`` explicitly selects the native Gallium driver.
One EGL display uses one backend for its lifetime.  Zink supports NWindow and
pbuffer surfaces through VI_NN/Kopper, including swap intervals zero and one
and ``EGL_MESA_horizon_surface_resize``.  Explicit Vulkan and either OpenGL
backend may coexist in the same process through the shared Horizon runtime.

Switch Vulkan WSI supports ``FIFO`` and ``IMMEDIATE`` presentation.  Immediate
swapchains use at least three NWindow buffers and swap interval zero.  NVK
calibrated device timestamps use Horizon's GPU timestamp query.

Runtime policy
--------------

NVK enables mapped physical completion for 3D queues.  Copy-only and bind
contexts retain native-fence completion.  The native fallback can be selected
before device creation with:

.. code-block:: sh

   NVK_SWITCH_MAPPED_COMPLETION=0

NVK command pools and transient memory streams use CPU-uncached, GPU-cached
memory by default.  Either class can be restored to CPU-cached memory for
compatibility testing:

.. code-block:: sh

   NVK_SWITCH_CMD_MEM_CPU_UNCACHED=0
   NVK_SWITCH_MEM_STREAM_CPU_UNCACHED=0

Mesa GLthread can be controlled before EGL context creation with
``MESA_SWITCH_GLTHREAD=0`` or ``1``.  Gallium's threaded context remains a
separate driver decision.

The optional ``GL_ARB_gl_spirv`` path remains hidden by default pending wider
device coverage.  It may be enabled before context creation with
``NOUVEAU_SWITCH_GL_SPIRV=1``.

Platform limitations
--------------------

Horizon does not provide POSIX file descriptors or DMA-BUF memory sharing and
cannot create fixed-address or reserved CPU mappings.  Switch NVK therefore
does not expose ``VK_KHR_external_memory_fd``,
``VK_EXT_external_memory_dma_buf``, or ``VK_EXT_map_memory_placed`` and reports
the corresponding external buffer/image handle types as unsupported.

The EGL frontend supports NWindow surfaces and pbuffers.  Native pixmap
surfaces are not implemented.  Until Khronos conformance testing is complete,
Switch EGL configurations are advertised as non-conformant.
