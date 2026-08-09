/*
 * Private push-buffer ABI shared with devkitPro libdrm_nouveau 1.0.1.
 */

#ifndef NOUVEAU_SWITCH_ABI_H
#define NOUVEAU_SWITCH_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <switch.h>

#include "nouveau.h"
#include "nouveau_drm.h"

typedef struct {
   int atomic;
} nouveau_switch_atomic_t;

struct nouveau_bo_priv {
   struct nouveau_bo base;
   nouveau_switch_atomic_t refcnt;
   void *map_addr;
   uint32_t name;
   uint32_t access;
   NvMap map;
   NvFence fence;
};

static inline struct nouveau_bo_priv *
nouveau_switch_bo(struct nouveau_bo *bo)
{
   return (struct nouveau_bo_priv *)bo;
}

struct nouveau_device_priv {
   struct nouveau_device base;
   uint32_t *client;
   int nr_client;
   Mutex lock;
   NvAddressSpace addr_space;
};

static inline struct nouveau_device_priv *
nouveau_switch_device(struct nouveau_device *dev)
{
   return (struct nouveau_device_priv *)dev;
}

struct drm_nouveau_gem_pushbuf_bo *
cli_kref_get(struct nouveau_client *client, struct nouveau_bo *bo);

struct nouveau_pushbuf *
cli_push_get(struct nouveau_client *client, struct nouveau_bo *bo);

void
cli_kref_set(struct nouveau_client *client, struct nouveau_bo *bo,
             struct drm_nouveau_gem_pushbuf_bo *kref,
             struct nouveau_pushbuf *push);

#define NS_LIST_INIT(item)                                                   \
   do {                                                                      \
      (item)->prev = (item);                                                 \
      (item)->next = (item);                                                 \
   } while (0)

#define NS_LIST_ADD(item, list)                                             \
   do {                                                                      \
      (item)->prev = (list);                                                 \
      (item)->next = (list)->next;                                           \
      (list)->next->prev = (item);                                           \
      (list)->next = (item);                                                 \
   } while (0)

#define NS_LIST_DEL(item)                                                   \
   do {                                                                      \
      (item)->prev->next = (item)->next;                                     \
      (item)->next->prev = (item)->prev;                                     \
   } while (0)

#define NS_LIST_DEL_INIT(item)                                              \
   do {                                                                      \
      NS_LIST_DEL(item);                                                     \
      NS_LIST_INIT(item);                                                    \
   } while (0)

#define NS_LIST_ENTRY(type, item, field)                                    \
   ((type *)((char *)(item) - offsetof(type, field)))

#define NS_LIST_EMPTY(item) ((item)->next == (item))

#define NS_LIST_FOR_EACH_ENTRY(item, list, field)                           \
   for ((item) = NS_LIST_ENTRY(__typeof__(*(item)), (list)->next, field);    \
        &(item)->field != (list);                                            \
        (item) = NS_LIST_ENTRY(__typeof__(*(item)), (item)->field.next,      \
                               field))

#define NS_LIST_FOR_EACH_ENTRY_SAFE(item, temp, list, field)                \
   for ((item) = NS_LIST_ENTRY(__typeof__(*(item)), (list)->next, field),    \
        (temp) = NS_LIST_ENTRY(__typeof__(*(item)), (item)->field.next,      \
                               field);                                       \
        &(item)->field != (list);                                            \
        (item) = (temp),                                                     \
        (temp) = NS_LIST_ENTRY(__typeof__(*(item)), (temp)->field.next,      \
                               field))

#define NS_LIST_JOIN(list, join)                                            \
   do {                                                                      \
      if (!NS_LIST_EMPTY(list)) {                                            \
         (list)->next->prev = (join);                                        \
         (list)->prev->next = (join)->next;                                  \
         (join)->next->prev = (list)->prev;                                  \
         (join)->next = (list)->next;                                        \
      }                                                                      \
   } while (0)

_Static_assert(offsetof(struct nouveau_bo_priv, access) == 100,
               "libdrm_nouveau BO access ABI changed");
_Static_assert(offsetof(struct nouveau_bo_priv, fence) == 136,
               "libdrm_nouveau BO fence ABI changed");

#endif
