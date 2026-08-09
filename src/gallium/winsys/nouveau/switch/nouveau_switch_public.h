
#ifndef __NOUVEAU_SWITCH_PUBLIC_H__
#define __NOUVEAU_SWITCH_PUBLIC_H__
#include <stdbool.h>
#include <switch.h>

struct pipe_screen;
struct pipe_resource;
struct nouveau_object;
struct nouveau_pushbuf;

struct pipe_screen *nouveau_switch_screen_create(void);
int nouveau_switch_resource_get_syncpoint(struct pipe_resource *resource, unsigned int *out_threshold);
int nouveau_switch_resource_get_buffer(struct pipe_resource *resource, NvGraphicBuffer *buffer);
int nouveau_switch_pushbuf_kick_deferred(struct nouveau_pushbuf *push,
                                         struct nouveau_object *chan);
void nouveau_switch_pushbuf_set_kick_notify(
   struct nouveau_pushbuf *push,
   bool (*kick_notify)(struct nouveau_pushbuf *));
uint64_t nouveau_switch_pushbuf_batch_generation(struct nouveau_pushbuf *push);

#endif
