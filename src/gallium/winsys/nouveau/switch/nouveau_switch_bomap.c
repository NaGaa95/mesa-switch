/*
 * Copyright 2026 Mesa Switch contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "nouveau_switch_libdrm.h"

static unsigned
bo_map_hash(const struct nouveau_bo *bo)
{
   return ((uintptr_t)bo >> 4) % NOUVEAU_SWITCH_BO_MAP_BUCKETS;
}

static struct nouveau_switch_client_bo_entry *
bo_map_lookup(struct nouveau_switch_client_bo_map *map,
              const struct nouveau_bo *bo)
{
   for (struct nouveau_switch_client_bo_entry *entry =
           map->buckets[bo_map_hash(bo)];
        entry; entry = entry->next) {
      if (entry->bo == bo)
         return entry;
   }

   return NULL;
}

static struct nouveau_switch_client_bo_entry *
bo_map_take_free(struct nouveau_switch_client_bo_map *map)
{
   struct nouveau_switch_client_bo_entry *entry =
      map->buckets[NOUVEAU_SWITCH_BO_MAP_BUCKETS];

   if (entry)
      map->buckets[NOUVEAU_SWITCH_BO_MAP_BUCKETS] = entry->next;
   else
      entry = malloc(sizeof(*entry));

   return entry;
}

void
nouveau_switch_client_map_finish(struct nouveau_client *client)
{
   if (!client)
      return;

   struct nouveau_switch_client *switch_client =
      nouveau_switch_client(client);

   simple_mtx_lock(&switch_client->lock);
   for (unsigned i = 0; i <= NOUVEAU_SWITCH_BO_MAP_BUCKETS; i++) {
      struct nouveau_switch_client_bo_entry *entry =
         switch_client->bo_map.buckets[i];
      while (entry) {
         struct nouveau_switch_client_bo_entry *next = entry->next;
         free(entry);
         entry = next;
      }
      switch_client->bo_map.buckets[i] = NULL;
   }
   simple_mtx_unlock(&switch_client->lock);
}

struct drm_nouveau_gem_pushbuf_bo *
nouveau_switch_client_kref_get(struct nouveau_client *client,
                               struct nouveau_bo *bo)
{
   if (!client || !bo)
      return NULL;

   struct nouveau_switch_client *switch_client =
      nouveau_switch_client(client);
   simple_mtx_lock(&switch_client->lock);
   struct nouveau_switch_client_bo_entry *entry =
      bo_map_lookup(&switch_client->bo_map, bo);
   struct drm_nouveau_gem_pushbuf_bo *kref = entry ? entry->kref : NULL;
   simple_mtx_unlock(&switch_client->lock);
   return kref;
}

struct nouveau_pushbuf *
nouveau_switch_client_push_get(struct nouveau_client *client,
                               struct nouveau_bo *bo)
{
   if (!client || !bo)
      return NULL;

   struct nouveau_switch_client *switch_client =
      nouveau_switch_client(client);
   simple_mtx_lock(&switch_client->lock);
   struct nouveau_switch_client_bo_entry *entry =
      bo_map_lookup(&switch_client->bo_map, bo);
   struct nouveau_pushbuf *push = entry ? entry->push : NULL;
   simple_mtx_unlock(&switch_client->lock);
   return push;
}

int
nouveau_switch_client_kref_set(
   struct nouveau_client *client, struct nouveau_bo *bo,
   struct drm_nouveau_gem_pushbuf_bo *kref, struct nouveau_pushbuf *push)
{
   if (!client || !bo)
      return -EINVAL;

   struct nouveau_switch_client *switch_client =
      nouveau_switch_client(client);
   struct nouveau_switch_client_bo_map *map = &switch_client->bo_map;

   simple_mtx_lock(&switch_client->lock);
   struct nouveau_switch_client_bo_entry *entry = bo_map_lookup(map, bo);

   if (!entry) {
      if (!kref && !push)
         goto out;

      entry = bo_map_take_free(map);
      if (!entry) {
         simple_mtx_unlock(&switch_client->lock);
         return -ENOMEM;
      }

      const unsigned hash = bo_map_hash(bo);
      entry->next = map->buckets[hash];
      if (entry->next)
         entry->next->prev_next = &entry->next;
      entry->prev_next = &map->buckets[hash];
      entry->bo = bo;
      map->buckets[hash] = entry;
   }

   if (kref || push) {
      entry->kref = kref;
      entry->push = push;
   } else {
      *entry->prev_next = entry->next;
      if (entry->next)
         entry->next->prev_next = entry->prev_next;

      entry->next = map->buckets[NOUVEAU_SWITCH_BO_MAP_BUCKETS];
      map->buckets[NOUVEAU_SWITCH_BO_MAP_BUCKETS] = entry;
   }

out:
   simple_mtx_unlock(&switch_client->lock);
   return 0;
}
