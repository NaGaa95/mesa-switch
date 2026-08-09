Native OpenGL on Nintendo Switch
=================================

This branch builds Mesa's native Gallium ``nouveau``/NVC0 driver for the
Nintendo Switch GM20B GPU.  It provides EGL, desktop OpenGL, OpenGL ES 1,
and OpenGL ES 2/3 entry points.  Vulkan and Zink are intentionally outside
the scope of this branch.

The Switch winsys continues to use ``switch-libdrm_nouveau`` for Horizon/libnx
device, memory, BO, and client-map integration.  Mesa replaces that package's
old ``pushbuf.o`` with an ABI-compatible implementation which batches logical
Nouveau submissions in libnx's GPFIFO userspace queue.  Mesa's Linux DRM ioctl
winsys cannot be used on Horizon.  The compatibility code treats the Switch as
a unified-memory SoC.

Mesa 26.2 makes Nouveau's push-buffer notification callback fallible.  The
Switch winsys carries that boolean callback privately and propagates failure
from submission while remaining source- and layout-compatible with the
currently packaged ``switch-libdrm_nouveau`` header.  No locally patched
portlibs header is required.

Native submission batching
--------------------------

NVC0 still closes a logical push buffer after every Gallium draw, preserving
its fence notification, BO validation, resource lifetime, syncpoint command,
cache acquire, and ``NO_PREFETCH`` ordering.  Draw-end kicks are deferred in
libnx's GPFIFO queue and a real ``nvGpuChannelKickoff`` is forced by:

* a Gallium context flush (including ``eglSwapBuffers``);
* query, surface, compute, video, or other non-draw submission;
* a fence wait, CPU BO map/wait, or cross-channel BO dependency;
* command-buffer or GPFIFO pressure;
* context teardown; or
* the configured logical-submit limit.

The default limit is 128 logical draw submissions, which keeps CPU/GPU overlap
while reducing hundreds of native service calls per frame to a small number.
Set these before EGL initialization for A/B tests:

.. code-block:: sh

   NOUVEAU_SWITCH_BATCH=0
   NOUVEAU_SWITCH_BATCH_SUBMITS=64

The first setting restores one native kickoff per logical submission.  A
``NOUVEAU_SWITCH_BATCH_SUBMITS`` value of ``0`` batches until a hard flush or
GPFIFO pressure.

Incremental native-batch residency
----------------------------------

Within one native batch, the Switch winsys keeps each attached Nouveau
``bufctx`` live after its first validation.  Later draws add only newly bound
or access-upgraded BOs instead of walking and re-referencing every stable
texture, shader, framebuffer, and vertex resource.  A successful native
kickoff advances a residency epoch and moves the complete current set back to
pending exactly once for the next batch.

GPFIFO and pending-reference pressure can end a native batch while the next
logical command list is already being assembled.  Before that boundary Mesa
seeds the in-progress record with the complete live BO set, so the first draw
of the new batch remains self-contained.  Validation-only cleanup similarly
invalidates the epoch rather than retaining references that no command owns.

Each logical flush also prepares the command BO for the next record with a
zero-sized ``nouveau_pushbuf_space`` call.  This retains the command BO without
restoring the removed full ``bufctx`` walk.  The winsys verifies that the new
record owns a current command-BO reference and latches a submission error
immediately if the invariant is ever broken.

The optimization is enabled by default.  Use this fallback before EGL
initialization for an A/B or recovery run:

.. code-block:: sh

   NOUVEAU_SWITCH_INCREMENTAL_REFS=0

Mesa GLthread
-------------

The Switch EGL frontend enables Mesa GLthread by default.  GL entry points
marshal commands on the application thread and one Mesa worker thread performs
state tracking and NVC0 command generation.  ``eglSwapBuffers``, context
rebinding/unbinding, and context destruction drain the queue before the
frontend directly accesses the Gallium context or native window.

NVC0 exposes GLthread's mapping capabilities only on Switch.  The supported
cross-thread operation is deliberately narrow: a newly allocated GART stream
buffer is mapped once with
``WRITE | UNSYNCHRONIZED | THREAD_SAFE`` through a BO-map-locked path and can
remain mapped while GM20B consumes older ranges.  Other Nouveau map paths keep
their existing synchronization behavior.

Set one of the following before EGL context creation to disable GLthread for
an A/B or recovery test; the Switch-specific option has final precedence:

.. code-block:: sh

   mesa_glthread=0
   MESA_GLTHREAD=0
   MESA_SWITCH_GLTHREAD=0

Prerequisites
-------------

Install a current devkitPro Switch toolchain with devkitA64 and libnx, plus
the Switch packages for ``libdrm_nouveau``, libelf, expat, zlib, and zstd.
Meson and Ninja must be available in the shell used for the build.

Build and stage the SDK
-----------------------

From an MSYS2/devkitPro shell:

.. code-block:: sh

   export DEVKITPRO=/opt/devkitpro
   export DEVKITA64=/opt/devkitpro/devkitA64
   ./build-opengl.sh

The SDK is staged at ``mesa-install/opt/devkitpro/portlibs/switch``.
On MSYS2 the script automatically selects ``switch_cross_file_msys2.txt``;
other hosts use ``switch_cross_file.txt``.  ``BUILD_DIR``, ``DESTDIR``,
``CROSS_FILE``, ``NATIVE_FILE``, ``MESON``, and ``NINJA`` can be overridden.
The build is static, release-mode, LLVM-free, and contains:

* ``libEGL.a`` with the Switch EGL frontend, Mesa state tracker, NVC0 driver,
  Nouveau winsys, and Mesa-private utility objects;
* ``libGL.a`` with public desktop ``gl*`` entry points;
* ``libGLESv1_CM.a``, ``libGLESv2.a``, and ``libglapi.a``;
* GL, GLES, and EGL headers;
* relocatable staged pkg-config files and ``OpenGLConfig.cmake``.

For a Makefile consumer, put the staged SDK before the installed portlibs:

.. code-block:: make

   MESA_SDK := /path/to/mesa-install/opt/devkitpro/portlibs/switch
   LIBDIRS  := $(MESA_SDK) $(PORTLIBS) $(LIBNX)
   LIBS     := -Wl,--start-group \
               -lGL -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
               -lexpat -lzstd -lz -lnx -lstdc++ -lm \
               -Wl,--end-group

``libGL`` is only needed for directly linked desktop OpenGL calls.  Programs
that load desktop functions through ``eglGetProcAddress`` may omit it.

For CMake, point ``OpenGL_DIR`` at the staged config directory and use the
normal imported targets:

.. code-block:: sh

   cmake -DOpenGL_DIR="$MESA_SDK/lib/cmake/OpenGL" ...

.. code-block:: cmake

   target_link_libraries(app PRIVATE OpenGL::EGL OpenGL::GL)
   # or: OpenGL::EGL OpenGL::GLES2

Device validation
-----------------

Compare application builds under the same clocks, resolution, settings,
workload, and camera conditions.  Record at least:

* cold boot to the rendered workload and its first shader-heavy section;
* a warm run after the single-file Mesa shader cache has populated;
* frame time or FPS in one repeatable CPU-heavy and one GPU-heavy workload;
* a 20--30 minute run through representative rendering conditions;
* suspend/resume, exit/relaunch, and repeated resource-heavy transitions.

The Switch frontend defaults to ``MESA_DISK_CACHE_SINGLE_FILE=1`` to reduce
SD-card metadata traffic.  Only enable ``MESA_NO_ERROR=1`` after correctness
and stability have been established.

The host build proves compilation and static linkage; rendering, syncpoint
behavior, suspend/resume, and performance still require execution on a
physical Switch.
