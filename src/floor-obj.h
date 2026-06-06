/**
 * \file floor-obj.h
 * \brief Floor objective tracking - lightweight per-level exploration goals
 *
 * Copyright (c) 2026 Angband developers
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#ifndef FLOOR_OBJ_H
#define FLOOR_OBJ_H

#include "h-basic.h"
#include "z-type.h"
#include "cave.h"

struct player;
struct monster;
struct object;
struct chunk;

/* Functions */
void floor_obj_init(struct chunk *c);
void floor_obj_free(struct chunk *c);
bool floor_obj_generate(struct chunk *c, struct player *p);
void floor_obj_check_monster_kill(struct player *p, const struct monster *m);
void floor_obj_check_item_pickup(struct player *p, const struct object *obj);
void floor_obj_check_player_move(struct player *p);
void floor_obj_check_room_discovery(struct player *p, struct loc grid);
void floor_obj_claim_rewards(struct player *p);
const char *floor_obj_get_status_text(struct chunk *c, int idx);
bool floor_obj_has_active(struct chunk *c);
int floor_obj_get_highlight_grid(struct chunk *c, struct loc *grid);
bool floor_obj_is_highlight_grid(struct chunk *c, struct loc grid);

/* Save/load helpers */
void wr_floor_obj(struct chunk *c);
void rd_floor_obj(struct chunk *c);
void floor_obj_validate(struct chunk *c, struct player *p);

#endif /* FLOOR_OBJ_H */
