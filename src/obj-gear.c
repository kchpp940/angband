/**
 * \file obj-gear.c
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

#include "angband.h"
#include "cmd-core.h"
#include "game-event.h"
#include "init.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-ignore.h"
#include "obj-knowledge.h"
#include "obj-pile.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "player-calcs.h"
#include "player-util.h"

static const struct slot_info {
	int index;
	bool acid_vuln;
	bool name_in_desc;
	const char *mention;
	const char *heavy_describe;
	const char *describe;
} slot_table[] = {
	#define EQUIP(a, b, c, d, e, f) { EQUIP_##a, b, c, d, e, f },
	#include "list-equip-slots.h"
	#undef EQUIP
	{ EQUIP_MAX, false, false, NULL, NULL, NULL }
};

/**
 * Return the slot number for a given name, or quit game
 */
int slot_by_name(struct player *p, const char *name)
{
	int i;

	/* Look for the correctly named slot */
	for (i = 0; i < p->body.count; i++) {
		if (streq(name, p->body.slots[i].name)) {
			break;
		}
	}

	assert(i < p->body.count);

	/* Index for that slot */
	return i;
}

/**
 * Gets a slot of the given type, preferentially empty unless full is true
 */
static int slot_by_type(struct player *p, int type, bool full)
{
	int i, fallback = p->body.count;

	/* Look for a correct slot type */
	for (i = 0; i < p->body.count; i++) {
		if (type == p->body.slots[i].type) {
			if (full) {
				/* Found a full slot */
				if (p->body.slots[i].obj != NULL) break;
			} else {
				/* Found an empty slot */
				if (p->body.slots[i].obj == NULL) break;
			}
			/* Not right for full/empty, but still the right type */
			if (fallback == p->body.count)
				fallback = i;
		}
	}

	/* Index for the best slot we found, or p->body.count if none found  */
	return (i != p->body.count) ? i : fallback;
}

/**
 * Indicate whether a slot is of a given type.
 *
 * \param p is the player to test; if NULL, will assume the default body plan.
 * \param slot is the slot index for the player.
 * \param type is one of the EQUIP_* constants from list-equip-slots.h.
 * \return true if the slot can hold that type; otherwise false
 */
bool slot_type_is(struct player *p, int slot, int type)
{
	/* Assume default body if no player */
	struct player_body body = p ? p->body : bodies[0];

	return body.slots[slot].type == type ? true : false;
}

/**
 * Get the object in a specific slot (if any).  Quit if slot index is invalid.
 */
struct object *slot_object(struct player *p, int slot)
{
	/* Check bounds */
	assert(slot >= 0 && slot < p->body.count);

	/* Ensure a valid body */
	if (p->body.slots && p->body.slots[slot].obj) {
		return p->body.slots[slot].obj;
	}

	return NULL;
}

struct object *equipped_item_by_slot_name(struct player *p, const char *name)
{
	/* Ensure a valid body */
	if (p->body.slots) {
		return slot_object(p, slot_by_name(p, name));
	}

	return NULL;
}

int object_slot(struct player_body body, const struct object *obj)
{
	int i;

	for (i = 0; i < body.count; i++) {
		if (obj == body.slots[i].obj) {
			break;
		}
	}

	return i;
}

bool object_is_equipped(struct player_body body, const struct object *obj)
{
	return object_slot(body, obj) < body.count;
}

bool object_is_carried(struct player *p, const struct object *obj)
{
	return pile_contains(p->gear, obj);
}

/**
 * Check if an object is in the quiver
 */
bool object_is_in_quiver(struct player *p, const struct object *obj)
{
	int i;

	for (i = 0; i < z_info->quiver_size; i++) {
		if (obj == p->upkeep->quiver[i]) {
			return true;
		}
	}

	return false;
}

/**
 * Get the total number of objects in the pack or quiver that are like the
 * given object.
 *
 * \param p is the player whose inventory is used for the calculation.
 * \param obj is the template for the objects to look for.
 * \param ignore_inscrip if true, ignore the inscriptions when testing whether
 * an object is similar; otherwise, test the inscriptions as well.
 * \param first if not NULL, set to the first stack like obj (by ordering in
 * the quiver or pack with quiver taking precedence over pack; if the pack
 * and quiver haven't been computed, it will be the first non-equipped stack
 * in the gear).
 */
uint16_t object_pack_total(struct player *p, const struct object *obj,
		bool ignore_inscrip, struct object **first)
{
	uint16_t total = 0;
	char first_label = '\0';
	struct object *cursor;

	if (first) {
		*first = NULL;
	}
	for (cursor = p->gear; cursor; cursor = cursor->next) {
		bool like;

		if (cursor == obj) {
			/*
			 * object_similar() excludes cursor == obj so if
			 * obj is not equipped, account for it here.
			 */
			like = !object_is_equipped(p->body, obj);
		} else if (ignore_inscrip) {
			like = object_similar(obj, cursor, OSTACK_PACK);
		} else {
			like = object_stackable(obj, cursor, OSTACK_PACK);
		}
		if (like) {
			total += cursor->number;
			if (first) {
				char test_label = gear_to_label(p, cursor);

				if (!*first) {
					*first = cursor;
					first_label = test_label;
				} else {
					if (test_label >= 'a'
							&& test_label <= 'z') {
						if (first_label == '\0'
								|| (first_label >= 'a'
								&& first_label <= 'z'
								&& test_label < first_label)) {
							*first = cursor;
							first_label = test_label;
						}
					} else if (test_label >= '0'
							&& test_label <= '9') {
						if (first_label == '\0'
								|| (first_label >= 'a'
								&& first_label <= 'z')
								|| (first_label >= '0'
								&& first_label <= '9'
								&& test_label < first_label)) {
							*first = cursor;
							first_label = test_label;
						}
					}
				}
			}
		}
	}

	return total;
}

/**
 * Calculate the number of pack slots used by the current gear.
 *
 * Note that this function does not check that there are adequate slots in the
 * quiver, just the total quantity of missiles.
 */
int pack_slots_used(const struct player *p)
{
	const struct object *obj;
	int i, pack_slots = 0;
	int quiver_ammo = 0;

	for (obj = p->gear; obj; obj = obj->next) {
		bool found = false;
		/* Equipment doesn't count */
		if (!object_is_equipped(p->body, obj)) {
			/* Check if it is in the quiver */
			if (tval_is_ammo(obj) ||
					of_has(obj->flags, OF_THROWING)) {
				for (i = 0; i < z_info->quiver_size; i++) {
					if (p->upkeep->quiver[i] == obj) {
						quiver_ammo += obj->number *
							(tval_is_ammo(obj) ?
							1 : z_info->thrown_quiver_mult);
						found = true;
						break;
					}
				}
			}
			if (!found) {
				/* Count regular slots */
				pack_slots++;
			}
		}
	}

	/* Full slots */
	pack_slots += quiver_ammo / z_info->quiver_slot_size;

	/* Plus one for any remainder */
	if (quiver_ammo % z_info->quiver_slot_size) {
		pack_slots++;
	}

	return pack_slots;
}

/*
 * Return a string mentioning how a given item is carried
 */
const char *equip_mention(struct player *p, int slot)
{
	int type = p->body.slots[slot].type;

	/* Heavy */
	if ((type == EQUIP_WEAPON && p->state.heavy_wield) ||
			(type == EQUIP_WEAPON && p->state.heavy_shoot))
		return slot_table[type].heavy_describe;
	else if (slot_table[type].name_in_desc)
		return format(slot_table[type].mention, p->body.slots[slot].name);
	else
		return slot_table[type].mention;
}


/*
 * Return a string describing how a given item is being worn.
 * Currently, only used for items in the equipment, not inventory.
 */
const char *equip_describe(struct player *p, int slot)
{
	int type = p->body.slots[slot].type;

	/* Heavy */
	if ((type == EQUIP_WEAPON && p->state.heavy_wield) ||
			(type == EQUIP_WEAPON && p->state.heavy_shoot))
		return slot_table[type].heavy_describe;
	else if (slot_table[type].name_in_desc)
		return format(slot_table[type].describe, p->body.slots[slot].name);
	else
		return slot_table[type].describe;
}

/**
 * Determine which equipment slot (if any) an item likes. The slot might (or
 * might not) be open, but it is a slot which the object could be equipped in.
 *
 * For items where multiple slots could work (e.g. rings), the function
 * will try to return an open slot if possible.
 */
int wield_slot(const struct object *obj)
{
	/* Slot for equipment */
	switch (obj->tval)
	{
		case TV_BOW: return slot_by_type(player, EQUIP_BOW, false);
		case TV_AMULET: return slot_by_type(player, EQUIP_AMULET, false);
		case TV_CLOAK: return slot_by_type(player, EQUIP_CLOAK, false);
		case TV_SHIELD: return slot_by_type(player, EQUIP_SHIELD, false);
		case TV_GLOVES: return slot_by_type(player, EQUIP_GLOVES, false);
		case TV_BOOTS: return slot_by_type(player, EQUIP_BOOTS, false);
	}

	if (tval_is_melee_weapon(obj))
		return slot_by_type(player, EQUIP_WEAPON, false);
	else if (tval_is_ring(obj))
		return slot_by_type(player, EQUIP_RING, false);
	else if (tval_is_light(obj))
		return slot_by_type(player, EQUIP_LIGHT, false);
	else if (tval_is_body_armor(obj))
		return slot_by_type(player, EQUIP_BODY_ARMOR, false);
	else if (tval_is_head_armor(obj))
		return slot_by_type(player, EQUIP_HAT, false);

	/* No slot available */
	return -1;
}


/**
 * Acid has hit the player, attempt to affect some armor.
 *
 * Note that the "base armor" of an object never changes.
 * If any armor is damaged (or resists), the player takes less damage.
 */
bool minus_ac(struct player *p)
{
	int i, count = 0;
	struct object *obj = NULL;

	/* Avoid crash during monster power calculations */
	if (!p->gear) return false;

	/* Count the armor slots */
	for (i = 0; i < p->body.count; i++) {
		/* Ignore non-armor */
		if (slot_type_is(p, i, EQUIP_WEAPON)) continue;
		if (slot_type_is(p, i, EQUIP_BOW)) continue;
		if (slot_type_is(p, i, EQUIP_RING)) continue;
		if (slot_type_is(p, i, EQUIP_AMULET)) continue;
		if (slot_type_is(p, i, EQUIP_LIGHT)) continue;

		/* Add */
		count++;
	}

	/* Pick one at random */
	for (i = p->body.count - 1; i >= 0; i--) {
		/* Ignore non-armor */
		if (slot_type_is(p, i, EQUIP_WEAPON)) continue;
		if (slot_type_is(p, i, EQUIP_BOW)) continue;
		if (slot_type_is(p, i, EQUIP_RING)) continue;
		if (slot_type_is(p, i, EQUIP_AMULET)) continue;
		if (slot_type_is(p, i, EQUIP_LIGHT)) continue;

		if (one_in_(count--)) break;
	}

	/* Get the item */
	obj = slot_object(p, i);

	/* If we can still damage the item */
	if (obj && (obj->ac + obj->to_a > 0)) {
		char o_name[80];
		object_desc(o_name, sizeof(o_name), obj, ODESC_BASE, p);

		/* Object resists */
		if (obj->el_info[ELEM_ACID].flags & EL_INFO_IGNORE) {
			msg("Your %s is unaffected!", o_name);
		} else {
			msg("Your %s is damaged!", o_name);

			/* Damage the item */
			obj->to_a--;
			if (p->obj_k->to_a)
				obj->known->to_a = obj->to_a;

			p->upkeep->update |= (PU_BONUS);
			p->upkeep->redraw |= (PR_EQUIP);
		}

		/* There was an effect */
		return true;
	} else {
		/* No damage or effect */
		return false;
	}
}

/**
 * Convert a gear object into a one character label.
 */
char gear_to_label(struct player *p, struct object *obj)
{
	/* Skip rogue-like cardinal direction movement keys. */
	const char labels[] =
		 "abcdefgimnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i;

	/* Equipment is easy */
	if (object_is_equipped(p->body, obj)) {
		return labels[equipped_item_slot(p->body, obj)];
	}

	/* Check the quiver */
	for (i = 0; i < z_info->quiver_size; i++) {
		if (p->upkeep->quiver[i] == obj) {
			return I2D(i);
		}
	}

	/* Check the inventory */
	for (i = 0; i < z_info->pack_size; i++) {
		if (p->upkeep->inven[i] == obj) {
			return labels[i];
		}
	}

	return '\0';
}

/**
 * Remove an object from the gear list, leaving it unattached
 * \param p the player to affect
 * \param obj the object to remove
 * \return whether an object was removed
 */
static bool gear_excise_object(struct player *p, struct object *obj)
{
	int i;

	pile_excise(&p->gear_k, obj->known);
	pile_excise(&p->gear, obj);

	/* Change the weight */
	p->upkeep->total_weight -= obj->number * object_weight_one(obj);

	/* Make sure it isn't still equipped */
	for (i = 0; i < p->body.count; i++) {
		if (slot_object(p, i) == obj) {
			p->body.slots[i].obj = NULL;
			p->upkeep->equip_cnt--;
		}
	}

	/* Update the gear */
	calc_inventory(p);

	/* Housekeeping */
	p->upkeep->update |= (PU_BONUS);
	p->upkeep->notice |= (PN_COMBINE);
	p->upkeep->redraw |= (PR_INVEN | PR_EQUIP);

	return true;
}

struct object *gear_last_item(struct player *p)
{
	return pile_last_item(p->gear);
}

void gear_insert_end(struct player *p, struct object *obj)
{
	pile_insert_end(&p->gear, obj);
	pile_insert_end(&p->gear_k, obj->known);
}

/**
 * Remove an amount of an object from the inventory or quiver, returning
 * a detached object which can be used.
 *
 * Optionally describe what remains.
 */
struct object *gear_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left)
{
	struct object *usable;
	struct object *first_remainder = NULL;
	char name[80];
	char label = gear_to_label(p, obj);
	bool artifact = (obj->known->artifact != NULL);

	/* Bounds check */
	num = MIN(num, obj->number);

	/* Split off a usable object if necessary */
	if (obj->number > num) {
		usable = object_split(obj, num);

		/* Change the weight */
		p->upkeep->total_weight -= num * object_weight_one(obj);

		if (message) {
			uint16_t total;

			/*
			 * Don't show aggregate total in pack if equipped or
			 * if the description could have a number of charges
			 * or recharging notice specific to the stack (not
			 * aggregating those quantities so there would be
			 * confusion if aggregating the count).
			 */
			if (object_is_equipped(p->body, obj)
					|| tval_can_have_charges(obj)
					|| tval_is_rod(obj)
					|| obj->timeout > 0) {
				total = obj->number;
			} else {
				total = object_pack_total(p, obj, false,
					&first_remainder);
				assert(total >= first_remainder->number);
				if (total == first_remainder->number) {
					first_remainder = NULL;
				}
			}
			object_desc(name, sizeof(name), obj,
				ODESC_PREFIX | ODESC_FULL | ODESC_ALTNUM |
				(total << 16), p);
		}
	} else {
		if (message) {
			if (artifact) {
				object_desc(name, sizeof(name), obj,
					ODESC_FULL | ODESC_SINGULAR, p);
			} else {
				uint16_t total;

				/*
				 * Use same logic as above for showing an
				 * aggregate total.
				 */
				if (object_is_equipped(p->body, obj)
						|| tval_can_have_charges(obj)
						|| tval_is_rod(obj)
						|| obj->timeout > 0) {
					total = obj->number;
				} else {
					total = object_pack_total(p, obj,
						false, &first_remainder);
				}

				assert(total >= num);
				total -= num;
				if (!total || total <= first_remainder->number) {
					first_remainder = NULL;
				}
				object_desc(name, sizeof(name), obj,
					ODESC_PREFIX | ODESC_FULL |
					ODESC_ALTNUM | (total << 16), p);
			}
		}

		/* We're using the entire stack */
		usable = obj;
		gear_excise_object(p, usable);
		*none_left = true;

		/* Stop tracking item */
		if (tracked_object_is(p->upkeep, obj))
			track_object(p->upkeep, NULL);

		/* Inventory has changed, so disable repeat command */
		cmd_disable_repeat();
	}

	/* Housekeeping */
	p->upkeep->update |= (PU_BONUS);
	p->upkeep->notice |= (PN_COMBINE);
	p->upkeep->redraw |= (PR_INVEN | PR_EQUIP);

	/* Print a message if desired */
	if (message) {
		if (artifact) {
			msg("You no longer have the %s (%c).", name, label);
		} else if (first_remainder) {
			label = gear_to_label(p, first_remainder);
			msg("You have %s (1st %c).", name, label);
		} else {
			msg("You have %s (%c).", name, label);
		}
	}

	return usable;
}

/**
 * Check how many missiles can be put in the quiver with a limit on whether
 * the quiver can expand to take more slots in the pack.
 *
 * \param p Is the player with the quiver to use.
 * \param obj Is the object to add.
 * \param n_add_pack At entry, *n_add_pack is the maximum number of additional
 * pack slots to give to the quiver.  At exit, *n_add_pack will be the number
 * of those slots that were not used to expand the quiver.
 * \param n_to_quiver At exit, *n_to_quiver will be the number that can be
 * added to the quiver.  It will be no more than obj->number.  The value of
 * *n_to_quiver at entry is not used.
 */
static void quiver_absorb_num(const struct player *p, const struct object *obj,
		int *n_add_pack, int *n_to_quiver)
{
	bool ammo = tval_is_ammo(obj);

	/* Must be ammo or good for throwing */
	if (ammo || of_has(obj->flags, OF_THROWING)) {
		int i, quiver_count = 0, space_free = 0, n_empty = 0;
		int desired_slot = preferred_quiver_slot(obj);
		bool displaces = false;

		/* Count the current space this object could go into. */
		for (i = 0; i < z_info->quiver_size; i++) {
			const struct object *quiver_obj = p->upkeep->quiver[i];
			if (quiver_obj) {
				int mult = tval_is_ammo(quiver_obj) ?
					1 : z_info->thrown_quiver_mult;

				quiver_count += quiver_obj->number * mult;
				if (object_stackable(quiver_obj, obj, OSTACK_PACK)) {
					assert(quiver_obj->number * mult <=
						z_info->quiver_slot_size);
					space_free += z_info->quiver_slot_size -
						quiver_obj->number * mult;
				} else if (desired_slot == i &&
						preferred_quiver_slot(quiver_obj) != i) {
					/*
					 * The object to be added prefers to go
					 * in this slot, but it's occupied by
					 * something that could be displaced
					 * to another quiver slot, if one is
					 * available.
					 */
					displaces = true;
					assert(quiver_obj->number * mult <=
						z_info->quiver_slot_size);
					/*
					 * Avoid double counting in the ammo
					 * case since the empty slot, if any,
					 * for the displaced stack is treated
					 * as fully available.
					 */
					if (ammo) {
						space_free += z_info->quiver_slot_size
							- quiver_obj->number
							* mult;
					} else {
						space_free += z_info->quiver_slot_size;
					}
				}
			} else {
				++n_empty;
				/*
				 * Ammo can fit in any empty slot in the quiver.
				 * Non-ammo throwing items are restricted to
				 * their preferred slot.
				 */
				if (ammo || desired_slot == i) {
					space_free += z_info->quiver_slot_size;
				}
			}
		}

		/*
		 * Only possible to add if there is space free in the quiver
		 * and either are displacing a pile with an empty quiver slot
		 * available for it or are not displacing a pile at all.
		 */
		if (space_free && ((displaces && n_empty) || !displaces)) {
			int mult = ammo ? 1 : z_info->thrown_quiver_mult;
			/*
			 * When quiver_count % quiver_slot_size is zero, adding
			 * anything will require a pack slot.
			 */
			int remainder = quiver_count % z_info->quiver_slot_size;
			int limit_from_pack = (remainder) ?
				z_info->quiver_slot_size - remainder : 0;

			if (*n_add_pack > 0) {
				limit_from_pack += *n_add_pack *
					z_info->quiver_slot_size;
			}

			/* Return the number or amount that fits. */
			space_free = MIN(space_free, limit_from_pack);
			*n_to_quiver = MIN(obj->number, space_free / mult);
			*n_add_pack -= (*n_to_quiver * mult +
				z_info->quiver_slot_size - 1 -
				remainder) / z_info->quiver_slot_size;
			return;
		}
	}

	/* Not suitable for the quiver or no space */
	*n_to_quiver = 0;
}

/**
 * Calculate how much of an item is can be carried in the inventory or quiver.
 */
int inven_carry_num(const struct player *p, const struct object *obj)
{
	int n_free_slot = z_info->pack_size - pack_slots_used(p);
	int num_to_quiver, num_left, i;

	/* Treasure can always be picked up. */
	if (tval_is_money(obj) && lookup_kind(obj->tval, obj->sval)) {
		return obj->number;
	}

	/* Absorb as many as we can in the quiver. */
	quiver_absorb_num(p, obj, &n_free_slot, &num_to_quiver);

	/* The quiver will get everything, or the pack can hold what's left. */
	if (num_to_quiver == obj->number || n_free_slot > 0) {
		return obj->number;
	}

	/* See if we can add to a partially full inventory slot. */
	num_left = obj->number - num_to_quiver;
	for (i = 0; i < z_info->pack_size; i++) {
		struct object *inven_obj = p->upkeep->inven[i];
		if (inven_obj && object_stackable(inven_obj, obj, OSTACK_PACK)) {
			num_left -= inven_obj->kind->base->max_stack -
				inven_obj->number;
			if (num_left <= 0) break;
		}
	}

	/* Return the number we can absorb */
	return obj->number - MAX(num_left, 0);
}

/**
 * Check if we have space for some of an item in the pack.
 */
bool inven_carry_okay(const struct object *obj)
{
	return inven_carry_num(player, obj) > 0;
}

/**
 * Describe the charges on an item in the inventory.
 */
void inven_item_charges(struct object *obj)
{
	/* Require staff/wand */
	if (tval_can_have_charges(obj) && object_flavor_is_aware(obj)) {
		msg("You have %d charge%s remaining.",
				obj->pval,
				PLURAL(obj->pval));
	}
}

/**
 * Add an item to the players inventory.
 *
 * If the new item can combine with an existing item in the inventory,
 * it will do so, using object_mergeable() and object_absorb(), else,
 * the item will be placed into the first available gear array index.
 *
 * This function can be used to "over-fill" the player's pack, but only
 * once, and such an action must trigger the "overflow" code immediately.
 * Note that when the pack is being "over-filled", the new item must be
 * placed into the "overflow" slot, and the "overflow" must take place
 * before the pack is reordered, but (optionally) after the pack is
 * combined.  This may be tricky.  See "dungeon.c" for info.
 *
 * Note that this code removes any location information from the object once
 * it is placed into the inventory, but takes no responsibility for removing
 * the object from any other pile it was in.
 */
void inven_carry(struct player *p, struct object *obj, bool absorb,
				 bool message)
{
	bool combining = false;

	/* Check for combining, if appropriate */
	if (absorb) {
		struct object *combine_item = NULL;

		struct object *gear_obj = p->gear;
		while ((combine_item == NULL) && (gear_obj != NULL)) {
			object_stack_t stack_mode =
				object_is_in_quiver(p, gear_obj) ?
				OSTACK_QUIVER : OSTACK_PACK;

			if (!object_is_equipped(p->body, gear_obj) &&
					object_mergeable(gear_obj, obj, stack_mode)) {
				combine_item = gear_obj;
			}

			gear_obj = gear_obj->next;
		}

		if (combine_item) {
			/* Increase the weight */
			p->upkeep->total_weight +=
				obj->number * object_weight_one(obj);

			/* Combine the items, and their known versions */
			object_absorb(combine_item->known, obj->known);
			obj->known = NULL;
			object_absorb(combine_item, obj);

			/* Ensure numbers are aligned (should not be necessary, but safe) */
			combine_item->known->number = combine_item->number;

			obj = combine_item;
			combining = true;
		}
	}

	/* We didn't manage the find an object to combine with */
	if (!combining) {
		/* Paranoia */
		assert(pack_slots_used(p) <= z_info->pack_size);

		gear_insert_end(p, obj);
		apply_autoinscription(p, obj);

		/* Remove cave object details */
		obj->held_m_idx = 0;
		obj->grid = loc(0, 0);
		obj->known->grid = loc(0, 0);

		/* Update the inventory */
		p->upkeep->total_weight += obj->number * object_weight_one(obj);
		p->upkeep->notice |= (PN_COMBINE);

		/* Hobbits ID mushrooms on pickup, gnomes ID wands and staffs on pickup */
		if (!object_flavor_is_aware(obj)) {
			if (player_has(p, PF_KNOW_MUSHROOM) && tval_is_mushroom(obj)) {
				object_flavor_aware(p, obj);
				msg("Mushrooms for breakfast!");
			} else if (player_has(p, PF_KNOW_ZAPPER) && tval_is_zapper(obj))
				object_flavor_aware(p, obj);
		}
	}

	p->upkeep->update |= (PU_BONUS | PU_INVEN);
	p->upkeep->redraw |= (PR_INVEN);
	update_stuff(p);

	if (message) {
		char o_name[80];
		struct object *first;
		uint16_t total;
		char label;

		/*
		 * Show an aggregate total if the description doesn't have
		 * a charge/recharging notice that's specific to the stack.
		 */
		if (tval_can_have_charges(obj) || tval_is_rod(obj)
				|| obj->timeout > 0) {
			total = obj->number;
			first = obj;
		} else {
			total = object_pack_total(p, obj, false, &first);
		}
		assert(first && total >= first->number);
		object_desc(o_name, sizeof(o_name), obj,
			ODESC_PREFIX | ODESC_FULL | ODESC_ALTNUM |
			(total << 16), p);
		label = gear_to_label(p, first);
		if (total > first->number) {
			msg("You have %s (1st %c).", o_name, label);
		} else {
			assert(first == obj);
			msg("You have %s (%c).", o_name, label);
		}
	}

	if (object_is_in_quiver(p, obj))
		sound(MSG_QUIVER);
}


/**
 * Wield or wear a single item from the pack or floor
 */
void inven_wield(struct object *obj, int slot)
{
	struct object *wielded, *old = player->body.slots[slot].obj;

	const char *fmt;
	char o_name[80];
	bool dummy = false;

	/* Increase equipment counter if empty slot */
	if (old == NULL)
		player->upkeep->equip_cnt++;

	/* Take a turn */
	player->upkeep->energy_use = z_info->move_energy;

	/* It's either a gear object or a floor object */
	if (object_is_carried(player, obj)) {
		/* Split off a new object if necessary */
		if (obj->number > 1) {
			wielded = gear_object_for_use(player, obj, 1, false,
				&dummy);

			/* It's still carried; keep its weight in the total. */
			assert(wielded->number == 1);
			player->upkeep->total_weight +=
				object_weight_one(wielded);

			/* The new item needs new gear and known gear entries */
			wielded->next = obj->next;
			obj->next = wielded;
			wielded->prev = obj;
			if (wielded->next)
				(wielded->next)->prev = wielded;
			wielded->known->next = obj->known->next;
			obj->known->next = wielded->known;
			wielded->known->prev = obj->known;
			if (wielded->known->next)
				(wielded->known->next)->prev = wielded->known;
		} else {
			/* Just use the object directly */
			wielded = obj;
		}
	} else {
		/* Get a floor item and carry it */
		wielded = floor_object_for_use(player, obj, 1, false, &dummy);
		inven_carry(player, wielded, false, false);
	}

	/* Wear the new stuff */
	player->body.slots[slot].obj = wielded;

	/* Do any ID-on-wield */
	object_learn_on_wield(player, wielded);

	/* Where is the item now */
	if (tval_is_melee_weapon(wielded))
		fmt = "You are wielding %s (%c).";
	else if (wielded->tval == TV_BOW)
		fmt = "You are shooting with %s (%c).";
	else if (tval_is_light(wielded))
		fmt = "Your light source is %s (%c).";
	else
		fmt = "You are wearing %s (%c).";

	/* Describe the result */
	object_desc(o_name, sizeof(o_name), wielded,
		ODESC_PREFIX | ODESC_FULL, player);

	/* Message */
	msgt(MSG_WIELD, fmt, o_name, gear_to_label(player, wielded));

	/* Sticky flag geats a special mention */
	if (of_has(wielded->flags, OF_STICKY)) {
		/* Warn the player */
		msgt(MSG_CURSED, "Oops! It feels deathly cold!");
	}

	/* See if we have to overflow the pack */
	combine_pack(player);
	pack_overflow(old);

	/* Recalculate bonuses, torch, mana, gear */
	player->upkeep->notice |= (PN_IGNORE);
	player->upkeep->update |= (PU_BONUS | PU_INVEN | PU_UPDATE_VIEW);
	player->upkeep->redraw |= (PR_INVEN | PR_EQUIP | PR_ARMOR);
	player->upkeep->redraw |= (PR_STATS | PR_HP | PR_MANA | PR_SPEED);
	update_stuff(player);

	/* Disable repeats */
	cmd_disable_repeat();
}


/**
 * Take off a non-cursed equipment item
 *
 * Note that taking off an item when "full" may cause that item
 * to fall to the ground.
 *
 * Note also that this function does not try to combine the taken off item
 * with other inventory items - that must be done by the calling function.
 */
void inven_takeoff(struct object *obj)
{
	int slot = equipped_item_slot(player->body, obj);
	const char *act;
	char o_name[80];

	/* Paranoia */
	if (slot == player->body.count) return;

	/* Describe the object */
	object_desc(o_name, sizeof(o_name), obj, ODESC_PREFIX | ODESC_FULL,
		player);

	/* Describe removal by slot */
	if (slot_type_is(player, slot, EQUIP_WEAPON))
		act = "You were wielding";
	else if (slot_type_is(player, slot, EQUIP_BOW))
		act = "You were holding";
	else if (slot_type_is(player, slot, EQUIP_LIGHT))
		act = "You were holding";
	else
		act = "You were wearing";

	/* De-equip the object */
	player->body.slots[slot].obj = NULL;
	player->upkeep->equip_cnt--;

	player->upkeep->update |= (PU_BONUS | PU_INVEN | PU_UPDATE_VIEW);
	player->upkeep->notice |= (PN_IGNORE);
	update_stuff(player);

	/* Message */
	msgt(MSG_WIELD, "%s %s (%c).", act, o_name, gear_to_label(player, obj));

	return;
}


/**
 * Drop (some of) a non-cursed inventory/equipment item "near" the current
 * location
 *
 * There are two cases here - a single object or entire stack is being dropped,
 * or part of a stack is being split off and dropped
 */
void inven_drop(struct object *obj, int amt)
{
	struct object *dropped;
	bool none_left = false;
	bool equipped = false;
	bool quiver;

	char name[80];
	char label;

	/* Error check */
	if (amt <= 0)
		return;

	/* Check it is still held, in case there were two drop commands queued
	 * for this item.  This is in theory not ideal, but in practice should
	 * be safe. */
	if (!object_is_carried(player, obj))
		return;

	/* Get where the object is now */
	label = gear_to_label(player, obj);

	/* Is it in the quiver? */
	quiver = object_is_in_quiver(player, obj);

	/* Not too many */
	if (amt > obj->number) amt = obj->number;

	/* Take off equipment, don't combine */
	if (object_is_equipped(player->body, obj)) {
		equipped = true;
		inven_takeoff(obj);
	}

	/* Get the object */
	dropped = gear_object_for_use(player, obj, amt, false, &none_left);

	/* Describe the dropped object */
	object_desc(name, sizeof(name), dropped, ODESC_PREFIX | ODESC_FULL,
		player);

	/* Message */
	msg("You drop %s (%c).", name, label);

	/* Describe what's left */
	if (dropped->artifact) {
		object_desc(name, sizeof(name), dropped,
			ODESC_FULL | ODESC_SINGULAR, player);
		msg("You no longer have the %s (%c).", name, label);
	} else {
		struct object *first;
		struct object *desc_target;
		uint16_t total;

		/*
		 * Like gear_object_for_use(), don't show an aggregate total
		 * if it was equipped or the item has charges/recharging
		 * notice that is specific to the stack.
		 */
		if (equipped || tval_can_have_charges(obj) || tval_is_rod(obj)
				|| obj->timeout > 0) {
			first = NULL;
			if (none_left) {
				total = 0;
				desc_target = dropped;
			} else {
				total = obj->number;
				desc_target = obj;
			}
		} else {
			total = object_pack_total(player, obj, false, &first);
			desc_target = (total) ? obj : dropped;
		}

		object_desc(name, sizeof(name), desc_target,
			ODESC_PREFIX | ODESC_FULL | ODESC_ALTNUM |
			(total << 16), player);
		if (!first) {
			msg("You have %s (%c).", name, label);
		} else {
			label = gear_to_label(player, first);
			if (total > first->number) {
				msg("You have %s (1st %c).", name, label);
			} else {
				msg("You have %s (%c).", name, label);
			}
		}
	}

	/* Drop it near the player */
	drop_near(cave, &dropped, 0, player->grid, false, true);

	/* Sound for quiver objects */
	if (quiver)
		sound(MSG_QUIVER);

	event_signal(EVENT_INVENTORY);
	event_signal(EVENT_EQUIPMENT);
}


/**
 * Return whether each stack of objects can be merged into two uneven stacks.
 */
static bool inven_can_stack_partial(struct player *p, const struct object *obj1,
	const struct object *obj2, object_stack_t mode1, object_stack_t mode2)
{
	object_stack_t cmode = mode1 | mode2;

	if (! object_stackable(obj1, obj2, cmode)) {
		return false;
	}

	/*
	 * Now verify the numbers are suitable for uneven stacks.  Want the
	 * leading stack, obj1, to have its count maximized.
	 */
	if (!(cmode & OSTACK_STORE)) {
		/* The quiver may have stricter limits. */
		if (mode1 & OSTACK_QUIVER) {
			int qlimit = z_info->quiver_slot_size /
				(tval_is_ammo(obj1) ?
				1 : z_info->thrown_quiver_mult);

			/*
			 * These's no reason to combine if already at the limit.
			 */
			if (obj1->number == qlimit) {
				return false;
			}

			/*
			 * Checked the per-stack limits.  If trying to move
			 * items to the quiver, also check the overall quiver
			 * limits to avoid combining and then splitting in
			 * calc_inventory().
			 */
			if (mode2 & ~OSTACK_QUIVER) {
				int n_free_slot = z_info->pack_size -
					pack_slots_used(p);
				int num_to_quiver;

				quiver_absorb_num(p, obj2, &n_free_slot,
					&num_to_quiver);
				if (num_to_quiver <= 0) {
					return false;
				}
			}
		} else if (obj1->number == obj1->kind->base->max_stack) {
			/*
			 * These's no reason to combine if already at the limit.
			 */
			return false;
		}
	}

	return true;
}


/**
 * Combine items in the pack, confirming no blank objects or gold
 */
void combine_pack(struct player *p)
{
	struct object *obj1, *obj2, *prev;
	bool display_message = false;
	bool disable_repeat = false;

	/* Combine the pack (backwards) */
	obj1 = gear_last_item(p);
	while (obj1) {
		assert(obj1->kind);
		assert(!tval_is_money(obj1));
		prev = obj1->prev;

		/* Scan the items above that item */
		for (obj2 = p->gear; obj2 && obj2 != obj1; obj2 = obj2->next) {
			object_stack_t stack_mode2 =
				object_is_in_quiver(p, obj2) ?
				OSTACK_QUIVER : OSTACK_PACK;

			assert(obj2->kind);

			/* Can we drop "obj1" onto "obj2"? */
			if (object_mergeable(obj2, obj1, stack_mode2)) {
				display_message = true;
				disable_repeat = true;
				object_absorb(obj2->known, obj1->known);
				obj1->known = NULL;
				object_absorb(obj2, obj1);

				/* Ensure numbers align (should not be necessary, but safer) */
				obj2->known->number = obj2->number;

				break;
			} else {
				object_stack_t stack_mode1 =
					object_is_in_quiver(p, obj1) ?
					OSTACK_QUIVER : OSTACK_PACK;

				if (inven_can_stack_partial(p, obj2, obj1,
						stack_mode2, stack_mode1)) {
					/*
					 * Don't display a message for this
					 * case:  shuffling items between
					 * stacks isn't interesting to the
					 * player.
					 */
					object_absorb_partial(obj2->known,
						obj1->known, stack_mode2,
						stack_mode1);
					object_absorb_partial(obj2, obj1,
						stack_mode2, stack_mode1);
					/*
					 * Ensure numbers align (should not be
					 * necessary, but safer)
					 */
					obj2->known->number = obj2->number;
					obj1->known->number = obj1->number;

					break;
				}
			}
		}
		obj1 = prev;
	}

	calc_inventory(p);

	/* Redraw gear */
	event_signal(EVENT_INVENTORY);
	event_signal(EVENT_EQUIPMENT);

	/* Message */
	if (display_message) {
		msg("You combine some items in your pack.");

		/*
		 * Stop "repeat last command" from working if a stack was
		 * completely combined with another.
		 */
		if (disable_repeat) cmd_disable_repeat();
	}
}

/**
 * Returns whether the pack is holding the maximum number of items.
 */
bool pack_is_full(void)
{
	return pack_slots_used(player) == z_info->pack_size;
}

/**
 * Returns whether the pack is holding the more than the maximum number of
 * items. If this is true, calling pack_overflow() will trigger a pack overflow.
 */
bool pack_is_overfull(void)
{
	return pack_slots_used(player) > z_info->pack_size;
}

/**
 * Overflow an item from the pack, if it is overfull.
 */
void pack_overflow(struct object *obj)
{
	int i;
	char o_name[80];

	if (!pack_is_overfull()) return;

	/* Disturbing */
	disturb(player);

	/* Warning */
	msg("Your pack overflows!");

	/* Get the last proper item */
	for (i = 1; i <= z_info->pack_size; i++)
		if (!player->upkeep->inven[i])
			break;

	/* Drop the last inventory item unless requested otherwise */
	if (!obj) {
		obj = player->upkeep->inven[i - 1];
	}

	/* Rule out weirdness (like pack full, but inventory empty) */
	assert(obj != NULL);

	/* Describe */
	object_desc(o_name, sizeof(o_name), obj, ODESC_PREFIX | ODESC_FULL,
		player);

	/* Message */
	msg("You drop %s.", o_name);

	/* Excise the object and drop it (carefully) near the player */
	gear_excise_object(player, obj);
	drop_near(cave, &obj, 0, player->grid, false, true);

	/* Describe */
	msg("You no longer have %s.", o_name);

	/* Notice, update, redraw */
	if (player->upkeep->notice) notice_stuff(player);
	if (player->upkeep->update) update_stuff(player);
	if (player->upkeep->redraw) redraw_stuff(player);
}

/**
 * Look at an item's inscription to determine where it wants to be placed in
 * the quiver.  If the item is not appropriate for the quiver or is not
 * appropriately inscribed, return -1.
 */
int preferred_quiver_slot(const struct object *obj)
{
	int desired_slot = -1;

	if (obj->note && (tval_is_ammo(obj) ||
			of_has(obj->flags, OF_THROWING))) {
		const char *s = strchr(quark_str(obj->note), '@');
		char fire_key, throw_key;

		/*
		 * Would be nice to use cmd_lookup_key() for this, but that is
		 * part of the ui layer (declared in ui-game.h).  Instead,
		 * hardwire the keys for the fire and throw commands.
		 */
		if (OPT(player, rogue_like_commands)) {
			fire_key = 't';
		} else {
			fire_key = 'f';
		}
		throw_key = 'v';
		while (1) {
			if (!s) break;
			if (s[1] == fire_key || s[1] == throw_key) {
				desired_slot = s[2] - '0';
				break;
			}
			s = strchr(s + 1, '@');
		}
	}

	return desired_slot;
}

/**
 * Initialize all equipment sets for a player.
 */
void equip_set_init(struct player *p)
{
	int i;

	for (i = 0; i < EQUIP_SET_MAX; i++) {
		p->equip_sets[i].valid = false;
		p->equip_sets[i].name = NULL;
		p->equip_sets[i].slots = NULL;
		p->equip_sets[i].num_slots = 0;
	}
}

/**
 * Free all equipment sets for a player.
 */
void equip_set_free(struct player *p)
{
	int i, j;

	for (i = 0; i < EQUIP_SET_MAX; i++) {
		if (p->equip_sets[i].name) {
			mem_free(p->equip_sets[i].name);
			p->equip_sets[i].name = NULL;
		}
		if (p->equip_sets[i].slots) {
			for (j = 0; j < p->equip_sets[i].num_slots; j++) {
				if (p->equip_sets[i].slots[j].inscription) {
					mem_free(p->equip_sets[i].slots[j].inscription);
				}
				if (p->equip_sets[i].slots[j].artifact_name) {
					mem_free(p->equip_sets[i].slots[j].artifact_name);
				}
				if (p->equip_sets[i].slots[j].ego_name) {
					mem_free(p->equip_sets[i].slots[j].ego_name);
				}
			}
			mem_free(p->equip_sets[i].slots);
			p->equip_sets[i].slots = NULL;
		}
		p->equip_sets[i].num_slots = 0;
		p->equip_sets[i].valid = false;
	}
}

/**
 * Save the current equipment configuration to a set.
 */
bool equip_set_save(struct player *p, int index, const char *name)
{
	int i, slot_count;
	struct equip_set *set;

	if (index < 0 || index >= EQUIP_SET_MAX) {
		return false;
	}

	set = &p->equip_sets[index];

	equip_set_delete(p, index);

	set->name = name ? string_make(name) : string_make(format("Set %d", index + 1));
	set->num_slots = p->body.count;
	set->slots = mem_zalloc(set->num_slots * sizeof(struct equip_set_slot));

	slot_count = 0;
	for (i = 0; i < p->body.count; i++) {
		struct object *obj = slot_object(p, i);
		struct equip_set_slot *sslot = &set->slots[i];

		sslot->slot_type = p->body.slots[i].type;

		if (obj) {
			const char *inscr = (obj->note != 0) ? quark_str(obj->note) : NULL;

			sslot->used = true;
			sslot->tval = obj->tval;
			sslot->sval = obj->sval;
			sslot->pval = obj->pval;
			sslot->ac = obj->ac;
			sslot->weight = obj->weight;
			sslot->to_h = obj->to_h;
			sslot->to_d = obj->to_d;
			sslot->to_a = obj->to_a;
			sslot->dd = obj->dd;
			sslot->ds = obj->ds;
			sslot->origin = obj->origin;
			sslot->origin_depth = obj->origin_depth;

			if (inscr && inscr[0] != '\0') {
				sslot->inscription = string_make(inscr);
			} else {
				sslot->inscription = NULL;
			}

			if (obj->artifact) {
				sslot->artifact_name = string_make(obj->artifact->name);
			} else {
				sslot->artifact_name = NULL;
			}
			if (obj->ego) {
				sslot->ego_name = string_make(obj->ego->name);
			} else {
				sslot->ego_name = NULL;
			}
			slot_count++;
		} else {
			sslot->used = false;
			sslot->tval = 0;
			sslot->sval = 0;
			sslot->pval = 0;
			sslot->ac = 0;
			sslot->weight = 0;
			sslot->to_h = 0;
			sslot->to_d = 0;
			sslot->to_a = 0;
			sslot->dd = 0;
			sslot->ds = 0;
			sslot->origin = 0;
			sslot->origin_depth = 0;
			sslot->inscription = NULL;
			sslot->artifact_name = NULL;
			sslot->ego_name = NULL;
		}
	}

	set->valid = (slot_count > 0);
	return set->valid;
}

/**
 * Delete a saved equipment set.
 */
bool equip_set_delete(struct player *p, int index)
{
	int j;
	struct equip_set *set;

	if (index < 0 || index >= EQUIP_SET_MAX) {
		return false;
	}

	set = &p->equip_sets[index];

	if (set->name) {
		mem_free(set->name);
		set->name = NULL;
	}
	if (set->slots) {
		for (j = 0; j < set->num_slots; j++) {
			if (set->slots[j].inscription) {
				mem_free(set->slots[j].inscription);
			}
			if (set->slots[j].artifact_name) {
				mem_free(set->slots[j].artifact_name);
			}
			if (set->slots[j].ego_name) {
				mem_free(set->slots[j].ego_name);
			}
		}
		mem_free(set->slots);
		set->slots = NULL;
	}
	set->num_slots = 0;
	set->valid = false;

	return true;
}

/**
 * Check if an equipment set is valid (exists/saved).
 */
bool equip_set_is_valid(struct player *p, int index)
{
	if (index < 0 || index >= EQUIP_SET_MAX) {
		return false;
	}
	return p->equip_sets[index].valid;
}

/**
 * Get the name of an equipment set.
 */
const char *equip_set_name(struct player *p, int index)
{
	if (index < 0 || index >= EQUIP_SET_MAX) {
		return NULL;
	}
	return p->equip_sets[index].name;
}

/**
 * Check if two inscription strings are effectively equal (both NULL or same content).
 */
static bool equip_set_inscription_eq(const char *a, const char *b)
{
	if (!a && !b) return true;
	if (!a || !b) return false;
	return streq(a, b);
}

/**
 * Check if an object matches the criteria in a set slot.
 * Uses strong matching: inscription must match if present; otherwise all
 * stable identifying fields (pval, ac, weight, origin, plus the standard
 * to_h/to_d/to_a/dd/ds, tval/sval, artifact/ego) must match.
 */
static bool equip_set_object_matches(struct object *obj, struct equip_set_slot *slot)
{
	if (!obj || !slot) return false;

	if (slot->artifact_name) {
		if (!obj->artifact) return false;
		if (!streq(obj->artifact->name, slot->artifact_name)) return false;
		if (!equip_set_inscription_eq(
				slot->inscription,
				(obj->note != 0) ? quark_str(obj->note) : NULL)) {
			return false;
		}
		return true;
	}

	if (obj->tval != slot->tval) return false;
	if (obj->sval != slot->sval) return false;

	if (slot->ego_name) {
		if (!obj->ego) return false;
		if (!streq(obj->ego->name, slot->ego_name)) return false;
	} else if (obj->ego) {
		return false;
	}

	if (obj->pval != slot->pval) return false;
	if (obj->ac != slot->ac) return false;
	if (obj->weight != slot->weight) return false;
	if (obj->origin != slot->origin) return false;
	if (obj->origin_depth != slot->origin_depth) return false;

	if (obj->to_h != slot->to_h) return false;
	if (obj->to_d != slot->to_d) return false;
	if (obj->to_a != slot->to_a) return false;
	if (obj->dd != slot->dd) return false;
	if (obj->ds != slot->ds) return false;

	if (!equip_set_inscription_eq(
			slot->inscription,
			(obj->note != 0) ? quark_str(obj->note) : NULL)) {
		return false;
	}

	return true;
}

/**
 * Find a single matching object in the player's inventory or equipment
 * for a set slot.  Returns NULL if there is not exactly one match.
 * Use equip_set_find_all_matches() for ambiguity detection.
 */
struct object *equip_set_find_match(struct player *p, struct equip_set_slot *slot)
{
	struct object *obj;
	struct object *found = NULL;

	if (!slot || !slot->used) return NULL;

	for (obj = p->gear; obj; obj = obj->next) {
		if (equip_set_object_matches(obj, slot)) {
			if (found) {
				return NULL;
			}
			found = obj;
		}
	}

	return found;
}

/**
 * Find ALL matching objects in the player's inventory/equipment for a slot.
 * Returns the count of matches; *matches_out is set to a newly-allocated
 * array of matching object pointers (caller must mem_free() it).
 * Returns 0 if no match and sets *matches_out = NULL.
 */
int equip_set_find_all_matches(struct player *p, struct equip_set_slot *slot,
	struct object ***matches_out)
{
	struct object *obj;
	struct object **matches;
	int count = 0;
	int cap = 8;

	*matches_out = NULL;
	if (!slot || !slot->used) return 0;

	matches = mem_zalloc(cap * sizeof(struct object *));

	for (obj = p->gear; obj; obj = obj->next) {
		if (equip_set_object_matches(obj, slot)) {
			if (count >= cap) {
				cap *= 2;
				matches = mem_realloc(matches, cap * sizeof(struct object *));
			}
			matches[count++] = obj;
		}
	}

	if (count == 0) {
		mem_free(matches);
		return 0;
	}

	*matches_out = matches;
	return count;
}

/**
 * Free all memory owned by an equipment swap plan.
 */
void equip_set_free_plan(struct equip_swap_plan *plan)
{
	int i;

	if (!plan) return;

	if (plan->steps) mem_free(plan->steps);
	if (plan->will_takeoff) mem_free(plan->will_takeoff);
	if (plan->will_wield) mem_free(plan->will_wield);
	if (plan->cursed_objs) mem_free(plan->cursed_objs);
	if (plan->missing_slots) mem_free(plan->missing_slots);

	if (plan->ambiguous) {
		for (i = 0; i < plan->ambiguous_count; i++) {
			if (plan->ambiguous[i].matches) {
				mem_free(plan->ambiguous[i].matches);
			}
		}
		mem_free(plan->ambiguous);
	}

	mem_free(plan);
}

/**
 * Static, non-mutating validation of a swap plan.
 *
 * Returns true if the plan can be executed safely with zero risk of partial
 * state.  This function never modifies any player, object, or pack state.
 * If it returns false, plan->verify_error contains a human-readable reason.
 *
 * Checks performed:
 *   1. Every takeoff target is still equipped, removable (not sticky), and
 *      sits in the expected body slot.
 *   2. Every wield target is still carried or equipped.
 *   3. No two steps try to wield two different items into the same body slot.
 *   4. Each body slot type matches the item being wielded there
 *      (ring → EQUIP_RING, weapon → EQUIP_WEAPON, etc.).
 *   5. After conceptually removing all takeoff targets, every target slot
 *      that receives a new item is empty (or contains the same item).
 *   6. Pack capacity: in the worst case (no stacking, no combining), the
 *      pack must hold every taken-off item AND every currently-in-pack
 *      wieldable item that will be removed from the pack.  We use the
 *      strictest upper bound: current pack + takeoff_count -
 *      wield_items_in_pack ≤ pack_size.
 */
bool equip_set_verify_plan(struct player *p, struct equip_swap_plan *plan)
{
	int i, j;
	bool *slot_will_receive;
	bool *slot_will_be_vacated;
	int simulated_pack;
	int wield_items_in_pack = 0;

	if (!plan) {
		return false;
	}

	plan->verify_error[0] = '\0';

	slot_will_receive = mem_zalloc(p->body.count * sizeof(bool));
	slot_will_be_vacated = mem_zalloc(p->body.count * sizeof(bool));

	/* --- Pass 1: individual step validity + slot occupancy simulation --- */

	for (i = 0; i < plan->num_steps; i++) {
		struct equip_swap_step *st = &plan->steps[i];
		struct object *old_obj = st->old_obj;
		struct object *new_obj = st->new_obj;

		if (st->body_slot < 0 || st->body_slot >= p->body.count) {
			strnfmt(plan->verify_error, sizeof(plan->verify_error),
				"Step %d: body_slot %d out of range", i, st->body_slot);
			mem_free(slot_will_receive);
			mem_free(slot_will_be_vacated);
			return false;
		}

		if (!old_obj && !new_obj) {
			strnfmt(plan->verify_error, sizeof(plan->verify_error),
				"Step %d: no old_obj and no new_obj", i);
			mem_free(slot_will_receive);
			mem_free(slot_will_be_vacated);
			return false;
		}

		if (old_obj) {
			if (!object_is_equipped(p->body, old_obj)) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), old_obj,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Cannot take off %s: not currently equipped", o_name);
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}
			if (!obj_can_takeoff(old_obj)) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), old_obj,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Cannot take off %s: cursed or stuck", o_name);
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}
			slot_will_be_vacated[st->body_slot] = true;
		}

		if (new_obj) {
			bool carried = object_is_carried(p, new_obj);
			bool equipped = object_is_equipped(p->body, new_obj);
			int expected_slot_type = p->body.slots[st->body_slot].type;

			if (!carried && !equipped) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), new_obj,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Cannot wield %s: no longer in inventory or equipment",
					o_name);
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}

			if (carried && !equipped) {
				wield_items_in_pack++;
			}

			switch (new_obj->tval) {
				case TV_BOW:
					if (expected_slot_type != EQUIP_BOW) goto bad_type;
					break;
				case TV_AMULET:
					if (expected_slot_type != EQUIP_AMULET) goto bad_type;
					break;
				case TV_CLOAK:
					if (expected_slot_type != EQUIP_CLOAK) goto bad_type;
					break;
				case TV_SHIELD:
					if (expected_slot_type != EQUIP_SHIELD) goto bad_type;
					break;
				case TV_GLOVES:
					if (expected_slot_type != EQUIP_GLOVES) goto bad_type;
					break;
				case TV_BOOTS:
					if (expected_slot_type != EQUIP_BOOTS) goto bad_type;
					break;
				default:
					if (tval_is_melee_weapon(new_obj)) {
						if (expected_slot_type != EQUIP_WEAPON) goto bad_type;
					} else if (tval_is_ring(new_obj)) {
						if (expected_slot_type != EQUIP_RING) goto bad_type;
					} else if (tval_is_light(new_obj)) {
						if (expected_slot_type != EQUIP_LIGHT) goto bad_type;
					} else if (tval_is_body_armor(new_obj)) {
						if (expected_slot_type != EQUIP_BODY_ARMOR) goto bad_type;
					} else if (tval_is_head_armor(new_obj)) {
						if (expected_slot_type != EQUIP_HAT) goto bad_type;
					} else {
bad_type:
						{
							char o_name[80];
							object_desc(o_name, sizeof(o_name), new_obj,
								ODESC_PREFIX | ODESC_FULL, p);
							strnfmt(plan->verify_error,
								sizeof(plan->verify_error),
								"Slot type mismatch: %s cannot go into %s",
								o_name,
								equip_describe(p, st->body_slot));
							mem_free(slot_will_receive);
							mem_free(slot_will_be_vacated);
							return false;
						}
					}
			}

			if (slot_will_receive[st->body_slot]) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), new_obj,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Two items targeted for the same slot (%s): %s conflicts",
					equip_describe(p, st->body_slot), o_name);
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}
			slot_will_receive[st->body_slot] = true;
		}
	}

	/* --- Pass 2: every receiving slot will be empty at wield time --- */

	for (i = 0; i < plan->num_steps; i++) {
		struct equip_swap_step *st = &plan->steps[i];
		if (!st->new_obj) continue;

		{
			struct object *now = slot_object(p, st->body_slot);
			bool will_be_empty = slot_will_be_vacated[st->body_slot] ||
				now == NULL;

			if (now && now == st->new_obj) {
				continue;
			}
			if (!will_be_empty) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), st->new_obj,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Cannot wield %s into %s: slot will not be empty",
					o_name, equip_describe(p, st->body_slot));
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}
		}
	}

	/* --- Pass 3: check for cross-step item conflicts --- */

	for (i = 0; i < plan->num_steps; i++) {
		struct object *obj_i = plan->steps[i].old_obj ?
			plan->steps[i].old_obj : plan->steps[i].new_obj;
		for (j = i + 1; j < plan->num_steps; j++) {
			struct object *obj_j = plan->steps[j].old_obj ?
				plan->steps[j].old_obj : plan->steps[j].new_obj;
			if (obj_i && obj_j && obj_i == obj_j &&
				plan->steps[i].new_obj && plan->steps[j].new_obj &&
				plan->steps[i].body_slot != plan->steps[j].body_slot) {
				char o_name[80];
				object_desc(o_name, sizeof(o_name), obj_i,
					ODESC_PREFIX | ODESC_FULL, p);
				strnfmt(plan->verify_error, sizeof(plan->verify_error),
					"Item %s assigned to two different slots", o_name);
				mem_free(slot_will_receive);
				mem_free(slot_will_be_vacated);
				return false;
			}
		}
	}

	/* --- Pass 4: strictest pack capacity bound (no combining assumed) --- */

	simulated_pack = pack_slots_used(p) + plan->takeoff_count - wield_items_in_pack;
	if (simulated_pack > z_info->pack_size) {
		strnfmt(plan->verify_error, sizeof(plan->verify_error),
			"Pack would overflow: %d slots needed, only %d available",
			simulated_pack, z_info->pack_size);
		mem_free(slot_will_receive);
		mem_free(slot_will_be_vacated);
		return false;
	}

	mem_free(slot_will_receive);
	mem_free(slot_will_be_vacated);
	return true;
}

/**
 * Build a complete, validated equipment swap plan for the saved set at
 * the given index.
 *
 * The plan contains:
 *   - per-body-slot swap steps (pre-assigned unique slots for each wield)
 *   - classified display lists (takeoff, wield, cursed, missing, ambiguous)
 *   - an is_feasible flag which is true ONLY if the swap can be executed
 *     atomically with no rollback expected
 *
 * The caller must call equip_set_free_plan() when done.
 */
struct equip_swap_plan *equip_set_build_plan(struct player *p, int index)
{
	struct equip_swap_plan *plan;
	struct equip_set *set;
	int i, j;
	int max_items;
	bool *slot_assigned;

	if (index < 0 || index >= EQUIP_SET_MAX) return NULL;
	set = &p->equip_sets[index];
	if (!set->valid) return NULL;

	plan = mem_zalloc(sizeof(struct equip_swap_plan));
	max_items = p->body.count;

	plan->steps = mem_zalloc(max_items * sizeof(struct equip_swap_step));
	plan->will_takeoff = mem_zalloc(max_items * sizeof(struct object *));
	plan->will_wield = mem_zalloc(max_items * sizeof(struct object *));
	plan->cursed_objs = mem_zalloc(max_items * sizeof(struct object *));
	plan->missing_slots = mem_zalloc(max_items * sizeof(struct equip_set_slot *));
	plan->ambiguous = mem_zalloc(max_items * sizeof(struct equip_swap_ambiguous));

	slot_assigned = mem_zalloc(max_items * sizeof(bool));

	for (i = 0; i < set->num_slots && i < p->body.count; i++) {
		struct equip_set_slot *sslot = &set->slots[i];
		struct object *current_obj = slot_object(p, i);
		struct object **match_arr = NULL;
		int match_n;

		if (!sslot->used) {
			if (current_obj) {
				if (!obj_can_takeoff(current_obj)) {
					plan->cursed_objs[plan->cursed_count++] = current_obj;
				} else {
					struct equip_swap_step *st = &plan->steps[plan->num_steps++];
					st->body_slot = i;
					st->old_obj = current_obj;
					st->new_obj = NULL;
					plan->will_takeoff[plan->takeoff_count++] = current_obj;
					slot_assigned[i] = true;
				}
			}
			continue;
		}

		if (current_obj && equip_set_object_matches(current_obj, sslot)) {
			slot_assigned[i] = true;
			continue;
		}

		match_n = equip_set_find_all_matches(p, sslot, &match_arr);

		if (match_n == 0) {
			plan->missing_slots[plan->missing_count++] = sslot;
			if (current_obj) {
				if (!obj_can_takeoff(current_obj)) {
					plan->cursed_objs[plan->cursed_count++] = current_obj;
				} else {
					struct equip_swap_step *st = &plan->steps[plan->num_steps++];
					st->body_slot = i;
					st->old_obj = current_obj;
					st->new_obj = NULL;
					plan->will_takeoff[plan->takeoff_count++] = current_obj;
					slot_assigned[i] = true;
				}
			}
			continue;
		}

		if (match_n > 1) {
			struct equip_swap_ambiguous *am = &plan->ambiguous[plan->ambiguous_count++];
			am->set_slot = sslot;
			am->matches = match_arr;
			am->match_count = match_n;
			if (current_obj) {
				if (!obj_can_takeoff(current_obj)) {
					plan->cursed_objs[plan->cursed_count++] = current_obj;
				} else {
					struct equip_swap_step *st = &plan->steps[plan->num_steps++];
					st->body_slot = i;
					st->old_obj = current_obj;
					st->new_obj = NULL;
					plan->will_takeoff[plan->takeoff_count++] = current_obj;
					slot_assigned[i] = true;
				}
			}
			continue;
		}

		{
			struct object *match_obj = match_arr[0];
			bool match_already_equipped = false;
			int match_equipped_slot = -1;

			if (match_arr) mem_free(match_arr);

			if (object_is_equipped(p->body, match_obj)) {
				match_already_equipped = true;
				match_equipped_slot = object_slot(p->body, match_obj);
			}

			if (current_obj) {
				if (!obj_can_takeoff(current_obj)) {
					plan->cursed_objs[plan->cursed_count++] = current_obj;
					continue;
				}
			}

			if (match_already_equipped && match_equipped_slot == i) {
				slot_assigned[i] = true;
				continue;
			}

			{
				struct equip_swap_step *st = &plan->steps[plan->num_steps++];
				st->body_slot = i;
				st->old_obj = current_obj;
				st->new_obj = match_obj;
				slot_assigned[i] = true;

				if (current_obj) {
					plan->will_takeoff[plan->takeoff_count++] = current_obj;
				}
				if (!match_already_equipped) {
					plan->will_wield[plan->wield_count++] = match_obj;
				}
			}

			if (match_already_equipped && match_equipped_slot != i) {
				for (j = 0; j < plan->num_steps - 1; j++) {
					if (plan->steps[j].body_slot == match_equipped_slot) {
						break;
					}
				}
				if (j == plan->num_steps - 1) {
					struct equip_swap_step *st2 = &plan->steps[plan->num_steps++];
					st2->body_slot = match_equipped_slot;
					st2->old_obj = match_obj;
					st2->new_obj = NULL;
					if (!obj_can_takeoff(match_obj)) {
						plan->cursed_objs[plan->cursed_count++] = match_obj;
						plan->num_steps--;
					} else {
						plan->will_takeoff[plan->takeoff_count++] = match_obj;
						slot_assigned[match_equipped_slot] = true;
					}
				}
			}
		}
	}

	mem_free(slot_assigned);

	{
		if (!equip_set_verify_plan(p, plan)) {
			plan->is_feasible = false;
		} else {
			plan->is_feasible = (plan->missing_count == 0) &&
				(plan->ambiguous_count == 0) &&
				(plan->cursed_count == 0);
		}
	}

	return plan;
}

/**
 * Execute an equipment swap plan.
 *
 * Precondition: equip_set_verify_plan(p, plan) has returned true.
 *
 * Two-phase execution, no rollback (the strict pre-check guarantees that
 * every individual step will succeed):
 *   Phase 1 — Take off ALL old items.
 *   Phase 2 — Wield ALL new items into their pre-assigned body slots.
 *
 * If the precondition holds (verified plan), execution cannot fail in any
 * way that would leave partial state.  We still guard against unexpected
 * mid-execution state mutations by doing a very light post-check; if the
 * check somehow fails, we simply report failure without touching anything
 * further (the caller will see no changes marked).
 *
 * Returns true if any equipment state was changed.
 */
bool equip_set_execute_plan(struct player *p, struct equip_swap_plan *plan)
{
	int i;
	bool changed = false;
	int num_taken = 0;
	int num_wielded = 0;

	if (!plan || !plan->is_feasible) {
		return false;
	}

	if (plan->num_steps == 0) {
		return false;
	}

	if (!equip_set_verify_plan(p, plan)) {
		msg("Equipment swap aborted: %s",
			plan->verify_error[0] ? plan->verify_error :
			"preconditions no longer hold");
		return false;
	}

	for (i = 0; i < plan->num_steps; i++) {
		struct object *old_obj = plan->steps[i].old_obj;
		if (!old_obj) continue;
		inven_takeoff(old_obj);
		num_taken++;
	}

	for (i = 0; i < plan->num_steps; i++) {
		struct object *new_obj = plan->steps[i].new_obj;
		if (!new_obj) continue;
		inven_wield(new_obj, plan->steps[i].body_slot);
		num_wielded++;
	}

	changed = (num_taken > 0) || (num_wielded > 0);

	if (changed) {
		combine_pack(p);
		p->upkeep->notice |= (PN_IGNORE);
		p->upkeep->update |= (PU_BONUS | PU_INVEN | PU_UPDATE_VIEW);
		p->upkeep->redraw |= (PR_INVEN | PR_EQUIP | PR_ARMOR);
		p->upkeep->redraw |= (PR_STATS | PR_HP | PR_MANA | PR_SPEED);
		update_stuff(p);
		cmd_disable_repeat();
	}

	return changed;
}

/**
 * Backwards-compatible preview wrapper.
 * Builds a plan internally, then copies the display lists out.
 * The caller must still free all returned arrays as before.
 */
bool equip_set_switch_preview(struct player *p, int index,
	struct object ***will_takeoff, int *takeoff_count,
	struct object ***will_wield, struct equip_set_slot ***will_wield_slots, int *wield_count,
	struct equip_set_slot ***missing_slots, int *missing_count,
	struct object ***cursed_slots, int *cursed_count,
	struct equip_set_slot ***ambiguous_slots, struct object ***ambiguous_matches, int *ambiguous_count)
{
	struct equip_swap_plan *plan;
	int i, j;
	int max_items;

	*takeoff_count = 0;
	*wield_count = 0;
	*missing_count = 0;
	*cursed_count = 0;
	*ambiguous_count = 0;

	plan = equip_set_build_plan(p, index);
	if (!plan) {
		return false;
	}

	max_items = p->body.count;

	*will_takeoff = mem_zalloc(max_items * sizeof(struct object *));
	*will_wield = mem_zalloc(max_items * sizeof(struct object *));
	*will_wield_slots = mem_zalloc(max_items * sizeof(struct equip_set_slot *));
	*missing_slots = mem_zalloc(max_items * sizeof(struct equip_set_slot *));
	*cursed_slots = mem_zalloc(max_items * sizeof(struct object *));
	*ambiguous_slots = mem_zalloc(max_items * sizeof(struct equip_set_slot *));
	*ambiguous_matches = mem_zalloc(max_items * sizeof(struct object *));

	*takeoff_count = plan->takeoff_count;
	for (i = 0; i < plan->takeoff_count; i++) {
		(*will_takeoff)[i] = plan->will_takeoff[i];
	}

	{
		int wc = 0;
		for (j = 0; j < plan->num_steps; j++) {
			if (plan->steps[j].new_obj) {
				(*will_wield)[wc] = plan->steps[j].new_obj;
				(*will_wield_slots)[wc] = NULL;
				for (i = 0; i < p->equip_sets[index].num_slots; i++) {
					if (p->equip_sets[index].slots[i].slot_type ==
						p->body.slots[plan->steps[j].body_slot].type &&
						p->equip_sets[index].slots[i].used &&
						equip_set_object_matches(plan->steps[j].new_obj,
							&p->equip_sets[index].slots[i])) {
						(*will_wield_slots)[wc] = &p->equip_sets[index].slots[i];
						break;
					}
				}
				wc++;
			}
		}
		*wield_count = wc;
	}

	*missing_count = plan->missing_count;
	for (i = 0; i < plan->missing_count; i++) {
		(*missing_slots)[i] = plan->missing_slots[i];
	}

	*cursed_count = plan->cursed_count;
	for (i = 0; i < plan->cursed_count; i++) {
		(*cursed_slots)[i] = plan->cursed_objs[i];
	}

	*ambiguous_count = plan->ambiguous_count;
	for (i = 0; i < plan->ambiguous_count; i++) {
		(*ambiguous_slots)[i] = plan->ambiguous[i].set_slot;
		(*ambiguous_matches)[i] = plan->ambiguous[i].matches[0];
	}

	equip_set_free_plan(plan);
	return true;
}

/**
 * Switch to a saved equipment set using the atomic plan pipeline.
 * This refactored version builds a plan, reports any blockers (ambiguous /
 * cursed / missing), and then executes the plan atomically with full rollback.
 */
bool equip_set_apply(struct player *p, int index)
{
	struct equip_swap_plan *plan;
	int j;
	bool changed;

	if (index < 0 || index >= EQUIP_SET_MAX) return false;
	if (!p->equip_sets[index].valid) return false;

	plan = equip_set_build_plan(p, index);
	if (!plan) {
		return false;
	}

	if (plan->ambiguous_count > 0) {
		for (j = 0; j < plan->ambiguous_count; j++) {
			struct equip_set_slot *sslot = plan->ambiguous[j].set_slot;
			if (sslot->artifact_name) {
				msg("Multiple items match the artifact %s; cannot switch safely.",
					sslot->artifact_name);
			} else {
				char buf[80];
				struct object_kind *kind = lookup_kind(sslot->tval, sslot->sval);
				if (kind) {
					strnfmt(buf, sizeof(buf), "%s", kind->name);
					if (sslot->ego_name) {
						my_strcat(buf, " (", sizeof(buf));
						my_strcat(buf, sslot->ego_name, sizeof(buf));
						my_strcat(buf, ")", sizeof(buf));
					}
					msg("Multiple items match %s; cannot switch safely.", buf);
				} else {
					msg("Multiple items match a slot; cannot switch safely.");
				}
			}
			msg("  Tip: inscribe a unique @-tag on each saved piece to disambiguate.");
		}
		equip_set_free_plan(plan);
		return false;
	}

	if (plan->cursed_count > 0) {
		for (j = 0; j < plan->cursed_count; j++) {
			char o_name[80];
			object_desc(o_name, sizeof(o_name), plan->cursed_objs[j],
				ODESC_PREFIX | ODESC_FULL, p);
			msg("You cannot remove the cursed %s.", o_name);
		}
	}

	if (plan->missing_count > 0) {
		for (j = 0; j < plan->missing_count; j++) {
			struct equip_set_slot *sslot = plan->missing_slots[j];
			if (sslot->artifact_name) {
				msg("You are missing the artifact %s.", sslot->artifact_name);
			} else {
				char buf[80];
				struct object_kind *kind = lookup_kind(sslot->tval, sslot->sval);
				if (kind) {
					strnfmt(buf, sizeof(buf), "%s", kind->name);
					if (sslot->ego_name) {
						my_strcat(buf, " (", sizeof(buf));
						my_strcat(buf, sslot->ego_name, sizeof(buf));
						my_strcat(buf, ")", sizeof(buf));
					}
					msg("You are missing %s.", buf);
				} else {
					msg("You are missing an item for a slot.");
				}
			}
		}
	}

	changed = equip_set_execute_plan(p, plan);
	equip_set_free_plan(plan);
	return changed;
}
