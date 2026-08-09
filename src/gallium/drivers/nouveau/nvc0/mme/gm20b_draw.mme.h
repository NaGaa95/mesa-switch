/*
 * GM20B direct draw MME programs.
 * The integration and zero-base dispatch restrictions are Mesa-specific.
 *
 * Copyright (C) 2018-2020 fincs
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#ifndef NVC0_GM20B_DRAW_MME_H
#define NVC0_GM20B_DRAW_MME_H

#include <stdint.h>

static const uint32_t gm20b_mme_draw[] = {
   0x00000601, 0x00004211, 0x00000701, 0x00d74061,
   0x0000b897, 0x01438551, 0x00016807, 0x00002841,
   0x0638c021, 0x00010041, 0x00002841, 0x01618021,
   0x00000841, 0x00d78021, 0x00003041, 0x01614071,
   0xffffff11, 0xfffeb817, 0xd0808912, 0x0000a897,
   0x0638c021, 0x000100c1, 0x00000041,
};

static const uint32_t gm20b_mme_draw_indexed[] = {
   0x00000601, 0x00004211, 0x00000701, 0x017dc061,
   0x00000301, 0x0000b897, 0x05434451, 0x00001841,
   0x00002041, 0x01118021, 0x00001841, 0x00131d10,
   0x00016807, 0x0638c021, 0x00000041, 0x00001841,
   0x00002041, 0x01618021, 0x00000841, 0x017e0021,
   0x00003041, 0x01614071, 0xffffff11, 0xfffeb817,
   0xd0808912, 0x0000a897, 0x0638c021, 0x00000041,
   0x00000041, 0x00000041, 0x014340f1, 0x01118071,
};

#endif
