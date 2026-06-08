/**
 * \file obj-pile.h
 * \brief Deal with piles of objects
 *
 * Pile/list primitives only: object lifecycle, linked-list insert/excise,
 * stacking/merging helpers, floor scanning, pile charges display.
 *
 * Also re-exports the public floor-level transfer APIs (floor_object_for_use,
 * floor_carry, drop_near, push_object).  The underlying transfer planning
 * layer lives in obj-transfer.h and is only for internal use by obj-gear,
 * cmd-pickup, and the test harness.
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

#ifndef OBJECT_PILE_H
#define OBJECT_PILE_H

#include "cave.h"
#include "player.h"

#define OBJECT_LIST_SIZE  128
#define OBJECT_LIST_INCR  128

/**
 * Modes for stacking by object_similar()/object_stackable()/object_mergeable()
 */
typedef enum
{
	OSTACK_NONE    = 0x00, /* No options (this does NOT mean no stacking) */
	OSTACK_STORE   = 0x01, /* Store stacking */
	OSTACK_PACK    = 0x02, /* Inventory and home */
	OSTACK_LIST    = 0x04, /* Object list */
	OSTACK_MONSTER = 0x08, /* Monster carrying objects */
	OSTACK_FLOOR   = 0x10, /* Floor stacking */
	OSTACK_QUIVER  = 0x20  /* Quiver */
} object_stack_t;

/**
 * Modes for floor scanning by scan_floor()
 */
typedef enum
{
	OFLOOR_NONE    = 0x00, /* No options */
	OFLOOR_TEST    = 0x01, /* Verify item tester */
	OFLOOR_SENSE   = 0x02, /* Sensed or known items only */
	OFLOOR_TOP     = 0x04, /* Only the top item */
	OFLOOR_VISIBLE = 0x08, /* Visible items only */
} object_floor_t;

struct object *object_new(void);
void object_free(struct object *obj);
void object_delete(struct chunk *c, struct chunk *p_c,
				   struct object **obj_address);
void object_pile_free(struct chunk *c, struct chunk *p_c, struct object *obj);

void pile_insert(struct object **pile, struct object *obj);
void pile_insert_end(struct object **pile, struct object *obj);
void pile_excise(struct object **pile, struct object *obj);
struct object *pile_last_item(struct object *const pile);
bool pile_contains(const struct object *top, const struct object *obj);

bool object_similar(const struct object *obj1, const struct object *obj2,
	object_stack_t mode);
bool object_stackable(const struct object *obj1, const struct object *obj2,
					  object_stack_t mode);
bool object_mergeable(const struct object *obj1, const struct object *obj2,
					object_stack_t mode);
void object_origin_combine(struct object *obj1, const struct object *obj2);
void object_absorb_partial(struct object *obj1, struct object *obj2,
	object_stack_t mode1, object_stack_t mode2);
void object_absorb(struct object *obj1, struct object *obj2);
void object_wipe(struct object *obj);
void object_copy(struct object *obj1, const struct object *obj2);
void object_copy_amt(struct object *dest, struct object *src, int amt);
struct object *object_split(struct object *src, int amt);

void floor_item_charges(struct object *obj);
int scan_floor(struct object **items, int max_size, struct player *p,
		object_floor_t mode, item_tester tester);
int scan_distant_floor(struct object **items, int max_size, struct player *p,
		struct loc grid);
int scan_items(struct object **item_list, size_t item_list_max,
		struct player *p, int mode, item_tester tester);
bool item_is_available(struct object *obj);

/*
 * Public floor-level transfer APIs (implemented in obj-transfer.c).
 * These are re-exported here so callers do not need to include the
 * internal transfer planning layer directly.
 */
struct object *floor_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left);
bool floor_carry(struct chunk *c, struct loc grid, struct object *drop,
	bool *notice);
void drop_near(struct chunk *c, struct object **dest, int chance,
	struct loc grid, bool kill_held_m_idx, bool verbose);
void push_object(struct loc grid);

#endif /* OBJECT_PILE_H */
