/*
 * Copyright 2012 Red Hat Inc.
 * Copyright 2026 Mesa Switch contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nouveau_switch_libdrm.h"

struct nouveau_switch_bufref {
   struct nouveau_bufref base;
   struct nouveau_switch_bufref *next;
};

struct nouveau_switch_bufbin {
   struct nouveau_switch_bufref *list;
   int relocs;
};

struct nouveau_switch_bufctx {
   struct nouveau_bufctx base;
   struct nouveau_switch_bufref *free;
   int nr_bins;
   struct nouveau_switch_bufbin bins[];
};

static inline struct nouveau_switch_bufctx *
switch_bufctx(struct nouveau_bufctx *bufctx)
{
   return (struct nouveau_switch_bufctx *)bufctx;
}

int
nouveau_bufctx_new(struct nouveau_client *client, int bins,
                   struct nouveau_bufctx **out)
{
   if (!out || bins <= 0)
      return -EINVAL;

   struct nouveau_switch_bufctx *ctx =
      calloc(1, sizeof(*ctx) + sizeof(ctx->bins[0]) * bins);
   if (!ctx)
      return -ENOMEM;

   NS_LIST_INIT(&ctx->base.head);
   NS_LIST_INIT(&ctx->base.pending);
   NS_LIST_INIT(&ctx->base.current);
   ctx->base.client = client;
   ctx->nr_bins = bins;
   *out = &ctx->base;
   return 0;
}

void
nouveau_bufctx_reset(struct nouveau_bufctx *bufctx, int bin)
{
   if (!bufctx)
      return;

   struct nouveau_switch_bufctx *ctx = switch_bufctx(bufctx);
   if (bin < 0 || bin >= ctx->nr_bins)
      return;

   struct nouveau_switch_bufbin *bufbin = &ctx->bins[bin];
   while (bufbin->list) {
      struct nouveau_switch_bufref *ref = bufbin->list;
      NS_LIST_DEL_INIT(&ref->base.thead);
      bufbin->list = ref->next;
      ref->next = ctx->free;
      ctx->free = ref;
   }

   bufctx->relocs -= bufbin->relocs;
   bufbin->relocs = 0;
}

void
nouveau_bufctx_del(struct nouveau_bufctx **pbufctx)
{
   if (!pbufctx || !*pbufctx)
      return;

   struct nouveau_switch_bufctx *ctx = switch_bufctx(*pbufctx);
   while (ctx->nr_bins > 0) {
      nouveau_bufctx_reset(&ctx->base, ctx->nr_bins - 1);
      ctx->nr_bins--;
   }

   while (ctx->free) {
      struct nouveau_switch_bufref *next = ctx->free->next;
      free(ctx->free);
      ctx->free = next;
   }

   free(ctx);
   *pbufctx = NULL;
}

struct nouveau_bufref *
nouveau_bufctx_refn(struct nouveau_bufctx *bufctx, int bin,
                    struct nouveau_bo *bo, uint32_t flags)
{
   if (!bufctx || !bo)
      return NULL;

   struct nouveau_switch_bufctx *ctx = switch_bufctx(bufctx);
   if (bin < 0 || bin >= ctx->nr_bins)
      return NULL;

   struct nouveau_switch_bufbin *bufbin = &ctx->bins[bin];
   struct nouveau_switch_bufref *ref = ctx->free;
   if (ref)
      ctx->free = ref->next;
   else
      ref = malloc(sizeof(*ref));

   if (!ref)
      return NULL;

   memset(&ref->base, 0, sizeof(ref->base));
   ref->base.bo = bo;
   ref->base.flags = flags;
   NS_LIST_INIT(&ref->base.thead);
   NS_LIST_ADD(&ref->base.thead, bufctx->pending.prev);
   ref->next = bufbin->list;
   bufbin->list = ref;
   return &ref->base;
}

struct nouveau_bufref *
nouveau_bufctx_mthd(struct nouveau_bufctx *bufctx, int bin, uint32_t packet,
                    struct nouveau_bo *bo, uint64_t data, uint32_t flags,
                    uint32_t vor, uint32_t tor)
{
   struct nouveau_bufref *ref =
      nouveau_bufctx_refn(bufctx, bin, bo, flags);
   if (!ref)
      return NULL;

   struct nouveau_switch_bufctx *ctx = switch_bufctx(bufctx);
   ref->packet = packet;
   ref->data = data;
   ref->vor = vor;
   ref->tor = tor;
   ctx->bins[bin].relocs++;
   bufctx->relocs++;
   return ref;
}
