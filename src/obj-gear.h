/**
 * \file: obj-gear.h
 * \brief management of inventory, equipment and quiver
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 * Copyright (c) 2014 Nick McConnell
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

#ifndef OBJECT_GEAR_H
#define OBJECT_GEAR_H

#include "player.h"

/**
 * Player equipment slot types
 */
enum
{
	#define EQUIP(a, b, c, d, e, f) EQUIP_##a,
	#include "list-equip-slots.h"
	#undef EQUIP
	EQUIP_MAX
};

int slot_by_name(struct player *p, const char *name);
bool slot_type_is(struct player *p, int slot, int type);
struct object *slot_object(struct player *p, int slot);
struct object *equipped_item_by_slot_name(struct player *p, const char *name);
int object_slot(struct player_body body, const struct object *obj);
bool object_is_equipped(struct player_body body, const struct object *obj);
bool object_is_carried(struct player *p, const struct object *obj);
bool object_is_in_quiver(struct player *p, const struct object *obj);
uint16_t object_pack_total(struct player *p, const struct object *obj,
	bool ignore_inscrip, struct object **first);
int pack_slots_used(const struct player *p);
const char *equip_mention(struct player *p, int slot);
const char *equip_describe(struct player *p, int slot);
int wield_slot(const struct object *obj);
bool minus_ac(struct player *p);
char gear_to_label(struct player *p, struct object *obj);
struct object *gear_last_item(struct player *p);
void gear_insert_end(struct player *p, struct object *obj);
struct object *gear_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left);
int inven_carry_num(const struct player *p, const struct object *obj);
bool inven_carry_okay(const struct object *obj);
void inven_item_charges(struct object *obj);
void inven_carry(struct player *p, struct object *obj, bool absorb,
				 bool message);
void inven_wield(struct object *obj, int slot);
void inven_takeoff(struct object *item);
void inven_drop(struct object *obj, int amt);
void combine_pack(struct player *p);
bool pack_is_full(void);
bool pack_is_overfull(void);
void pack_overflow(struct object *obj);
int preferred_quiver_slot(const struct object *obj);

/**
 * A single step in an equipment swap: one body slot, take off the old item,
 * put on the new item.  Either old_obj or new_obj may be NULL (but not both).
 */
struct equip_swap_step {
	int body_slot;              /**< index into player->body.slots */
	struct object *old_obj;     /**< item currently equipped (NULL if empty) */
	struct object *new_obj;     /**< item to wield (NULL if just taking off) */
};

/**
 * An ambiguous slot in a saved set: multiple inventory/equipment items
 * match the slot's identifying criteria, so we cannot safely auto-select.
 */
struct equip_swap_ambiguous {
	struct equip_set_slot *set_slot;    /**< the saved slot spec */
	struct object **matches;            /**< all items matching the spec */
	int match_count;
};

/**
 * A complete, validated equipment swap plan.
 * Built once and used for both preview display AND atomic execution.
 * is_feasible is true only when: no missing, no ambiguous, no cursed blockers,
 * all slots assigned, and inventory capacity is verified sufficient.
 */
struct equip_swap_plan {
	bool is_feasible;

	/* Ordered list of body-slot-level swaps to execute */
	struct equip_swap_step *steps;
	int num_steps;

	/* Display-oriented classifications (shared with preview UI) */
	struct object **will_takeoff;
	int takeoff_count;
	struct object **will_wield;
	int wield_count;
	struct object **cursed_objs;
	int cursed_count;
	struct equip_set_slot **missing_slots;
	int missing_count;
	struct equip_swap_ambiguous *ambiguous;
	int ambiguous_count;
};

void equip_set_init(struct player *p);
void equip_set_free(struct player *p);
bool equip_set_save(struct player *p, int index, const char *name);
bool equip_set_delete(struct player *p, int index);
bool equip_set_is_valid(struct player *p, int index);
const char *equip_set_name(struct player *p, int index);
struct object *equip_set_find_match(struct player *p, struct equip_set_slot *slot);
int equip_set_find_all_matches(struct player *p, struct equip_set_slot *slot,
	struct object ***matches_out);
struct equip_swap_plan *equip_set_build_plan(struct player *p, int index);
void equip_set_free_plan(struct equip_swap_plan *plan);
bool equip_set_execute_plan(struct player *p, struct equip_swap_plan *plan);
bool equip_set_switch_preview(struct player *p, int index,
	struct object ***will_takeoff, int *takeoff_count,
	struct object ***will_wield, struct equip_set_slot ***will_wield_slots, int *wield_count,
	struct equip_set_slot ***missing_slots, int *missing_count,
	struct object ***cursed_slots, int *cursed_count,
	struct equip_set_slot ***ambiguous_slots, struct object ***ambiguous_matches, int *ambiguous_count);
bool equip_set_apply(struct player *p, int index);


#endif /* OBJECT_GEAR_H */
