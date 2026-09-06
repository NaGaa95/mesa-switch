/*
 * Copyright 2010 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "nouveau_screen.h"
#include "nouveau_context.h"
#include "nouveau_winsys.h"
#include "nouveau_fence.h"
#include "util/os_time.h"

#if DETECT_OS_POSIX
#include <sched.h>
#endif

static bool
_nouveau_fence_wait(struct nouveau_fence *fence,
                    struct util_debug_callback *debug, uint64_t timeout_ns);

bool
nouveau_fence_new(struct nouveau_context *nv, struct nouveau_fence **fence)
{
   struct nouveau_fence *new_fence = CALLOC_STRUCT(nouveau_fence);
   if (!new_fence)
      return false;

#ifndef __SWITCH__
   int ret = nouveau_bo_new(nv->screen->device, NOUVEAU_BO_GART, 0x1000, 0x1000, NULL, &new_fence->bo);
   if (ret) {
      FREE(new_fence);
      return false;
   }
#endif

   new_fence->screen = nv->screen;
   new_fence->context = nv;
   new_fence->ref = 1;
   list_inithead(&new_fence->work);

   *fence = new_fence;
   return true;
}

static void
nouveau_fence_trigger_work(struct nouveau_fence *fence)
{
   simple_mtx_assert_locked(&fence->screen->fence.lock);

   struct nouveau_fence_work *work, *tmp;

   LIST_FOR_EACH_ENTRY_SAFE(work, tmp, &fence->work, list) {
      work->func(work->data);
      list_del(&work->list);
      FREE(work);
   }
}

#ifdef __SWITCH__
static void
nouveau_fence_quarantine_work(struct nouveau_fence *fence)
{
   simple_mtx_assert_locked(&fence->screen->fence.lock);

   struct nouveau_fence_work *work, *tmp;

   /* Retain work->data until GPU completion is known: callbacks may free
    * live storage. The callback nodes own no GPU storage and can be
    * discarded.
    */
   LIST_FOR_EACH_ENTRY_SAFE(work, tmp, &fence->work, list) {
      list_del(&work->list);
      FREE(work);
   }
}
#endif

static void
_nouveau_fence_emit(struct nouveau_fence *fence)
{
   struct nouveau_screen *screen = fence->screen;
   struct nouveau_fence_list *fence_list = &screen->fence;

   simple_mtx_assert_locked(&fence_list->lock);

   assert(fence->state != NOUVEAU_FENCE_STATE_EMITTING);
   if (fence->state >= NOUVEAU_FENCE_STATE_EMITTED)
      return;

   /* set this now, so that if fence.emit triggers a flush we don't recurse */
   fence->state = NOUVEAU_FENCE_STATE_EMITTING;

   p_atomic_inc(&fence->ref);

   if (fence_list->tail)
      fence_list->tail->next = fence;
   else
      fence_list->head = fence;

   fence_list->tail = fence;

#ifdef __SWITCH__
   fence->batch_cookie = ++fence_list->next_batch_cookie;
   if (fence->batch_cookie == 0)
      fence->batch_cookie = ++fence_list->next_batch_cookie;
   fence_list->emit(&fence->context->pipe, &fence->sequence, NULL);
#else
   fence_list->emit(&fence->context->pipe, &fence->sequence, fence->bo);
#endif

   assert(fence->state == NOUVEAU_FENCE_STATE_EMITTING);
   fence->state = NOUVEAU_FENCE_STATE_EMITTED;
}

static void
nouveau_fence_del(struct nouveau_fence *fence)
{
   struct nouveau_fence *it;
   struct nouveau_fence_list *fence_list = &fence->screen->fence;

   simple_mtx_assert_locked(&fence_list->lock);

   if (fence->state == NOUVEAU_FENCE_STATE_EMITTED ||
       fence->state == NOUVEAU_FENCE_STATE_FLUSHED) {
      if (fence == fence_list->head) {
         fence_list->head = fence->next;
         if (!fence_list->head)
            fence_list->tail = NULL;
      } else {
         for (it = fence_list->head; it && it->next != fence; it = it->next);
         it->next = fence->next;
         if (fence_list->tail == fence)
            fence_list->tail = it;
      }
   }

   if (!list_is_empty(&fence->work)) {
#ifdef __SWITCH__
      if (fence->state != NOUVEAU_FENCE_STATE_SIGNALLED) {
         _debug_printf("nouveau/switch: deleting unsignalled fence %u with "
                       "%u pending callbacks; quarantining callback data\n",
                       fence->sequence, fence->work_count);
         nouveau_fence_quarantine_work(fence);
      } else
#endif
      {
         debug_printf("WARNING: deleting fence with work still pending !\n");
         nouveau_fence_trigger_work(fence);
      }
   }

#ifndef __SWITCH__
   nouveau_bo_ref(NULL, &fence->bo);
#endif
   FREE(fence);
}

void
nouveau_fence_cleanup(struct nouveau_context *nv)
{
#ifdef __SWITCH__
   struct nouveau_fence_list *fence_list = &nv->screen->fence;
   struct nouveau_fence *drain = NULL;
   struct nouveau_fence *candidate = NULL;
   bool completed = true;

   /* Replacement-fence OOM can leave nv->fence NULL while an emitted,
    * list-owned fence still references nv. Detach fences from both
    * ownership locations.
    */
   nouveau_screen_submission_lock(nv->screen);
   simple_mtx_lock(&fence_list->lock);

   if (nv->fence) {
      _nouveau_fence_ref(nv->fence, &drain);
   } else {
      /* Fence-list order is submission order.  Waiting the last entry owned
       * by this context covers all of its earlier logical records while the
       * context is still live and can be rebound for a forced kickoff. */
      for (struct nouveau_fence *fence = fence_list->head;
           fence; fence = fence->next) {
         if (fence->context == nv)
            candidate = fence;
      }
      if (candidate)
         _nouveau_fence_ref(candidate, &drain);
   }

   if (drain)
      completed = _nouveau_fence_wait(drain, NULL, UINT64_MAX);

   /* Fences can outlive their context through the screen-owned Switch
    * pushbuf. Detach unsignaled entries before freeing the context. Keep a
    * local reference across waits and failed emission, which may remove
    * the fence from the screen list.
    */
   for (struct nouveau_fence *fence = fence_list->head;
        fence; fence = fence->next) {
      if (fence->context == nv)
         fence->context = NULL;
   }
   if (drain && drain->context == nv)
      drain->context = NULL;
   /* A successful wait can leave a new, un-emitted nv->fence outside
    * fence_list. Detach it before dropping the context reference to avoid
    * a stale nv pointer.
    */
   if (nv->fence && nv->fence->context == nv)
      nv->fence->context = NULL;
   if (!completed) {
      nv->screen->fence_teardown_quarantined = true;
      _debug_printf("nouveau/switch: context fence drain failed; "
                    "detaching outstanding fences and quarantining the "
                    "screen ownership graph\n");
   }

   _nouveau_fence_ref(NULL, &drain);
   _nouveau_fence_ref(NULL, &nv->fence);
   simple_mtx_unlock(&fence_list->lock);
   nouveau_screen_submission_unlock(nv->screen);
#else
   if (nv->fence) {
      struct nouveau_fence_list *fence_list = &nv->screen->fence;
      struct nouveau_fence *current = NULL;

      /* nouveau_fence_wait will create a new current fence, so wait on the
       * _current_ one, and remove both.
       */
      simple_mtx_lock(&fence_list->lock);
      _nouveau_fence_ref(nv->fence, &current);
      _nouveau_fence_wait(current, NULL, UINT64_MAX);
      _nouveau_fence_ref(NULL, &current);
      _nouveau_fence_ref(NULL, &nv->fence);
      simple_mtx_unlock(&fence_list->lock);
   }
#endif
}

void
_nouveau_fence_update(struct nouveau_screen *screen, bool flushed)
{
   struct nouveau_fence *fence;
   struct nouveau_fence *next = NULL;
   struct nouveau_fence_list *fence_list = &screen->fence;
   u32 sequence = fence_list->update(&screen->base);

   simple_mtx_assert_locked(&fence_list->lock);

   /* If running under drm-shim, let all fences be signalled so things run to
    * completion (avoids a hang at the end of shader-db).
    */
   if (unlikely(screen->disable_fences))
      sequence = screen->fence.sequence;

   if (fence_list->sequence_ack == sequence)
      return;
   fence_list->sequence_ack = sequence;

   for (fence = fence_list->head; fence; fence = next) {
      next = fence->next;
      sequence = fence->sequence;

      fence->state = NOUVEAU_FENCE_STATE_SIGNALLED;

      nouveau_fence_trigger_work(fence);
      _nouveau_fence_ref(NULL, &fence);

      if (sequence == fence_list->sequence_ack)
         break;
   }
   fence_list->head = next;
   if (!next)
      fence_list->tail = NULL;

   if (flushed) {
      for (fence = next; fence; fence = fence->next)
         if (fence->state == NOUVEAU_FENCE_STATE_EMITTED)
            fence->state = NOUVEAU_FENCE_STATE_FLUSHED;
   }
}

static bool
_nouveau_fence_signalled(struct nouveau_fence *fence)
{
   struct nouveau_screen *screen = fence->screen;

   simple_mtx_assert_locked(&screen->fence.lock);

   if (fence->state == NOUVEAU_FENCE_STATE_SIGNALLED)
      return true;

   if (fence->state >= NOUVEAU_FENCE_STATE_EMITTED)
      _nouveau_fence_update(screen, false);

   return fence->state == NOUVEAU_FENCE_STATE_SIGNALLED;
}

static bool
nouveau_fence_kick(struct nouveau_fence *fence)
{
   struct nouveau_context *context = fence->context;
   struct nouveau_screen *screen = fence->screen;
   struct nouveau_fence_list *fence_list = &screen->fence;
   const bool current =
      fence->state < NOUVEAU_FENCE_STATE_EMITTING && context &&
      context->fence == fence;
#ifdef __SWITCH__
   struct nouveau_pushbuf *push = context ? context->pushbuf : screen->pushbuf;
#endif

   simple_mtx_assert_locked(&fence_list->lock);

#ifdef __SWITCH__
   /* Rebind the fence's context before emission or kickoff can invoke
    * kick_notify. The caller holds the recursive submission lock before
    * fence.lock.
    */
   if (context)
      nouveau_pushbuf_bind_context(push, context);
#endif

   /* wtf, someone is waiting on a fence in flush_notify handler? */
   assert(fence->state != NOUVEAU_FENCE_STATE_EMITTING);

   if (fence->state < NOUVEAU_FENCE_STATE_EMITTED) {
#ifdef __SWITCH__
      /* Only a live owning context can emit a logical fence command.  Cleanup
       * detaches a fence whose emission failed, making later waits fail closed
       * instead of touching freed context memory. */
      if (!context || !push)
         return false;
#endif
      if (PUSH_AVAIL(context->pushbuf) < 16 &&
          nouveau_pushbuf_space(context->pushbuf, 16, 0, 0))
         return false;
      _nouveau_fence_emit(fence);
   }

#ifdef __SWITCH__
   /* FLUSHED may mean only software-batched on Switch. Kick off work
    * without a physical completion so each logical fence receives its
    * batch's native fence.
    */
   if (fence->state < NOUVEAU_FENCE_STATE_SIGNALLED &&
       !fence->native_fence_valid) {
      /* A detached emitted fence has no live context to drive kick_notify.
       * Its exact native completion must already have been published; if it
       * was not, completion is unknown and the wait must fail closed. */
      if (!context || !push)
         return false;
      if (nouveau_pushbuf_kick(push, push->channel))
         return false;
      /* The physical-submit callback owns exact association.  A global
       * last-fence fallback can attach a prior/no-op/CPU-upgrade completion to
       * this logical fence and defeat the callback's fail-closed contract. */
      if (!fence->native_fence_valid)
         return false;
   }
#else
   if (fence->state < NOUVEAU_FENCE_STATE_FLUSHED) {
      if (nouveau_pushbuf_kick(context->pushbuf))
         return false;
   }
#endif

   if (current) {
#ifdef __SWITCH__
      if (!context)
         return false;
#endif
      if (!_nouveau_fence_next(fence->context))
         return false;
   }

   _nouveau_fence_update(screen, false);

   return true;
}

static bool
_nouveau_fence_wait(struct nouveau_fence *fence,
                    struct util_debug_callback *debug, uint64_t timeout_ns)
{
   struct nouveau_screen *screen = fence->screen;
   struct nouveau_fence_list *fence_list = &screen->fence;
   int64_t start = 0;

   simple_mtx_assert_locked(&fence_list->lock);

   if (debug && debug->debug_message)
      start = os_time_get_nano();

   /* Fence handles may outlive their contexts.  In particular, do not enter
    * nouveau_fence_kick merely to rediscover that an already completed fence
    * is signalled: the owning context may have been destroyed meanwhile. */
   if (fence->state == NOUVEAU_FENCE_STATE_SIGNALLED)
      return true;

   if (!nouveau_fence_kick(fence))
      return false;

#ifdef __SWITCH__
   /* Poll the coherent sequence word without allocating libnx events.
    * Blocking waits use the batch's native fence, never an unbounded
    * shared-word spin.
    */
   for (uint32_t poll = 0; poll < 32; poll++) {
      _nouveau_fence_update(screen, false);
      if (fence->state == NOUVEAU_FENCE_STATE_SIGNALLED)
         goto wait_complete;
   }

   NOUVEAU_DRV_STAT(screen, any_non_kernel_fence_sync_count, 1);
   if (timeout_ns == 0)
      return false;

   if (!fence->native_fence_valid) {
      _debug_printf("nouveau/switch: fence %u has no native completion "
                    "(ack=%u next=%u)\n",
                    fence->sequence, fence_list->sequence_ack,
                    fence_list->sequence);
      return false;
   }

   if (!fence->native_fence_cpu_visible) {
      NvFence cpu_fence = { .id = UINT32_MAX };
      const int upgrade_ret =
         nouveau_switch_pushbuf_upgrade_physical_cpu_fence(
            screen->pushbuf, &fence->native_fence, &cpu_fence);
      if (upgrade_ret) {
         _debug_printf("nouveau/switch: failed to upgrade fence %u for "
                       "CPU wait: %d (native=%u:%u)\n",
                       fence->sequence, upgrade_ret,
                       fence->native_fence.id, fence->native_fence.value);
         return false;
      }
      fence->native_fence = cpu_fence;
      fence->native_fence_cpu_visible = true;
      fence->state = NOUVEAU_FENCE_STATE_FLUSHED;
   }

   const int ret = nouveau_switch_pushbuf_wait_fence(
      screen->pushbuf, &fence->native_fence, timeout_ns);
   if (ret) {
      _debug_printf("nouveau/switch: wait on fence %u failed: %d "
                    "(ack=%u next=%u native=%u:%u)\n",
                    fence->sequence, ret, fence_list->sequence_ack,
                    fence_list->sequence, fence->native_fence.id,
                    fence->native_fence.value);
      return false;
   }

   /* Allow a bounded interval for the sequence write to become visible
    * after native completion; a premature failure would propagate to GL
    * waits and retain the screen at teardown.
    */
   const int64_t settle_deadline_ns =
      os_time_get_nano() + 10 * 1000 * 1000;
   for (;;) {
      _nouveau_fence_update(screen, false);
      if (fence->state == NOUVEAU_FENCE_STATE_SIGNALLED)
         goto wait_complete;
      if (os_time_get_nano() >= settle_deadline_ns)
         break;
      svcSleepThread(10 * 1000);
   }

   _debug_printf("nouveau/switch: native fence completed but Gallium fence "
                 "%u did not (ack=%u next=%u native=%u:%u)\n",
                 fence->sequence, fence_list->sequence_ack,
                 fence_list->sequence, fence->native_fence.id,
                 fence->native_fence.value);
   return false;

wait_complete:
   if (debug && debug->debug_message)
      util_debug_message(debug, PERF_INFO,
                         "stalled %.3f ms waiting for fence",
                         (os_time_get_nano() - start) / 1000000.f);
   return true;
#else
   (void)timeout_ns;
   if (fence->state < NOUVEAU_FENCE_STATE_SIGNALLED) {
      NOUVEAU_DRV_STAT(screen, any_non_kernel_fence_sync_count, 1);
      int ret = nouveau_bo_wait(fence->bo, NOUVEAU_BO_RDWR, screen->client);
      if (ret) {
         debug_printf("Wait on fence %u (ack = %u, next = %u) errored with %s !\n",
                      fence->sequence,
                      fence_list->sequence_ack, fence_list->sequence, strerror(ret));
         return false;
      }

      _nouveau_fence_update(screen, false);
      if (fence->state != NOUVEAU_FENCE_STATE_SIGNALLED)
         return false;

      if (debug && debug->debug_message)
         util_debug_message(debug, PERF_INFO,
                            "stalled %.3f ms waiting for fence",
                            (os_time_get_nano() - start) / 1000000.f);
   }

   return true;
#endif
}

bool
_nouveau_fence_next(struct nouveau_context *nv)
{
   struct nouveau_fence_list *fence_list = &nv->screen->fence;

   simple_mtx_assert_locked(&fence_list->lock);

   /* Allocation failure leaves nv->fence NULL. Propagate the failure;
    * teardown drains the remaining work or quarantines its resources.
    */
   if (!nv->fence)
      return false;

   if (nv->fence->state < NOUVEAU_FENCE_STATE_EMITTING) {
      if (p_atomic_read(&nv->fence->ref) > 1)
         _nouveau_fence_emit(nv->fence);
      else
         return true;
   }

   _nouveau_fence_ref(NULL, &nv->fence);

   return nouveau_fence_new(nv, &nv->fence);
}

void
nouveau_fence_unref_bo(void *data)
{
   struct nouveau_bo *bo = data;

   nouveau_bo_ref(NULL, &bo);
}

bool
nouveau_fence_work(struct nouveau_fence *fence,
                   void (*func)(void *), void *data)
{
   struct nouveau_fence_work *work;
   struct nouveau_screen *screen;

   if (!fence || fence->state == NOUVEAU_FENCE_STATE_SIGNALLED) {
      func(data);
      return true;
   }

   work = CALLOC_STRUCT(nouveau_fence_work);
   if (!work)
      return false;
   work->func = func;
   work->data = data;

   /* the fence might get deleted by fence_kick */
   screen = fence->screen;

#ifdef __SWITCH__
   nouveau_screen_submission_lock(screen);
#endif
   simple_mtx_lock(&screen->fence.lock);
   list_add(&work->list, &fence->work);
   if (++fence->work_count > 64)
      nouveau_fence_kick(fence);
   simple_mtx_unlock(&screen->fence.lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(screen);
#endif
   return true;
}

void
_nouveau_fence_ref(struct nouveau_fence *fence, struct nouveau_fence **ref)
{
   if (fence)
      p_atomic_inc(&fence->ref);

   if (*ref) {
      simple_mtx_assert_locked(&(*ref)->screen->fence.lock);
      if (p_atomic_dec_zero(&(*ref)->ref))
         nouveau_fence_del(*ref);
   }

   *ref = fence;
}

void
nouveau_fence_ref(struct nouveau_fence *fence, struct nouveau_fence **ref,
                  struct nouveau_screen *screen)
{
   struct nouveau_fence_list *fence_list = &screen->fence;

   simple_mtx_lock(&fence_list->lock);
   _nouveau_fence_ref(fence, ref);
   simple_mtx_unlock(&fence_list->lock);
}

bool
nouveau_fence_wait(struct nouveau_fence *fence, struct util_debug_callback *debug)
{
   return nouveau_fence_wait_timeout(fence, debug, UINT64_MAX);
}

bool
nouveau_fence_wait_timeout(struct nouveau_fence *fence,
                           struct util_debug_callback *debug,
                           uint64_t timeout_ns)
{
   struct nouveau_screen *screen = fence->screen;
   struct nouveau_fence_list *fence_list = &screen->fence;
#ifdef __SWITCH__
   nouveau_screen_submission_lock(screen);
#endif
   simple_mtx_lock(&fence_list->lock);
   bool res = _nouveau_fence_wait(fence, debug, timeout_ns);
   simple_mtx_unlock(&fence_list->lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(screen);
#endif
   return res;
}

bool
nouveau_fence_next_if_current(struct nouveau_context *nv, struct nouveau_fence *fence)
{
   bool result = true;
#ifdef __SWITCH__
   nouveau_screen_submission_lock(fence->screen);
#endif
   simple_mtx_lock(&fence->screen->fence.lock);
   if (nv->fence == fence)
      result = _nouveau_fence_next(nv);
   simple_mtx_unlock(&fence->screen->fence.lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(fence->screen);
#endif
   return result;
}

bool
nouveau_fence_signalled(struct nouveau_fence *fence)
{
   struct nouveau_screen *screen = fence->screen;
#ifdef __SWITCH__
   nouveau_screen_submission_lock(screen);
#endif
   simple_mtx_lock(&screen->fence.lock);
   bool ret = _nouveau_fence_signalled(fence);
   simple_mtx_unlock(&screen->fence.lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(screen);
#endif
   return ret;
}
