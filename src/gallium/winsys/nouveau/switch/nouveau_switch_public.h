
#ifndef __NOUVEAU_SWITCH_PUBLIC_H__
#define __NOUVEAU_SWITCH_PUBLIC_H__
#include <stdbool.h>
#include <switch.h>

struct pipe_screen;
struct pipe_context;
struct pipe_resource;
struct nouveau_object;
struct nouveau_pushbuf;

struct pipe_screen *nouveau_switch_screen_create(void);
int nouveau_switch_resource_get_syncpoint(struct pipe_resource *resource, unsigned int *out_threshold);
int nouveau_switch_resource_get_buffer(struct pipe_resource *resource, NvGraphicBuffer *buffer);
int nouveau_switch_context_wait_nvmultifence(
   struct pipe_context *context, const NvMultiFence *fence);
int nouveau_switch_context_get_error(struct pipe_context *context);
int nouveau_switch_context_finish_required(struct pipe_context *context,
                                           uint64_t timeout_ns,
                                           const char *reason);
int nouveau_switch_context_get_cpu_fence(struct pipe_context *context,
                                         NvFence *out_fence);
int nouveau_switch_pushbuf_kick_deferred(struct nouveau_pushbuf *push,
                                         struct nouveau_object *chan);
int nouveau_switch_pushbuf_kick_full_barrier(
   struct nouveau_pushbuf *push, struct nouveau_object *chan);
int nouveau_switch_pushbuf_enqueue_nvmultifence(
   struct nouveau_pushbuf *push, const NvMultiFence *waits);
bool nouveau_switch_pushbuf_get_last_fence(
   struct nouveau_pushbuf *push, NvFence *out_fence,
   bool *cpu_visible_out);
int nouveau_switch_pushbuf_get_cpu_fence(
   struct nouveau_pushbuf *push, NvFence *out_fence);
int nouveau_switch_pushbuf_upgrade_physical_cpu_fence(
   struct nouveau_pushbuf *push, const NvFence *physical_fence,
   NvFence *out_fence);
int nouveau_switch_pushbuf_kick_cpu(struct nouveau_pushbuf *push,
                                    struct nouveau_object *chan);
int nouveau_switch_pushbuf_wait_fence(struct nouveau_pushbuf *push,
                                      const NvFence *fence,
                                      uint64_t timeout_ns);
int nouveau_switch_pushbuf_wait_fence_required(
   struct nouveau_pushbuf *push, const NvFence *fence, uint64_t timeout_ns,
   const char *reason);
int nouveau_switch_pushbuf_get_error(struct nouveau_pushbuf *push);
int nouveau_switch_pushbuf_enable_mapped_completion(
   struct nouveau_pushbuf *push);
bool nouveau_switch_pushbuf_channel_lost(struct nouveau_pushbuf *push);
bool nouveau_switch_pushbuf_device_lost(struct nouveau_pushbuf *push);
void nouveau_switch_pushbuf_set_kick_notify(
   struct nouveau_pushbuf *push,
   bool (*kick_notify)(struct nouveau_pushbuf *));
void nouveau_switch_pushbuf_set_native_kick_notify(
   struct nouveau_pushbuf *push,
   uint32_t (*native_kick_notify)(struct nouveau_pushbuf *,
                                   const NvFence *, bool cpu_visible));
/* marker_notify captures the logical fence for the current record. Publish
 * it only after the full record joins the physical batch, then pass it to
 * native_notify after Horizon accepts that batch.
 */
void nouveau_switch_pushbuf_set_fence_batch_notify(
   struct nouveau_pushbuf *push,
   bool (*marker_notify)(struct nouveau_pushbuf *, uint64_t *cookie_out),
   uint32_t (*native_notify)(struct nouveau_pushbuf *, const NvFence *,
                             bool cpu_visible, uint64_t cookie));
uint64_t nouveau_switch_pushbuf_batch_generation(struct nouveau_pushbuf *push);

#endif
