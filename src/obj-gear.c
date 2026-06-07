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
#include "obj-transfer.h"
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
void quiver_absorb_num(const struct player *p, const struct object *obj,
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
 * Wield or wear a single item from the pack or floor.
 *
 * Plan phase (no state changes):
 *   - If there is old equipment, verify it can fit back in the pack (taking
 *     into account that wielding a single-item pack stack frees one slot).
 *
 * Execute phase (only after plan is validated):
 *   - Split/take the new item from its source (pack or floor).
 *   - Handle old equipment: either leave it in gear as inventory or, if it
 *     cannot fit, explicitly remove it from gear and drop it (never rely
 *     on pack_overflow() to drop an unrelated item).
 *   - Put the new item into the equipment slot.
 */
void inven_wield(struct object *obj, int slot)
{
	struct object *wielded, *old = player->body.slots[slot].obj;
	struct obj_transfer_plan old_plan;
	bool old_fits = true;
	bool will_free_slot = false;

	const char *fmt;
	char o_name[80];
	char label_old[80] = {0};
	bool dummy = false;

	/* Take a turn */
	player->upkeep->energy_use = z_info->move_energy;

	/* ========== Plan phase: no state changes ========== */

	if (old) {
		/*
		 * Wearing from a single-item inventory stack frees one pack
		 * slot; old equipment is always a single item so it always
		 * fits in that freed slot.
		 */
		if (object_is_carried(player, obj) &&
				!object_is_equipped(player->body, obj) &&
				obj->number == 1) {
			will_free_slot = true;
		}

		/* Run plan to get merge info and raw capacity check */
		obj_transfer_plan_init(&old_plan, player, old);
		obj_transfer_plan_equip_to_pack(&old_plan);

		if (will_free_slot) {
			/* Freed 1 slot; old is 1 item, so it always fits */
			old_fits = true;
		} else {
			old_fits = old_plan.capacity_ok;
		}

		/* Pre-compute the old label before any state changes */
		object_desc(label_old, sizeof(label_old), old,
			ODESC_PREFIX | ODESC_FULL, player);
	}

	/* ========== Execute phase: modify state only now ========== */

	/* Increase equipment counter if the slot was empty */
	if (old == NULL)
		player->upkeep->equip_cnt++;

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
		/* Get a floor item and carry it (bypass capacity, it goes to equipment) */
		wielded = floor_object_for_use(player, obj, 1, false, &dummy);
		inven_carry(player, wielded, false, false);
	}

	/* --- Handle old equipment BEFORE we overwrite the slot --- */
	if (old) {
		/*
		 * Old item is no longer equipped; either it stays in the gear
		 * list as inventory, or we excise it and drop it explicitly.
		 */
		player->upkeep->equip_cnt--;

		if (old_fits) {
			/*
			 * Leave old in the gear list; since the slot is cleared
			 * it is no longer equipped and calc_inventory() will
			 * count it as inventory.  combine_pack() below merges it.
			 */
		} else {
			struct object *to_drop = old;

			disturb(player);
			msg("Your pack is full - you cannot carry any more.");
			msg("You drop %s.", label_old);

			/* Remove exactly this item from gear, not a random one */
			gear_excise_object(player, to_drop);
			drop_near(cave, &to_drop, 0, player->grid, false, true);
			msg("You no longer have %s.", label_old);

			event_signal(EVENT_INVENTORY);
			event_signal(EVENT_EQUIPMENT);
		}
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

	/*
	 * Merge inventory stacks.  This is still useful because the old
	 * equipment (if any) might now be mergeable with existing stacks.
	 */
	combine_pack(player);

	/*
	 * Safety net: pack_overflow() as a purely defensive measure.
	 * With proper plan-based capacity checks above, this should never
	 * trigger in normal operation.
	 */
	if (pack_is_overfull()) {
		pack_overflow(NULL);
	}

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
 * Plans the capacity first, then executes.  If the pack cannot accommodate
 * the item, the item being removed is dropped explicitly rather than relying
 * on pack_overflow() to drop some unrelated inventory item.
 */
void inven_takeoff(struct object *obj)
{
	int slot = equipped_item_slot(player->body, obj);
	const char *act;
	char o_name[80];
	char label;
	struct obj_transfer_plan plan;
	bool fits;

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

	/* Get the label before the item possibly leaves the gear list */
	label = gear_to_label(player, obj);

	/* --- Plan phase: no state changes yet --- */
	obj_transfer_plan_init(&plan, player, obj);
	obj_transfer_plan_equip_to_pack(&plan);
	fits = plan.capacity_ok;

	/* --- Execution phase: only modify state after plan confirmed --- */

	/* De-equip the object (regardless of whether it fits) */
	player->body.slots[slot].obj = NULL;
	player->upkeep->equip_cnt--;

	player->upkeep->update |= (PU_BONUS | PU_INVEN | PU_UPDATE_VIEW);
	player->upkeep->notice |= (PN_IGNORE);

	if (fits) {
		/*
		 * The item stays in the gear list; it is no longer equipped
		 * so calc_inventory() will count it as inventory.  The
		 * caller is responsible for combine_pack() as before.
		 */
		update_stuff(player);
	} else {
		struct object *dropped = obj;

		/*
		 * Pack is full and cannot merge.  Remove this exact item from
		 * the gear list (not a random one) and drop it on the ground.
		 */
		disturb(player);
		msg("Your pack is full - you cannot carry any more.");

		gear_excise_object(player, dropped);

		msg("You drop %s.", o_name);
		drop_near(cave, &dropped, 0, player->grid, false, true);
		msg("You no longer have %s.", o_name);

		update_stuff(player);

		/*
		 * Safety net: pack_overflow should never trigger in normal
		 * flow now, but call it as a defensive assertion.
		 */
		if (pack_is_overfull()) {
			pack_overflow(NULL);
		}

		event_signal(EVENT_INVENTORY);
		event_signal(EVENT_EQUIPMENT);

		msgt(MSG_WIELD, "%s %s (%c).", act, o_name, label);
		return;
	}

	/* Message */
	msgt(MSG_WIELD, "%s %s (%c).", act, o_name, label);

	return;
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
