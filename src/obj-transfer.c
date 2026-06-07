/**
 * \file obj-transfer.c
 * \brief Unified object transfer plan layer
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

#include "angband.h"
#include "cave.h"
#include "cmd-core.h"
#include "effects.h"
#include "game-event.h"
#include "game-input.h"
#include "generate.h"
#include "grafmode.h"
#include "init.h"
#include "mon-make.h"
#include "mon-util.h"
#include "monster.h"
#include "obj-curse.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-ignore.h"
#include "obj-info.h"
#include "obj-knowledge.h"
#include "obj-make.h"
#include "obj-pile.h"
#include "obj-slays.h"
#include "obj-transfer.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "player-calcs.h"
#include "player-history.h"
#include "player-spell.h"
#include "player-util.h"
#include "randname.h"
#include "trap.h"
#include "z-queue.h"


/**
 * Initialize an object transfer plan.
 */
void obj_transfer_plan_init(struct obj_transfer_plan *plan,
		struct player *p, struct object *source)
{
	memset(plan, 0, sizeof(*plan));
	plan->p = p;
	plan->source = source;
	plan->source_known = source ? source->known : NULL;
	plan->capacity_ok = true;
}

/**
 * Build a plan for moving objects from a floor pile to the player's pack.
 *
 * Handles quiver absorption, inventory stacking, and pack slot capacity.
 * Sets plan->movable to the total number that can be moved.
 *
 * \param plan The plan to fill in (must be initialized).
 * \param max_want Maximum number caller wants to move (0 = all of source).
 * \return true if any items can be moved.
 */
bool obj_transfer_plan_floor_to_pack(struct obj_transfer_plan *plan,
		int max_want)
{
	struct player *p = plan->p;
	struct object *obj = plan->source;
	int i, num_left;
	int n_free_slot;
	int num_to_quiver;
	int max_stack;

	if (!obj) return false;

	max_stack = obj->kind->base->max_stack;

	plan->requested = (max_want > 0 && max_want < obj->number)
		? max_want : obj->number;

	/* Treasure can always be picked up in full */
	if (tval_is_money(obj) && lookup_kind(obj->tval, obj->sval)) {
		plan->movable = plan->requested;
		plan->source_remaining = obj->number - plan->movable;
		plan->new_stack_amount = plan->movable;
		plan->needs_new_slot = (plan->new_stack_amount > 0);
		return true;
	}

	n_free_slot = z_info->pack_size - pack_slots_used(p);
	num_left = plan->requested;

	/* Absorb as many as we can in the quiver. */
	quiver_absorb_num(p, obj, &n_free_slot, &num_to_quiver);

	if (num_to_quiver > 0) {
		int qty = MIN(num_to_quiver, num_left);
		int remaining = qty;
		int desired_slot = preferred_quiver_slot(obj);
		bool ammo = tval_is_ammo(obj);
		int mult = ammo ? 1 : z_info->thrown_quiver_mult;

		for (i = 0; i < z_info->quiver_size && remaining > 0; i++) {
			struct object *quiver_obj = p->upkeep->quiver[i];
			if (quiver_obj &&
					object_stackable(quiver_obj, obj, OSTACK_PACK)) {
				int space = z_info->quiver_slot_size -
					quiver_obj->number * mult;
				if (space > 0) {
					int take = MIN(space / mult, remaining);
					if (take > 0) {
						plan->merges[plan->merge_count].dest_obj =
							quiver_obj;
						plan->merges[plan->merge_count].amount = take;
						plan->merges[plan->merge_count].mode =
							OSTACK_QUIVER;
						plan->merge_count++;
						remaining -= take;
					}
				}
			}
		}

		for (i = 0; i < z_info->quiver_size && remaining > 0; i++) {
			struct object *quiver_obj = p->upkeep->quiver[i];
			bool can_use_slot = false;

			if (!quiver_obj) {
				if (ammo || desired_slot == i) {
					can_use_slot = true;
				}
			} else if (!ammo && desired_slot == i &&
					preferred_quiver_slot(quiver_obj) != i) {
				can_use_slot = true;
			}

			if (can_use_slot) {
				int take = MIN(z_info->quiver_slot_size / mult,
					remaining);
				if (take > 0) {
					plan->merges[plan->merge_count].dest_obj = NULL;
					plan->merges[plan->merge_count].amount = take;
					plan->merges[plan->merge_count].mode =
						OSTACK_QUIVER;
					plan->merge_count++;
					remaining -= take;
				}
			}
		}

		num_left -= (qty - remaining);
	}

	/* Absorb into existing partial inventory stacks */
	if (num_left > 0) {
		for (i = 0; i < z_info->pack_size && num_left > 0; i++) {
			struct object *inven_obj = p->upkeep->inven[i];
			if (inven_obj && object_stackable(inven_obj, obj, OSTACK_PACK)) {
				int space = max_stack - inven_obj->number;
				if (space > 0) {
					int take = MIN(space, num_left);
					plan->merges[plan->merge_count].dest_obj = inven_obj;
					plan->merges[plan->merge_count].amount = take;
					plan->merges[plan->merge_count].mode = OSTACK_PACK;
					plan->merge_count++;
					num_left -= take;
				}
			}
		}
	}

	/* Use free pack slots for what's left */
	if (num_left > 0 && n_free_slot > 0) {
		int slots_needed = (num_left + max_stack - 1) / max_stack;
		int slots_use = MIN(slots_needed, n_free_slot);
		int can_fit = slots_use * max_stack;
		int take = MIN(can_fit, num_left);

		plan->new_stack_amount = take;
		plan->needs_new_slot = (take > 0);
		plan->capacity_extra_pack_slots = slots_use;
		num_left -= take;
	}

	plan->movable = plan->requested - num_left;
	plan->source_remaining = obj->number - plan->movable;
	plan->capacity_ok = (num_left == 0);

	return plan->movable > 0;
}

/**
 * Build a plan for moving objects from the player's pack/quiver/equipment
 * to a floor grid.
 *
 * \param plan The plan to fill in (must be initialized).
 * \param c The chunk (level).
 * \param grid The target floor grid.
 * \param max_want Maximum number caller wants to move (0 = all of source).
 * \return true if any items can be moved.
 */
bool obj_transfer_plan_pack_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, int max_want)
{
	struct object *obj = plan->source;
	struct object *floor_obj;
	int n = 0;
	int num_left;
	int max_stack;

	if (!obj) return false;
	if (!square_isobjectholding(c, grid)) return false;

	max_stack = obj->kind->base->max_stack;
	plan->requested = (max_want > 0 && max_want < obj->number)
		? max_want : obj->number;
	num_left = plan->requested;

	/* Scan objects in that grid for combination */
	for (floor_obj = square_object(c, grid);
			floor_obj && num_left > 0;
			floor_obj = floor_obj->next) {
		if (object_mergeable(floor_obj, obj, OSTACK_FLOOR)) {
			int space = max_stack - floor_obj->number;
			if (space > 0) {
				int take = MIN(space, num_left);
				plan->merges[plan->merge_count].dest_obj = floor_obj;
				plan->merges[plan->merge_count].amount = take;
				plan->merges[plan->merge_count].mode = OSTACK_FLOOR;
				plan->merge_count++;
				num_left -= take;
			}
		}
		n++;
	}

	/* Check if there's space for a new pile entry */
	if (num_left > 0) {
		if (n < z_info->floor_size &&
				(OPT(player, birth_stacking) || n == 0)) {
			plan->new_stack_amount = num_left;
			plan->needs_new_slot = true;
			num_left = 0;
		} else {
			/* Look for an ignored object to displace */
			struct object *ignore = NULL;
			for (floor_obj = square_object(c, grid);
					floor_obj;
					floor_obj = floor_obj->next) {
				if (ignore_item_ok(plan->p, floor_obj)) {
					ignore = floor_obj;
				}
			}
			if (ignore) {
				plan->new_stack_amount = num_left;
				plan->needs_new_slot = true;
				num_left = 0;
			}
		}
	}

	plan->movable = plan->requested - num_left;
	plan->source_remaining = obj->number - plan->movable;
	plan->capacity_ok = (num_left == 0);

	return plan->movable > 0;
}

/**
 * Build a plan for moving objects from one floor pile to another floor grid.
 *
 * \param plan The plan to fill in (must be initialized).
 * \param c The chunk (level).
 * \param grid The target floor grid.
 * \param max_want Maximum number caller wants to move (0 = all of source).
 * \return true if any items can be moved.
 */
bool obj_transfer_plan_floor_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, int max_want)
{
	return obj_transfer_plan_pack_to_floor(plan, c, grid, max_want);
}

/**
 * Build a plan for putting an equipped item back into the player's pack.
 *
 * Equipment items are always single items (obj->number == 1), so we just
 * check if there is pack space or an existing mergeable stack.
 *
 * \param plan The plan to fill in (must be initialized).
 * \return true if the item can fit in the pack.
 */
bool obj_transfer_plan_equip_to_pack(struct obj_transfer_plan *plan)
{
	return obj_transfer_plan_floor_to_pack(plan, 0);
}

/**
 * Split the source object according to the plan, returning the detached
 * portion.  If the whole stack is being moved, the source is excised from
 * whatever pile it was in and returned.
 *
 * This replaces the repeated pattern in floor_object_for_use() and
 * gear_object_for_use().
 */
struct object *obj_transfer_execute_split_source(
		struct obj_transfer_plan *plan)
{
	struct object *obj = plan->source;
	struct object *detached;
	bool none_left;

	if (plan->movable <= 0) return NULL;

	none_left = (plan->source_remaining == 0);

	if (plan->movable < obj->number) {
		detached = object_split(obj, plan->movable);
	} else {
		detached = obj;
		if (!loc_is_zero(obj->grid)) {
			if (obj->known) {
				square_excise_object(plan->p->cave, obj->grid,
					obj->known);
				delist_object(plan->p->cave, obj->known);
			}
			square_excise_object(cave, obj->grid, obj);
			delist_object(cave, obj);
		} else if (object_is_carried(plan->p, obj)) {
			struct player *p = plan->p;
			int i;

			pile_excise(&p->gear_k, obj->known);
			pile_excise(&p->gear, obj);

			p->upkeep->total_weight -=
				obj->number * object_weight_one(obj);

			for (i = 0; i < p->body.count; i++) {
				if (slot_object(p, i) == obj) {
					p->body.slots[i].obj = NULL;
					p->upkeep->equip_cnt--;
				}
			}

			calc_inventory(p);

			p->upkeep->update |= (PU_BONUS);
			p->upkeep->notice |= (PN_COMBINE);
			p->upkeep->redraw |= (PR_INVEN | PR_EQUIP);
		}
	}

	if (!loc_is_zero(detached->grid)) {
		detached->grid = loc(0, 0);
		if (detached->known) {
			detached->known->grid = loc(0, 0);
		}
	}

	if (none_left) {
		if (tracked_object_is(plan->p->upkeep, obj)) {
			track_object(plan->p->upkeep, NULL);
		}
		cmd_disable_repeat();
	}

	return detached;
}

/**
 * Execute a transfer plan by placing the detached object into the player's
 * pack, merging into existing stacks as planned and creating new stacks as
 * needed.
 *
 * \param plan The computed plan.
 * \param detached The detached portion of the source (from
 *        obj_transfer_execute_split_source).
 * \param absorb Whether to allow absorption into existing stacks (if false,
 *        only new stacks are created - only used by inven_wield).
 * \param message Whether to print pickup messages.
 */
void obj_transfer_execute_to_pack(struct obj_transfer_plan *plan,
		struct object *detached, bool absorb, bool message)
{
	struct player *p = plan->p;
	struct object *current = detached;
	struct object *combine_item = NULL;
	int i;
	bool combining = false;

	if (!detached || plan->movable <= 0) return;

	/* Process merge targets first */
	if (absorb && plan->merge_count > 0) {
		int remaining = detached->number;

		for (i = 0; i < plan->merge_count && remaining > 0; i++) {
			struct object *dest = plan->merges[i].dest_obj;
			int amt = plan->merges[i].amount;

			if (!dest) continue;
			if (amt <= 0) continue;
			amt = MIN(amt, remaining);

			if (amt == current->number) {
				/* Merge the entire current stack */
				p->upkeep->total_weight +=
					current->number * object_weight_one(current);
				object_absorb(dest->known, current->known);
				current->known = NULL;
				object_absorb(dest, current);
				dest->known->number = dest->number;
				combine_item = dest;
				combining = true;
				current = NULL;
				remaining = 0;
				break;
			} else {
				/* Partial absorption: split and merge */
				struct object *piece = object_split(current, amt);
				p->upkeep->total_weight +=
					piece->number * object_weight_one(piece);
				object_absorb(dest->known, piece->known);
				piece->known = NULL;
				object_absorb(dest, piece);
				dest->known->number = dest->number;
				combine_item = dest;
				combining = true;
				remaining -= amt;
			}
		}
	}

	/* Place remainder as new stacks */
	if (current && current->number > 0) {
		assert(pack_slots_used(p) <= z_info->pack_size);

		gear_insert_end(p, current);
		apply_autoinscription(p, current);

		current->held_m_idx = 0;
		current->grid = loc(0, 0);
		if (current->known) {
			current->known->grid = loc(0, 0);
		}

		p->upkeep->total_weight +=
			current->number * object_weight_one(current);
		p->upkeep->notice |= (PN_COMBINE);

		if (!object_flavor_is_aware(current)) {
			if (player_has(p, PF_KNOW_MUSHROOM) &&
					tval_is_mushroom(current)) {
				object_flavor_aware(p, current);
				msg("Mushrooms for breakfast!");
			} else if (player_has(p, PF_KNOW_ZAPPER) &&
					tval_is_zapper(current)) {
				object_flavor_aware(p, current);
			}
		}

		if (!combining) {
			combine_item = current;
		}
	}

	p->upkeep->update |= (PU_BONUS | PU_INVEN);
	p->upkeep->redraw |= (PR_INVEN);
	update_stuff(p);

	if (message && combine_item) {
		char o_name[80];
		struct object *first;
		uint16_t total;
		char label;

		if (tval_can_have_charges(combine_item) ||
				tval_is_rod(combine_item) ||
				combine_item->timeout > 0) {
			total = combine_item->number;
			first = combine_item;
		} else {
			total = object_pack_total(p, combine_item, false, &first);
		}
		assert(first && total >= first->number);
		object_desc(o_name, sizeof(o_name), combine_item,
			ODESC_PREFIX | ODESC_FULL | ODESC_ALTNUM |
			(total << 16), p);
		label = gear_to_label(p, first);
		if (total > first->number) {
			msg("You have %s (1st %c).", o_name, label);
		} else {
			assert(first == combine_item);
			msg("You have %s (%c).", o_name, label);
		}
	}

	if (combine_item && object_is_in_quiver(p, combine_item)) {
		sound(MSG_QUIVER);
	}
}

/**
 * Delete an object when the floor fails to carry it, and attempt to remove
 * it from the object list
 */
static void floor_carry_fail(struct chunk *c, struct object *drop, bool broke)
{
	struct object *known = drop->known;

	/* Delete completely */
	if (known) {
		char o_name[80];
		const char *verb = broke ?
			VERB_AGREEMENT(drop->number, "breaks", "break") :
			VERB_AGREEMENT(drop->number, "disappears", "disappear");
		object_desc(o_name, sizeof(o_name), drop, ODESC_BASE, player);
		msg("The %s %s.", o_name, verb);
		if (!loc_is_zero(known->grid))
			square_excise_object(player->cave, known->grid, known);
		delist_object(player->cave, known);
		object_delete(player->cave, NULL, &known);
	}
	delist_object(c, drop);
	object_delete(c, player->cave, &drop);
}

/**
 * Execute a transfer plan by placing the detached object onto a floor grid,
 * merging into existing piles as planned.
 *
 * \param plan The computed plan.
 * \param c The chunk (level).
 * \param grid The target floor grid.
 * \param detached The detached portion of the source.
 * \param note Set to false if the item should not be mentioned (ignored).
 */
void obj_transfer_execute_to_floor(struct obj_transfer_plan *plan,
		struct chunk *c, struct loc grid, struct object *detached,
		bool *note)
{
	struct object *current = detached;
	int i;

	if (!detached || plan->movable <= 0) return;

	/* Process merge targets first */
	if (plan->merge_count > 0) {
		int remaining = detached->number;

		for (i = 0; i < plan->merge_count && remaining > 0; i++) {
			struct object *dest = plan->merges[i].dest_obj;
			int amt = plan->merges[i].amount;

			if (!dest) continue;
			if (amt <= 0) continue;
			amt = MIN(amt, remaining);

			if (amt == current->number) {
				object_absorb(dest, current);
				current = NULL;
				remaining = 0;
				if (square_isview(c, grid)) {
					square_note_spot(c, grid);
				}
				if (ignore_item_ok(plan->p, dest) && note) {
					*note = false;
				}
				break;
			} else {
				struct object *piece = object_split(current, amt);
				object_absorb(dest, piece);
				remaining -= amt;
				if (square_isview(c, grid)) {
					square_note_spot(c, grid);
				}
				if (ignore_item_ok(plan->p, dest) && note) {
					*note = false;
				}
			}
		}
	}

	/* Place remainder as a new pile entry */
	if (current && current->number > 0) {
		struct object *ignore = NULL;
		struct object *obj_iter;
		int n = 0;

		for (obj_iter = square_object(c, grid);
				obj_iter;
				obj_iter = obj_iter->next) {
			n++;
			if (ignore_item_ok(plan->p, obj_iter)) {
				ignore = obj_iter;
			}
		}

		if (n >= z_info->floor_size ||
				(!OPT(plan->p, birth_stacking) && n)) {
			if (ignore) {
				struct chunk *p_c = (c == cave) ? plan->p->cave : NULL;
				square_excise_object(c, grid, ignore);
				delist_object(c, ignore);
				object_delete(c, p_c, &ignore);
			} else {
				floor_carry_fail(c, current, false);
				return;
			}
		}

		current->grid = grid;
		current->held_m_idx = 0;
		pile_insert(&c->squares[grid.y][grid.x].obj, current);
		list_object(c, current);

		if (current->known) {
			current->known->oidx = current->oidx;
			current->known->held_m_idx = 0;
			current->known->grid = loc(0, 0);
			plan->p->cave->objects[current->oidx] = current->known;
		}

		square_note_spot(c, grid);
		square_light_spot(c, grid);

		if (ignore_item_ok(plan->p, current) && note) {
			*note = false;
		}
	}
}

/**
 * Remove an amount of an object from the floor, returning a detached object
 * which can be used - it is assumed that the object is being manipulated by
 * given player and is on that player's grid.
 *
 * Optionally describe what remains.
 */
struct object *floor_object_for_use(struct player *p, struct object *obj,
	int num, bool message, bool *none_left)
{
	struct obj_transfer_plan plan;
	struct object *usable;
	char name[80];

	obj_transfer_plan_init(&plan, p, obj);
	plan.movable = MIN(num, obj->number);
	plan.source_remaining = obj->number - plan.movable;

	usable = obj_transfer_execute_split_source(&plan);
	*none_left = (plan.source_remaining == 0);

	/* Print a message if requested and there is anything left */
	if (message) {
		if (usable == obj)
			obj->number = 0;

		object_desc(name, sizeof(name), obj,
			ODESC_PREFIX | ODESC_FULL, p);

		if (usable == obj)
			obj->number = plan.movable;

		msg("You see %s.", name);
	}

	return usable;
}

/**
 * Find and return the oldest object on the given grid marked as "ignore".
 */
static struct object *floor_get_oldest_ignored(const struct player *p,
		struct chunk *c, struct loc grid)
{
	struct object *obj, *ignore = NULL;

	for (obj = square_object(c, grid); obj; obj = obj->next)
		if (ignore_item_ok(p, obj))
			ignore = obj;

	return ignore;
}

/**
 * Let the floor carry an object, deleting old ignored items if necessary.
 * The calling function must deal with the dropped object on failure.
 *
 * Optionally put the object at the top or bottom of the pile
 */
bool floor_carry(struct chunk *c, struct loc grid, struct object *drop,
				 bool *note)
{
	struct obj_transfer_plan plan;

	if (!square_isobjectholding(c, grid))
		return false;

	obj_transfer_plan_init(&plan, player, drop);
	if (!obj_transfer_plan_pack_to_floor(&plan, c, grid, 0)) {
		return false;
	}

	obj_transfer_execute_to_floor(&plan, c, grid, drop, note);
	return true;
}

/**
 * Find a grid near the given one for an object to fall on
 *
 * We check several locations to see if we can find a location at which
 * the object can combine, stack, or be placed.  Artifacts will try very
 * hard to be placed, including "teleporting" to a useful grid if needed.
 *
 * If prefer_pile is true, does not apply a penalty for putting different types
 * items in the same grid.
 *
 * If no appropriate grid is found, the given grid is unchanged
 */
static void drop_find_grid(const struct player *p, struct chunk *c,
		struct object *drop, bool prefer_pile, struct loc *grid)
{
	int best_score = -1;
	struct loc start = *grid;
	struct loc best = start;
	int i, dy, dx;
	struct object *obj;

	/* Scan local grids */
	for (dy = -3; dy <= 3; dy++) {
		for (dx = -3; dx <= 3; dx++) {
			bool combine = false;
			int dist = (dy * dy) + (dx * dx);
			struct loc try = loc_sum(start, loc(dx, dy));
			int num_shown = 0;
			int num_ignored = 0;
			int score;

			/* Lots of reasons to say no */
			if ((dist > 10) ||
				!square_in_bounds_fully(c, try) ||
				!los(c, start, try) ||
				!square_isfloor(c, try) ||
				square_istrap(c, try))
				continue;

			/* Analyse the grid for carrying the new object */
			for (obj = square_object(c, try); obj; obj = obj->next){
				/* Check for possible combination */
				if (object_mergeable(obj, drop, OSTACK_FLOOR))
					combine = true;

				/* Count objects */
				if (!ignore_item_ok(p, obj))
					num_shown++;
				else
					num_ignored++;
			}
			if (!combine)
				num_shown++;

			/* Disallow if the stack size is too big */
			if ((!OPT(p, birth_stacking) && (num_shown > 1)) ||
				((num_shown + num_ignored) > z_info->floor_size &&
				 !floor_get_oldest_ignored(p, c, try)))
				continue;

			/* Score the location based on how close and how full the grid is */
			score = 1000 -
				(dist + (prefer_pile ? 0 : num_shown * 5));

			if ((score < best_score) || ((score == best_score) && one_in_(2)))
				continue;

			best_score = score;
			best = try;
		}
	}

	/* Return if we have a score, otherwise fail or try harder for artifacts */
	if (best_score >= 0) {
		*grid = best;
		return;
	} else if (!drop->artifact) {
		return;
	}
	for (i = 0; i < 2000; i++) {
		/* Start bouncing from grid to grid, stopping if we find an empty one */
		if (i < 1000) {
			best = rand_loc(best, 1, 1);
			/* Keep in bounds. */
			best.x = MAX(0, MIN(best.x, c->width - 1));
			best.y = MAX(0, MIN(best.y, c->height - 1));
		} else {
			/* Now go to purely random locations */
			best = loc(randint0(c->width), randint0(c->height));
		}
		if (square_canputitem(c, best)) {
			*grid = best;
			return;
		}
	}
}

/**
 * Let an object fall to the ground at or near a location.
 *
 * The initial location is assumed to be "square_in_bounds_fully(cave, )".
 *
 * This function takes a parameter "chance".  This is the percentage
 * chance that the item will "disappear" instead of drop.  If the object
 * has been thrown, then this is the chance of disappearance on contact.
 *
 * This function will produce a description of a drop event under the player
 * when "verbose" is true.
 *
 * If "prefer_pile" is true, the penalty for putting different types of items
 * in the same square is not applied.
 *
 * The calling function needs to deal with the consequences of the dropped
 * object being destroyed or absorbed into an existing pile.
 */
void drop_near(struct chunk *c, struct object **dropped, int chance,
			   struct loc grid, bool verbose, bool prefer_pile)
{
	char o_name[80];
	struct loc best = grid;
	bool dont_ignore = verbose && !ignore_item_ok(player, *dropped);

	/* Only called in the current level */
	assert(c == cave);

	/* Describe object */
	object_desc(o_name, sizeof(o_name), *dropped, ODESC_BASE, player);

	/* Handle normal breakage */
	if (!((*dropped)->artifact) && (randint0(100) < chance)) {
		floor_carry_fail(c, *dropped, true);
		return;
	}

	/* Find the best grid and drop the item, destroying if there's no space */
	drop_find_grid(player, c, *dropped, prefer_pile, &best);
	if (floor_carry(c, best, *dropped, &dont_ignore)) {
		sound(MSG_DROP);
		if (dont_ignore && (square(c, best)->mon < 0)) {
			msg("You feel something roll beneath your feet.");
		}
	} else {
		floor_carry_fail(c, *dropped, false);
	}
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
	struct obj_transfer_plan plan;
	struct object *usable;
	struct object *first_remainder = NULL;
	char name[80];
	char label = gear_to_label(p, obj);
	bool artifact = (obj->known->artifact != NULL);

	obj_transfer_plan_init(&plan, p, obj);
	plan.movable = MIN(num, obj->number);
	plan.source_remaining = obj->number - plan.movable;

	/* Update weight for partial split */
	if (plan.movable < obj->number) {
		p->upkeep->total_weight -=
			plan.movable * object_weight_one(obj);
	}

	usable = obj_transfer_execute_split_source(&plan);
	*none_left = (plan.source_remaining == 0);

	if (message) {
		if (plan.source_remaining > 0) {
			uint16_t total;

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
		} else {
			if (artifact) {
				object_desc(name, sizeof(name), obj,
					ODESC_FULL | ODESC_SINGULAR, p);
			} else {
				uint16_t total;

				if (object_is_equipped(p->body, obj)
						|| tval_can_have_charges(obj)
						|| tval_is_rod(obj)
						|| obj->timeout > 0) {
					total = obj->number;
				} else {
					total = object_pack_total(p, obj,
						false, &first_remainder);
				}

				assert(total >= plan.movable);
				total -= plan.movable;
				if (!total || (first_remainder &&
						total <= first_remainder->number)) {
					first_remainder = NULL;
				}
				object_desc(name, sizeof(name), obj,
					ODESC_PREFIX | ODESC_FULL |
					ODESC_ALTNUM | (total << 16), p);
			}
		}
	}

	p->upkeep->update |= (PU_BONUS);
	p->upkeep->notice |= (PN_COMBINE);
	p->upkeep->redraw |= (PR_INVEN | PR_EQUIP);

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
 * Calculate how much of an item is can be carried in the inventory or quiver.
 */
int inven_carry_num(const struct player *p, const struct object *obj)
{
	struct obj_transfer_plan plan;
	obj_transfer_plan_init((struct obj_transfer_plan *)&plan,
		(struct player *)p, (struct object *)obj);
	if (obj_transfer_plan_floor_to_pack(&plan, 0)) {
		return plan.movable;
	}
	return 0;
}

/**
 * Check if we have space for some of an item in the pack.
 */
bool inven_carry_okay(const struct object *obj)
{
	return inven_carry_num(player, obj) > 0;
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
	struct obj_transfer_plan plan;

	obj_transfer_plan_init(&plan, p, obj);

	if (!absorb) {
		plan.movable = obj->number;
		plan.source_remaining = 0;
		plan.merge_count = 0;
		plan.new_stack_amount = obj->number;
		plan.needs_new_slot = true;
		plan.capacity_ok = true;
	} else {
		obj_transfer_plan_floor_to_pack(&plan, 0);
	}

	obj_transfer_execute_to_pack(&plan, obj, absorb, message);
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

		/*
		 * If the pack was full during takeoff, inven_takeoff() has
		 * already explicitly dropped the item to the floor and
		 * removed it from gear.  In that case we are done here;
		 * the remaining logic would try to re-drop it from gear
		 * where it no longer exists.
		 */
		if (!object_is_carried(player, obj)) {
			/* Sound for quiver objects */
			if (quiver)
				sound(MSG_QUIVER);

			event_signal(EVENT_INVENTORY);
			event_signal(EVENT_EQUIPMENT);
			return;
		}
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
 * Move all objects from a grid onto adjacent grids.
 *
 * Used when opening a door under an object pile.
 */
void push_object(struct loc grid)
{
	struct feature *feat_old = square_feat(cave, grid);
	struct object *obj = square_object(cave, grid);
	struct queue *queue = q_new(z_info->floor_size);
	struct trap *trap = square_trap(cave, grid);

	while (obj) {
		struct object *next = obj->next;
		struct object *newobj = object_new();

		object_copy(newobj, obj);
		newobj->oidx = 0;
		newobj->grid = loc(0, 0);
		if (newobj->known) {
			newobj->known = object_new();
			object_copy(newobj->known, obj->known);
			newobj->known->oidx = 0;
			newobj->known->grid = loc(0, 0);
		}
		q_push_ptr(queue, newobj);

		delist_object(cave, obj);
		object_delete(cave, player->cave, &obj);

		obj = next;
	}

	square_set_obj(cave, grid, NULL);

	square_force_floor(cave, grid);
	square_add_door(cave, grid, false);

	while (q_len(queue) > 0) {
		obj = q_pop_ptr(queue);

		if (obj->mimicking_m_idx) {
			struct monster *mimic =
				cave_monster(cave, obj->mimicking_m_idx);
			int d;

			assert(mimic);
			mimic->mimicked_obj = NULL;

			d = 1;
			while (1) {
				struct loc newgrid;
				bool dummy = true;

				if (d >= 4) {
					delete_monster_idx(cave, obj->mimicking_m_idx);
					if (obj->known) {
						object_delete(player->cave, NULL, &obj->known);
					}
					object_delete(cave, player->cave, &obj);
					break;
				}
				if (scatter_ext(cave, &newgrid, 1, grid, d,
						true, square_isempty) > 0
						&& floor_carry(cave, newgrid,
						obj, &dummy)) {
					monster_swap(grid, newgrid);
					mimic->mimicked_obj = obj;
					break;
				}
				++d;
			}
		} else {
			drop_near(cave, &obj, 0, grid, false, false);
		}
	}

	square_set_feat(cave, grid, feat_old->fidx);
	if (trap && !square_istrappable(cave, grid)) {
		square_destroy_trap(cave, grid);
	}

	q_free(queue);
}
