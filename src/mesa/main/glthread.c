/*
 * Copyright © 2012 Intel Corporation
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

/** @file glthread.c
 *
 * Support functions for the glthread feature of Mesa.
 *
 * In multicore systems, many applications end up CPU-bound with about half
 * their time spent inside their rendering thread and half inside Mesa.  To
 * alleviate this, we put a shim layer in Mesa at the GL dispatch level that
 * quickly logs the GL commands to a buffer to be processed by a worker
 * thread.
 */

#include "main/mtypes.h"
#include "main/glthread.h"
#include "main/glthread_marshal.h"
#include "main/hash.h"
#include "main/pixelstore.h"
#include "util/u_atomic.h"
#include "util/u_thread.h"
#include "util/u_cpu_detect.h"
#include "util/u_debug.h"
#include "util/thread_sched.h"

#include "state_tracker/st_context.h"

#define GLTHREAD_PERF_REPORT_INTERVAL_NS (5ull * ONE_SECOND_IN_NS)

static void
glthread_perf_atomic_max(uint64_t *value, uint64_t candidate)
{
   uint64_t current = p_atomic_read(value);
   while (candidate > current) {
      const uint64_t previous = p_atomic_cmpxchg(value, current, candidate);
      if (previous == current)
         return;
      current = previous;
   }
}

static void
glthread_perf_log(struct gl_context *ctx, const char *reason, bool force,
                  uint64_t now_ns)
{
   struct glthread_state *glthread = &ctx->GLThread;
   if (!glthread->perf_enabled)
      return;

   if (now_ns == 0)
      now_ns = os_time_get_nano();
   if (!force && now_ns - glthread->perf_last_report_ns <
                    GLTHREAD_PERF_REPORT_INTERVAL_NS)
      return;
   glthread->perf_last_report_ns = now_ns;

   const uint64_t submit_batches =
      p_atomic_read(&glthread->perf.submit_batches);
   const uint64_t submit_words =
      p_atomic_read(&glthread->perf.submit_words);
   const uint64_t enqueue_ns = p_atomic_read(&glthread->perf.enqueue_ns);
   const uint64_t enqueue_max_ns =
      p_atomic_read(&glthread->perf.enqueue_max_ns);
   const uint64_t worker_batches =
      p_atomic_read(&glthread->perf.worker_batches);
   const uint64_t worker_commands =
      p_atomic_read(&glthread->perf.worker_commands);
   const uint64_t worker_words =
      p_atomic_read(&glthread->perf.worker_words);
   const uint64_t worker_ns = p_atomic_read(&glthread->perf.worker_ns);
   const uint64_t worker_max_ns =
      p_atomic_read(&glthread->perf.worker_max_ns);
   const uint64_t latency_ns =
      p_atomic_read(&glthread->perf.queue_latency_ns);
   const uint64_t latency_max_ns =
      p_atomic_read(&glthread->perf.queue_latency_max_ns);
   const uint64_t direct_batches =
      p_atomic_read(&glthread->perf.direct_batches);
   const uint64_t direct_commands =
      p_atomic_read(&glthread->perf.direct_commands);
   const uint64_t direct_words =
      p_atomic_read(&glthread->perf.direct_words);
   const uint64_t direct_ns = p_atomic_read(&glthread->perf.direct_ns);
   const uint64_t direct_max_ns =
      p_atomic_read(&glthread->perf.direct_max_ns);
   const uint64_t finish_calls =
      p_atomic_read(&glthread->perf.finish_calls);
   const uint64_t wait_calls = p_atomic_read(&glthread->perf.wait_calls);
   const uint64_t wait_ns = p_atomic_read(&glthread->perf.wait_ns);
   const uint64_t wait_max_ns =
      p_atomic_read(&glthread->perf.wait_max_ns);

   _debug_printf(
      "Mesa GLThread: perf %s elapsed=%llums submit=%llu words=%llu "
      "enqueue=%llums avg/max=%llu/%lluus latency=%llums "
      "avg/max=%llu/%lluus\n",
      reason,
      (unsigned long long)((now_ns - glthread->perf_start_ns) / 1000000),
      (unsigned long long)submit_batches,
      (unsigned long long)submit_words,
      (unsigned long long)(enqueue_ns / 1000000),
      (unsigned long long)(submit_batches ?
                              enqueue_ns / submit_batches / 1000 : 0),
      (unsigned long long)(enqueue_max_ns / 1000),
      (unsigned long long)(latency_ns / 1000000),
      (unsigned long long)(worker_batches ?
                              latency_ns / worker_batches / 1000 : 0),
      (unsigned long long)(latency_max_ns / 1000));
   _debug_printf(
      "Mesa GLThread: perf %s worker=%llu cmds=%llu words=%llu cpu=%llums "
      "avg/max=%llu/%lluus direct=%llu cmds=%llu words=%llu cpu=%llums "
      "max=%lluus finish=%llu waits=%llu/%llums max=%lluus "
      "queue_totals=%u/%u/%u/%u\n",
      reason, (unsigned long long)worker_batches,
      (unsigned long long)worker_commands,
      (unsigned long long)worker_words,
      (unsigned long long)(worker_ns / 1000000),
      (unsigned long long)(worker_batches ?
                              worker_ns / worker_batches / 1000 : 0),
      (unsigned long long)(worker_max_ns / 1000),
      (unsigned long long)direct_batches,
      (unsigned long long)direct_commands,
      (unsigned long long)direct_words,
      (unsigned long long)(direct_ns / 1000000),
      (unsigned long long)(direct_max_ns / 1000),
      (unsigned long long)finish_calls,
      (unsigned long long)wait_calls,
      (unsigned long long)(wait_ns / 1000000),
      (unsigned long long)(wait_max_ns / 1000),
      p_atomic_read(&glthread->stats.num_offloaded_items),
      p_atomic_read(&glthread->stats.num_direct_items),
      p_atomic_read(&glthread->stats.num_syncs),
      p_atomic_read(&glthread->stats.num_batches));
}

static void
glthread_update_global_locking(struct gl_context *ctx)
{
   struct gl_shared_state *shared = ctx->Shared;

   /* Determine if we should lock the global mutexes. */
   simple_mtx_lock(&shared->Mutex);
   int64_t current_time = os_time_get_nano();

   /* We can only lock the mutexes after NoLockDuration nanoseconds have
    * passed since multiple contexts were active.
    */
   bool lock_mutexes = shared->GLThread.LastContextSwitchTime +
                       shared->GLThread.NoLockDuration < current_time;

   /* Check if multiple contexts are active (the last executing context is
    * different).
    */
   if (ctx != shared->GLThread.LastExecutingCtx) {
      if (lock_mutexes) {
         /* If we get here, we've been locking the global mutexes for a while
          * and now we are switching contexts. */
         if (shared->GLThread.LastContextSwitchTime +
             120 * ONE_SECOND_IN_NS < current_time) {
            /* If it's been more than 2 minutes of only one active context,
             * indicating that there was no other active context for a long
             * time, reset the no-lock time to its initial state of only 1
             * second. This is most likely an infrequent situation of
             * multi-context loading of game content and shaders.
             * (this is a heuristic)
             */
            shared->GLThread.NoLockDuration = ONE_SECOND_IN_NS;
         } else if (shared->GLThread.NoLockDuration < 32 * ONE_SECOND_IN_NS) {
            /* Double the no-lock duration if we are transitioning from only
             * one active context to multiple active contexts after a short
             * time, up to a maximum of 32 seconds, indicating that multiple
             * contexts are frequently executing. (this is a heuristic)
             */
            shared->GLThread.NoLockDuration *= 2;
         }

         lock_mutexes = false;
      }

      /* There are multiple active contexts. Update the last executing context
       * and the last context switch time. We only start locking global mutexes
       * after LastContextSwitchTime + NoLockDuration passes, so this
       * effectively resets the non-locking stopwatch to 0, so that multiple
       * contexts can execute simultaneously as long as they are not idle.
       */
      shared->GLThread.LastExecutingCtx = ctx;
      shared->GLThread.LastContextSwitchTime = current_time;
   }
   simple_mtx_unlock(&shared->Mutex);

   ctx->GLThread.LockGlobalMutexes = lock_mutexes;
}

static void
glthread_unmarshal_batch(void *job, void *gdata, int thread_index)
{
   struct glthread_batch *batch = (struct glthread_batch*)job;
   struct gl_context *ctx = batch->ctx;
   unsigned pos = 0;
   unsigned used = batch->used;
   uint64_t *buffer = batch->buffer;
   struct gl_shared_state *shared = ctx->Shared;
   const bool perf_enabled = ctx->GLThread.perf_enabled;
   const bool perf_worker = perf_enabled &&
      u_thread_is_self(ctx->GLThread.queue.threads[0]);
   const uint64_t perf_start_ns =
      perf_enabled ? os_time_get_nano() : 0;
   const uint64_t perf_submit_ns = batch->perf_submit_ns;
   uint64_t perf_commands = 0;

   /* Determine once every 64 batches whether shared mutexes should be locked.
    * We have to do this less frequently because os_time_get_nano() is very
    * expensive if the clock source is not TSC. See:
    *    https://gitlab.freedesktop.org/mesa/mesa/-/issues/8910
    */
   if (ctx->GLThread.GlobalLockUpdateBatchCounter++ % 64 == 0)
      glthread_update_global_locking(ctx);

   /* Execute the GL calls. */
   _mesa_glapi_set_dispatch(ctx->Dispatch.Current);

   /* Here we lock the mutexes once globally if possible. If not, we just
    * fallback to the individual API calls doing it.
    */
   bool lock_mutexes = ctx->GLThread.LockGlobalMutexes;
   if (lock_mutexes) {
      _mesa_HashLockMutex(&shared->BufferObjects);
      ctx->BufferObjectsLocked = true;
      simple_mtx_lock(&shared->TexMutex);
      ctx->TexturesLocked = true;
   }

   while (pos < used) {
      const struct marshal_cmd_base *cmd =
         (const struct marshal_cmd_base *)&buffer[pos];

      pos += _mesa_unmarshal_dispatch[cmd->cmd_id](ctx, cmd);
      if (perf_enabled)
         perf_commands++;
   }

   if (lock_mutexes) {
      ctx->TexturesLocked = false;
      simple_mtx_unlock(&shared->TexMutex);
      ctx->BufferObjectsLocked = false;
      _mesa_HashUnlockMutex(&shared->BufferObjects);
   }

   assert(pos == used);
   batch->used = 0;
   batch->perf_submit_ns = 0;

   if (perf_enabled) {
      const uint64_t elapsed_ns = os_time_get_nano() - perf_start_ns;
      if (perf_worker) {
         p_atomic_inc(&ctx->GLThread.perf.worker_batches);
         p_atomic_add(&ctx->GLThread.perf.worker_commands, perf_commands);
         p_atomic_add(&ctx->GLThread.perf.worker_words, used);
         p_atomic_add(&ctx->GLThread.perf.worker_ns, elapsed_ns);
         glthread_perf_atomic_max(&ctx->GLThread.perf.worker_max_ns,
                                  elapsed_ns);
         if (perf_submit_ns && perf_start_ns >= perf_submit_ns) {
            const uint64_t latency_ns = perf_start_ns - perf_submit_ns;
            p_atomic_add(&ctx->GLThread.perf.queue_latency_ns,
                         latency_ns);
            glthread_perf_atomic_max(
               &ctx->GLThread.perf.queue_latency_max_ns, latency_ns);
         }
      } else {
         p_atomic_inc(&ctx->GLThread.perf.direct_batches);
         p_atomic_add(&ctx->GLThread.perf.direct_commands, perf_commands);
         p_atomic_add(&ctx->GLThread.perf.direct_words, used);
         p_atomic_add(&ctx->GLThread.perf.direct_ns, elapsed_ns);
         glthread_perf_atomic_max(&ctx->GLThread.perf.direct_max_ns,
                                  elapsed_ns);
      }
   }

   unsigned batch_index = batch - ctx->GLThread.batches;
   _mesa_glthread_signal_call(&ctx->GLThread.LastProgramChangeBatch, batch_index);
   _mesa_glthread_signal_call(&ctx->GLThread.LastDListChangeBatchIndex, batch_index);

   p_atomic_inc(&ctx->GLThread.stats.num_batches);
}

static void
glthread_apply_thread_sched_policy(struct gl_context *ctx, bool initialization)
{
   struct glthread_state *glthread = &ctx->GLThread;

   if (!glthread->thread_sched_enabled)
      return;

   /* Apply our thread scheduling policy for better multithreading
    * performance.
    */
   if (initialization || ++glthread->pin_thread_counter % 128 == 0) {
      int cpu = util_get_current_cpu();

      if (cpu >= 0 &&
          util_thread_sched_apply_policy(glthread->queue.threads[0],
                                         UTIL_THREAD_GLTHREAD, cpu,
                                         &glthread->thread_sched_state)) {
         /* If it's successful, apply the policy to the driver threads too. */
         ctx->pipe->set_context_param(ctx->pipe,
                                      PIPE_CONTEXT_PARAM_UPDATE_THREAD_SCHEDULING,
                                      cpu);
      }
   }
}

static void
glthread_thread_initialization(void *job, void *gdata, int thread_index)
{
   struct gl_context *ctx = (struct gl_context*)job;

   st_set_background_context(ctx, &ctx->GLThread.stats);
   _mesa_glapi_set_context(ctx);
}

static void
_mesa_glthread_init_dispatch(struct gl_context *ctx,
                             struct _glapi_table *table)
{
   _mesa_glthread_init_dispatch0(ctx, table);
   _mesa_glthread_init_dispatch1(ctx, table);
   _mesa_glthread_init_dispatch2(ctx, table);
   _mesa_glthread_init_dispatch3(ctx, table);
   _mesa_glthread_init_dispatch4(ctx, table);
   _mesa_glthread_init_dispatch5(ctx, table);
   _mesa_glthread_init_dispatch6(ctx, table);
   _mesa_glthread_init_dispatch7(ctx, table);
}

void
_mesa_glthread_init(struct gl_context *ctx)
{
   struct pipe_screen *screen = ctx->screen;
   struct glthread_state *glthread = &ctx->GLThread;
   const bool perf_requested =
      debug_get_bool_option("MESA_GLTHREAD_PERF", false);
   assert(!glthread->enabled);

   if (!screen->caps.map_unsynchronized_thread_safe ||
       !screen->caps.allow_mapped_buffers_during_execution) {
      if (perf_requested)
         _debug_printf("Mesa GLThread: perf unavailable (screen caps)\n");
      return;
   }

   if (!util_queue_init(&glthread->queue, "gl", MARSHAL_MAX_BATCHES - 2,
                        1, 0, NULL)) {
      if (perf_requested)
         _debug_printf("Mesa GLThread: perf unavailable (queue init)\n");
      return;
   }

   _mesa_InitHashTable(&glthread->VAOs);
   _mesa_glthread_reset_vao(&glthread->DefaultVAO);
   glthread->CurrentVAO = &glthread->DefaultVAO;

   ctx->MarshalExec = _mesa_alloc_dispatch_table(true);
   if (!ctx->MarshalExec) {
      _mesa_DeinitHashTable(&glthread->VAOs, NULL, NULL);
      util_queue_destroy(&glthread->queue);
      if (perf_requested)
         _debug_printf("Mesa GLThread: perf unavailable (dispatch alloc)\n");
      return;
   }

   _mesa_glthread_init_dispatch(ctx, ctx->MarshalExec);
   _mesa_init_pixelstore_attrib(ctx, &glthread->Unpack);

   for (unsigned i = 0; i < MARSHAL_MAX_BATCHES; i++) {
      glthread->batches[i].ctx = ctx;
      util_queue_fence_init(&glthread->batches[i].fence);
   }
   glthread->next_batch = &glthread->batches[glthread->next];
   glthread->used = 0;
   glthread->stats.queue = &glthread->queue;

   _mesa_glthread_init_call_fence(&glthread->LastProgramChangeBatch);
   _mesa_glthread_init_call_fence(&glthread->LastDListChangeBatchIndex);

   glthread->perf_enabled = perf_requested;
   if (glthread->perf_enabled) {
      glthread->perf_start_ns = os_time_get_nano();
      glthread->perf_last_report_ns = glthread->perf_start_ns;
      _debug_printf("Mesa GLThread: perf telemetry enabled\n");
   }

   _mesa_glthread_enable(ctx);

   /* Execute the thread initialization function in the thread. */
   struct util_queue_fence fence;
   util_queue_fence_init(&fence);
   util_queue_add_job(&glthread->queue, ctx, &fence,
                      glthread_thread_initialization, NULL, 0);
   util_queue_fence_wait(&fence);
   util_queue_fence_destroy(&fence);

   glthread->thread_sched_enabled = ctx->pipe->set_context_param &&
                                    util_thread_scheduler_enabled();
   util_thread_scheduler_init_state(&glthread->thread_sched_state);
   glthread_apply_thread_sched_policy(ctx, true);
}

static void
free_vao(void *data, UNUSED void *userData)
{
   free(data);
}

void
_mesa_glthread_destroy(struct gl_context *ctx)
{
   struct glthread_state *glthread = &ctx->GLThread;

   _mesa_glthread_disable(ctx);

   glthread_perf_log(ctx, "final", true, 0);

   if (util_queue_is_initialized(&glthread->queue)) {
      util_queue_destroy(&glthread->queue);

      for (unsigned i = 0; i < MARSHAL_MAX_BATCHES; i++)
         util_queue_fence_destroy(&glthread->batches[i].fence);

      _mesa_DeinitHashTable(&glthread->VAOs, free_vao, NULL);
      _mesa_glthread_release_upload_buffer(ctx, false);
   }
}

void _mesa_glthread_enable(struct gl_context *ctx)
{
   if (ctx->GLThread.enabled ||
       ctx->Dispatch.Current == ctx->Dispatch.ContextLost ||
       ctx->GLThread.DebugOutputSynchronous)
      return;

   ctx->GLThread.enabled = true;
   ctx->GLApi = ctx->MarshalExec;

   /* glthread takes over all thread scheduling. */
   ctx->st->thread_scheduler_disabled = true;

   /* Update the dispatch only if the dispatch is current. */
   if (_mesa_get_dispatch(ctx) == ctx->Dispatch.Current) {
       _mesa_set_dispatch(ctx, ctx->GLApi);
   }
}

void _mesa_glthread_disable(struct gl_context *ctx)
{
   if (!ctx->GLThread.enabled)
      return;

   _mesa_glthread_finish(ctx);

   ctx->GLThread.enabled = false;
   ctx->GLApi = ctx->Dispatch.Current;

   /* Re-enable thread scheduling in st/mesa when glthread is disabled. */
   if (ctx->pipe->set_context_param && util_thread_scheduler_enabled())
      ctx->st->thread_scheduler_disabled = false;

   /* Update the dispatch only if the dispatch is current. */
   if (_mesa_get_dispatch(ctx) == ctx->MarshalExec) {
       _mesa_set_dispatch(ctx, ctx->GLApi);
   }

   /* Unbind VBOs in all VAOs that glthread bound for non-VBO vertex uploads
    * to restore original states.
    */
   if (!_mesa_is_desktop_gl_core(ctx))
      _mesa_glthread_unbind_uploaded_vbos(ctx);
}

static void
glthread_finalize_batch(struct glthread_state *glthread,
                        unsigned *num_items_counter)
{
   struct glthread_batch *next = glthread->next_batch;

   /* Mark the end of the batch, but don't increment "used". */
   struct marshal_cmd_base *last =
      (struct marshal_cmd_base *)&next->buffer[glthread->used];
   last->cmd_id = NUM_DISPATCH_CMD;

   p_atomic_add(num_items_counter, glthread->used);
   next->used = glthread->used;
   glthread->used = 0;

   glthread->LastCallList = NULL;
   glthread->LastBindBuffer1 = NULL;
   glthread->LastBindBuffer2 = NULL;
}

void
_mesa_glthread_flush_batch(struct gl_context *ctx)
{
   struct glthread_state *glthread = &ctx->GLThread;
   if (!glthread->enabled)
      return;

   if (ctx->Dispatch.Current == ctx->Dispatch.ContextLost) {
      _mesa_glthread_disable(ctx);
      return;
   }

   if (!glthread->used)
      return; /* the batch is empty */

   glthread_apply_thread_sched_policy(ctx, false);
   glthread_finalize_batch(glthread, &glthread->stats.num_offloaded_items);

   struct glthread_batch *next = glthread->next_batch;
   const unsigned perf_words = next->used;
   const uint64_t perf_start_ns =
      glthread->perf_enabled ? os_time_get_nano() : 0;
   if (glthread->perf_enabled)
      next->perf_submit_ns = perf_start_ns;

   util_queue_add_job(&glthread->queue, next, &next->fence,
                      glthread_unmarshal_batch, NULL, 0);
   if (glthread->perf_enabled) {
      const uint64_t now_ns = os_time_get_nano();
      const uint64_t enqueue_ns = now_ns - perf_start_ns;
      p_atomic_inc(&glthread->perf.submit_batches);
      p_atomic_add(&glthread->perf.submit_words, perf_words);
      p_atomic_add(&glthread->perf.enqueue_ns, enqueue_ns);
      glthread_perf_atomic_max(&glthread->perf.enqueue_max_ns, enqueue_ns);
      glthread_perf_log(ctx, "periodic", false, now_ns);
   }
   glthread->last = glthread->next;
   glthread->next = (glthread->next + 1) % MARSHAL_MAX_BATCHES;
   glthread->next_batch = &glthread->batches[glthread->next];
}

/**
 * Waits for all pending batches have been unmarshaled.
 *
 * This can be used by the main thread to synchronize access to the context,
 * since the worker thread will be idle after this.
 */
void
_mesa_glthread_finish(struct gl_context *ctx)
{
   struct glthread_state *glthread = &ctx->GLThread;
   if (!glthread->enabled)
      return;

   /* If this is called from the worker thread, then we've hit a path that
    * might be called from either the main thread or the worker (such as some
    * dri interface entrypoints), in which case we don't need to actually
    * synchronize against ourself.
    */
   if (u_thread_is_self(glthread->queue.threads[0]))
      return;

   if (glthread->perf_enabled)
      p_atomic_inc(&glthread->perf.finish_calls);

   struct glthread_batch *last = &glthread->batches[glthread->last];
   struct glthread_batch *next = glthread->next_batch;
   bool synced = false;

   if (!util_queue_fence_is_signalled(&last->fence)) {
      const uint64_t perf_start_ns =
         glthread->perf_enabled ? os_time_get_nano() : 0;
      util_queue_fence_wait(&last->fence);
      if (glthread->perf_enabled) {
         const uint64_t elapsed_ns = os_time_get_nano() - perf_start_ns;
         p_atomic_inc(&glthread->perf.wait_calls);
         p_atomic_add(&glthread->perf.wait_ns, elapsed_ns);
         glthread_perf_atomic_max(&glthread->perf.wait_max_ns,
                                  elapsed_ns);
      }
      synced = true;
   }

   glthread_apply_thread_sched_policy(ctx, false);

   if (glthread->used) {
      glthread_finalize_batch(glthread, &glthread->stats.num_direct_items);

      /* Since glthread_unmarshal_batch changes the dispatch to direct,
       * restore it after it's done.
       */
      struct _glapi_table *dispatch = GET_DISPATCH();
      glthread_unmarshal_batch(next, NULL, 0);
      _mesa_glapi_set_dispatch(dispatch);

      /* It's not a sync because we don't enqueue partial batches, but
       * it would be a sync if we did. So count it anyway.
       */
      synced = true;
   }

   if (synced)
      p_atomic_inc(&glthread->stats.num_syncs);
}

void
_mesa_glthread_finish_before(struct gl_context *ctx, const char *func)
{
   _mesa_glthread_finish(ctx);

   /* Uncomment this if you want to know where glthread syncs. */
   /*printf("fallback to sync: %s\n", func);*/
}

void
_mesa_error_glthread_safe(struct gl_context *ctx, GLenum error, bool glthread,
                          const char *format, ...)
{
   if (glthread) {
      _mesa_marshal_InternalSetError(error);
   } else {
      char s[MAX_DEBUG_MESSAGE_LENGTH];
      va_list args;

      va_start(args, format);
      ASSERTED size_t len = vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, format, args);
      va_end(args);

      /* Whoever calls _mesa_error should use shorter strings. */
      assert(len < MAX_DEBUG_MESSAGE_LENGTH);

      _mesa_error(ctx, error, "%s", s);
   }
}

bool
_mesa_glthread_invalidate_zsbuf(struct gl_context *ctx)
{
   struct glthread_state *glthread = &ctx->GLThread;
   if (!glthread->enabled)
      return false;
   _mesa_marshal_InternalInvalidateFramebufferAncillaryMESA();
   return true;
}

void
_mesa_glthread_PixelStorei(struct gl_context *ctx, GLenum pname, GLint param)
{
   switch (pname) {
   case GL_UNPACK_SWAP_BYTES:
      ctx->GLThread.Unpack.SwapBytes = !!param;
      break;
   case GL_UNPACK_LSB_FIRST:
      ctx->GLThread.Unpack.LsbFirst = !!param;
      break;
   case GL_UNPACK_ROW_LENGTH:
      if (param >= 0)
         ctx->GLThread.Unpack.RowLength = param;
      break;
   case GL_UNPACK_IMAGE_HEIGHT:
      if (param >= 0)
         ctx->GLThread.Unpack.ImageHeight = param;
      break;
   case GL_UNPACK_SKIP_PIXELS:
      if (param >= 0)
         ctx->GLThread.Unpack.SkipPixels = param;
      break;
   case GL_UNPACK_SKIP_ROWS:
      if (param >= 0)
         ctx->GLThread.Unpack.SkipRows = param;
      break;
   case GL_UNPACK_SKIP_IMAGES:
      if (param >= 0)
         ctx->GLThread.Unpack.SkipImages = param;
      break;
   case GL_UNPACK_ALIGNMENT:
      if (param >= 1 && param <= 8 && util_is_power_of_two_nonzero(param))
         ctx->GLThread.Unpack.Alignment = param;
      break;
   case GL_UNPACK_COMPRESSED_BLOCK_WIDTH:
      if (param >= 0)
         ctx->GLThread.Unpack.CompressedBlockWidth = param;
      break;
   case GL_UNPACK_COMPRESSED_BLOCK_HEIGHT:
      if (param >= 0)
         ctx->GLThread.Unpack.CompressedBlockHeight = param;
      break;
   case GL_UNPACK_COMPRESSED_BLOCK_DEPTH:
      if (param >= 0)
         ctx->GLThread.Unpack.CompressedBlockDepth = param;
      break;
   case GL_UNPACK_COMPRESSED_BLOCK_SIZE:
      if (param >= 0)
         ctx->GLThread.Unpack.CompressedBlockSize = param;
      break;
   }
}
