/**
 * \file obj-transfer.h
 * \brief Unified object transfer plan layer — INTERNAL HEADER
 *
 * This is an INTERNAL orchestration layer.  Do NOT include this header
 * from outside the object movement subsystem.  External callers should
 * use the public APIs re-exported from obj-pile.h (floor_object_for_use,
 * floor_carry, drop_near, push_object) and obj-gear.h (gear_object_for_use,
 * inven_carry, inven_drop, inven_carry_num, inven_carry_okay).
 *
 * This module provides a "plan first, execute second" abstraction for all
 * object movements between floor piles, the player's pack/quiver, and
 * equipment slots.  The quantity arithmetic (how many can move, how many
 * merge with existing stacks, how many need new slots, how many slots are
 * required) is computed in a single place and is NOT duplicated across
 * pickup, drop, wield, takeoff, etc.
 *
 * Dependency direction:
 *   obj-pile  (list/stack primitives)    <- bottom layer, no upward deps
 *   obj-gear  (pack/quiver capacity + equipment primitives)  <- depends on obj-pile
 *   obj-transfer  (this file: orchestration/plans)  <- depends on both
 *
 * Only obj-transfer.c, obj-gear.c, and cmd-pickup.c should include this
 * header directly; also the unit tests that exercise the planner internals.
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
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

#ifndef OBJECT_TRANSFER_H
#define OBJECT_TRANSFER_H

#include "cave.h"
#include "obj-pile.h"
#include "player.h"

#define TRANSFER_MAX_MERGES 16

struct obj_transfer_merge {
	struct object *dest_obj;
	int amount;
	object_stack_t mode;
};

struct obj_transfer_plan {
	struct player *p;
	struct object *source;
	struct object *source_known;

	int requested;
	int movable;
	int source_remaining;

	int merge_count;
	struct obj_transfer_merge merges[TRANSFER_MAX_MERGES];

	int new_stack_amount;
	bool needs_new_slot;

	int capacity_extra_pack_slots;

	bool capacity_ok;
};

void obj_transfer_plan_init(struct obj_transfer_plan *plan,
		struct player *p, struct object *source);

bool obj_transfer_plan_floor_to_pack(struct obj_transfer_plan *plan,
		int max_want);
bool obj_transfer_plan_equip_to_pack(struct obj_transfer_plan *plan);
bool obj_transfer_plan_pack_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, int max_want);
bool obj_transfer_plan_floor_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, int max_want);

struct object *obj_transfer_execute_split_source(
		struct obj_transfer_plan *plan);
void obj_transfer_execute_to_pack(struct obj_transfer_plan *plan,
		struct object *detached, bool absorb, bool message);
void obj_transfer_execute_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, struct object *detached,
		bool *note);

struct object *floor_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left);
struct object *gear_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left);

int inven_carry_num(const struct player *p, const struct object *obj);
bool inven_carry_okay(const struct object *obj);
void inven_carry(struct player *p, struct object *obj, bool absorb,
				 bool message);

bool floor_carry(struct chunk *c, struct loc grid, struct object *drop,
				 bool *note);
void drop_near(struct chunk *c, struct object **dropped, int chance,
			   struct loc grid, bool verbose, bool prefer_pile);

void inven_drop(struct object *obj, int amt);
void push_object(struct loc grid);

#endif /* OBJECT_TRANSFER_H */
