/**
 * \file obj-pile.c
 * \brief Deal with piles of objects
 *
 * Pile primitives only: object lifecycle, linked-list insert/excise,
 * stacking/merging helpers, floor scanning, pile charge display.
 *
 * Higher-level object movements (pickup, drop, carry, wield, takeoff) and
 * the unified quantity / capacity planning live in obj-transfer.c.
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
#include "game-input.h"
#include "init.h"
#include "monster.h"
#include "obj-curse.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-ignore.h"
#include "obj-knowledge.h"
#include "obj-pile.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "z-textblock.h"

/* #define LIST_DEBUG */

static struct object *fail_pile;
static struct object *fail_object;
static bool fail_prev;
static bool fail_next;
static const char *fail_file;
static int fail_line;

static void write_pile(ang_file *fff)
{
	file_putf(fff, "Pile integrity failure at %s:%d\n\n", fail_file, fail_line);
	file_putf(fff, "Guilty object\n=============\n");
	if (fail_object && fail_object->kind) {
		file_putf(fff, "Name: %s\n", fail_object->kind->name);
		if (fail_prev) {
			file_putf(fff, "Previous: ");
			if (fail_object->prev && fail_object->prev->kind) {
				file_putf(fff, "%s\n", fail_object->prev->kind->name);
			} else {
				file_putf(fff, "bad object\n");
			}
		}
		if (fail_next) {
			file_putf(fff, "Next: ");
			if (fail_object->next && fail_object->next->kind) {
				file_putf(fff, "%s\n", fail_object->next->kind->name);
			} else {
				file_putf(fff, "bad object\n");
			}
		}
		file_putf(fff, "\n");
	}
	if (fail_pile) {
		file_putf(fff, "Guilty pile\n=============\n");
		while (fail_pile) {
			if (fail_pile->kind) {
				file_putf(fff, "Name: %s\n", fail_pile->kind->name);
			} else {
				file_putf(fff, "bad object\n");
			}
			fail_pile = fail_pile->next;
		}
	}
}

/**
 * Quit on getting an object pile error, writing a diagnosis file
 */
static void pile_integrity_fail(struct object *pile, struct object *obj,
	const char *file, int line)
{
	char path[1024];

	/* Set the pile info to write out */
	fail_pile = pile;
	fail_object = obj;
	fail_prev = (obj->prev != NULL);
	fail_next = (obj->next != NULL);
	fail_file = file;
	fail_line = line;

	/* Write to the user directory */
	path_build(path, sizeof(path), ANGBAND_DIR_USER, "pile_error.txt");

	if (text_lines_to_file(path, write_pile)) {
		quit_fmt("Failed to create file %s.new", path);
	}
	quit_fmt("Pile integrity failure, details written to %s", path);
}

/**
 * Check the integrity of a linked - make sure it's not circular and that each
 * entry in the chain has consistent next and prev pointers.
 */
static void pile_check_integrity(const char *op, struct object *pile,
	struct object *hilight)
{
	struct object *obj = pile;
	struct object *prev = NULL;

#ifdef LIST_DEBUG
	int i = 0;
	fprintf(stderr, "\n%s  pile %08x\n", op, (int)pile);
#endif

	/* Check prev<->next chain */
	while (obj) {
#ifdef LIST_DEBUG
		fprintf(stderr, "[%2d] this = %08x  prev = %08x  next = %08x  %s\n",
			i, (int)obj, (int)obj->prev, (int)obj->next,
			(obj == hilight) ? "*" : "");
		i++;
#endif

		if (obj->prev != prev) {
			pile_integrity_fail(pile, obj, __FILE__, __LINE__);
		}
		prev = obj;
		obj = obj->next;
	};

	/* Check for circularity */
	for (obj = pile; obj; obj = obj->next) {
		struct object *check;
		for (check = obj->next; check; check = check->next) {
			if (check->next == obj) {
				pile_integrity_fail(pile, check, __FILE__, __LINE__);
			}
		}
	}
}

/**
 * Insert 'obj' into the pile 'pile'.
 *
 * 'obj' must not already be in any other lists.
 */
void pile_insert(struct object **pile, struct object *obj)
{
	if (obj->prev || obj->next) {
		pile_integrity_fail(NULL, obj, __FILE__, __LINE__);
	}

	if (*pile) {
		obj->next = *pile;
		(*pile)->prev = obj;
	}

	*pile = obj;

	pile_check_integrity("insert", *pile, obj);
}

/**
 * Insert 'obj' at the end of pile 'pile'.
 *
 * Unlike pile_insert(), obj can be the beginning of a new list of objects.
 */
void pile_insert_end(struct object **pile, struct object *obj)
{
	if (obj->prev) {
		pile_integrity_fail(NULL, obj, __FILE__, __LINE__);
	}

	if (*pile) {
		struct object *end = pile_last_item(*pile);

		end->next = obj;
		obj->prev = end;
	} else {
		*pile = obj;
	}

	pile_check_integrity("insert_end", *pile, obj);
}

/**
 * Remove object 'obj' from pile 'pile'.
 */
void pile_excise(struct object **pile, struct object *obj)
{
	struct object *prev = obj->prev;
	struct object *next = obj->next;

	if (!pile_contains(*pile, obj)) {
		pile_integrity_fail(*pile, obj, __FILE__, __LINE__);
	}
	pile_check_integrity("excise [pre]", *pile, obj);

	/* Special case: unlink top object */
	if (*pile == obj) {
		if (prev) {
			pile_integrity_fail(*pile, obj, __FILE__, __LINE__);
		}

		*pile = next;
	} else {
		if (obj->prev == NULL) {
			pile_integrity_fail(*pile, obj, __FILE__, __LINE__);
		}

		/* Otherwise unlink from the previous */
		prev->next = next;
		obj->prev = NULL;
	}

	/* And then unlink from the next */
	if (next) {
		next->prev = prev;
		obj->next = NULL;
	}

	pile_check_integrity("excise [post]", *pile, NULL);
}

/**
 * Return the last item in pile 'pile'.
 */
struct object *pile_last_item(struct object *const pile)
{
	struct object *obj = pile;

	pile_check_integrity("last_item", pile, NULL);

	/* No pile at all */
	if (!pile)
		return NULL;

	/* Run along the list, stopping just before the end */
	while (obj->next)
		obj = obj->next;

	return obj;
}

/**
 * Check if pile 'pile' contains object 'obj'.
 */
bool pile_contains(const struct object *top, const struct object *obj)
{
	const struct object *pile_obj = top;

	while (pile_obj) {
		if (obj == pile_obj)
			return true;
		pile_obj = pile_obj->next;
	}

	return false;
}

/**
 * Create a new object and return it
 */
struct object *object_new(void)
{
	return mem_zalloc(sizeof(struct object));
}

/**
 * Free up an object
 *
 * This doesn't affect any game state outside of the object itself
 */
void object_free(struct object *obj)
{
	mem_free(obj->slays);
	mem_free(obj->brands);
	mem_free(obj->curses);
	mem_free(obj);
}

/**
 * Delete an object and free its memory, and set its pointer to NULL
 * \param c is the chunk the object belongs to (usually)
 * \param p_c is the corresponding known chunk (eg player->cave if c is cave)
 * \param obj_address is the address of the struct object* to be deleted
 */
void object_delete(struct chunk *c, struct chunk *p_c,
				   struct object **obj_address)
{
	struct object *obj = *obj_address;
	struct object *prev = obj->prev;
	struct object *next = obj->next;

	/* Check any next and previous objects */
	if (next) {
		if (prev) {
			prev->next = next;
			next->prev = prev;
		} else {
			next->prev = NULL;
		}
	} else if (prev) {
		prev->next = NULL;
	}

	/* If we're tracking the object, stop */
	if (player && player->upkeep && obj == player->upkeep->object)
		player->upkeep->object = NULL;

	/* Orphan rather than actually delete if we still have a known object */
	if (c && p_c && obj->oidx && (obj == c->objects[obj->oidx]) &&
		p_c->objects[obj->oidx]) {
		obj->grid = loc(0, 0);
		obj->prev = NULL;
		obj->next = NULL;
		obj->held_m_idx = 0;
		obj->mimicking_m_idx = 0;

		/* Object is now purely imaginary to the player */
		obj->known->notice |= OBJ_NOTICE_IMAGINED;

		return;
	}

	/* Remove from any lists */
	if (p_c && p_c->objects && obj->oidx && (obj == p_c->objects[obj->oidx]))
		p_c->objects[obj->oidx] = NULL;

	if (c && c->objects && obj->oidx && (obj == c->objects[obj->oidx]))
		c->objects[obj->oidx] = NULL;

	object_free(obj);
	*obj_address = NULL;
}

/**
 * Free an entire object pile
 * \param c is the chunk holding the pile; should be NULL for piles held by
 * players or stores.
 * \param p_c is the player's view of the chunk holding the pile; should be NULL
 * when excising a pile in the player's view or for piles held by players or
 * stores.
 * \param obj is the pointer to the start of the pile to excise.
 */
void object_pile_free(struct chunk *c, struct chunk *p_c, struct object *obj)
{
	struct object *current = obj, *next;

	while (current) {
		next = current->next;
		object_delete(c, p_c, &current);
		current = next;
	}
}


/**
 * Determine if, ignoring any inscriptions, one item like obj1 can be stacked
 * with one item like obj2.
 *
 * See "object_absorb()" for the actual "absorption" code.
 *
 * If permitted, we allow weapons/armor to stack, if "known".
 *
 * Missiles will combine if both stacks have the same "known" status.
 * This is done to make unidentified stacks of missiles useful.
 *
 * Food, potions, scrolls, and "easy know" items always stack.
 *
 * Chests, and activatable items, except rods, never stack (for various
 * reasons).
 */
bool object_similar(const struct object *obj1, const struct object *obj2,
					  object_stack_t mode)
{
	int i;

	/* Equipment items don't stack */
	if (object_is_equipped(player->body, obj1))
		return false;
	if (object_is_equipped(player->body, obj2))
		return false;

	/* Mimicked items do not stack */
	if (obj1->mimicking_m_idx || obj2->mimicking_m_idx) return false;

	/* If either item is unknown, do not stack */
	if (mode & OSTACK_LIST && obj1->kind != obj1->known->kind) return false;
	if (mode & OSTACK_LIST && obj2->kind != obj2->known->kind) return false;

	/* Identical items cannot be stacked */
	if (obj1 == obj2) return false;

	/* Require identical object kinds */
	if (obj1->kind != obj2->kind) return false;

	/* Different flags don't stack */
	if (!of_is_equal(obj1->flags, obj2->flags)) return false;

	/* Different elements don't stack */
	for (i = 0; i < ELEM_MAX; i++) {
		if (obj1->el_info[i].res_level != obj2->el_info[i].res_level)
			return false;
		if ((obj1->el_info[i].flags & (EL_INFO_HATES | EL_INFO_IGNORE)) !=
			(obj2->el_info[i].flags & (EL_INFO_HATES | EL_INFO_IGNORE)))
			return false;
	}

	/* Artifacts never stack */
	if (obj1->artifact || obj2->artifact) return false;

	/* Analyze the items */
	if (tval_is_chest(obj1)) {
		/* Chests never stack */
		return false;
	}
	else if (tval_is_edible(obj1) || tval_is_potion(obj1) ||
		tval_is_scroll(obj1) || tval_is_rod(obj1)) {
		/* Food, potions, scrolls and rods all stack nicely,
		   since the kinds are identical, either both will be
		   aware or both will be unaware */
	} else if (tval_can_have_charges(obj1) || tval_is_money(obj1)) {
		/* Gold, staves and wands stack most of the time */
		/* Too much gold or too many charges */
		if (obj1->pval + obj2->pval > MAX_PVAL)
			return false;

		/* ... otherwise ok */
	} else if (tval_is_weapon(obj1) || tval_is_armor(obj1) ||
		tval_is_jewelry(obj1) || tval_is_light(obj1)) {
		bool obj1_is_known = object_fully_known(obj1);
		bool obj2_is_known = object_fully_known(obj2);

		/* Require identical values */
		if (obj1->ac != obj2->ac) return false;
		if (obj1->dd != obj2->dd) return false;
		if (obj1->ds != obj2->ds) return false;

		/* Require identical bonuses */
		if (obj1->to_h != obj2->to_h) return false;
		if (obj1->to_d != obj2->to_d) return false;
		if (obj1->to_a != obj2->to_a) return false;

		/* Require all identical modifiers */
		for (i = 0; i < OBJ_MOD_MAX; i++)
			if (obj1->modifiers[i] != obj2->modifiers[i])
				return (false);

		/* Require identical ego-item types */
		if (obj1->ego != obj2->ego) return false;

		/* Require identical curses */
		if (!curses_are_equal(obj1, obj2)) return false;

		/* Hack - Never stack recharging wearables ... */
		if ((obj1->timeout || obj2->timeout) &&
			!tval_is_light(obj1)) return false;

		/* ... and lights must have same amount of fuel */
		else if ((obj1->timeout != obj2->timeout) &&
				 tval_is_light(obj1)) return false;

		/* Prevent unIDd items stacking with IDd items in the object list */
		if (mode & OSTACK_LIST && (obj1_is_known != obj2_is_known))
			return false;
	} else {
		/* Anything else probably ok */
	}

	/* They must be similar enough */
	return true;
}

/**
 * Determine if one item like obj1 can be stacked with one item like obj2
 * (i.e. identical to object_similar() except for the inscription check).
 */
bool object_stackable(const struct object *obj1, const struct object *obj2,
					  object_stack_t mode)
{
	if (object_similar(obj1, obj2, mode)) {
		/* Require compatible inscriptions */
		return !obj1->note || !obj2->note || obj1->note == obj2->note;
	}
	return false;
}

/**
 * Return whether each stack of objects can be merged into one stack.
 */
bool object_mergeable(const struct object *obj1, const struct object *obj2,
					object_stack_t mode)
{
	int total = obj1->number + obj2->number;

	/* Check against stacking limit - except in stores which absorb anyway */
	if (!(mode & OSTACK_STORE)) {
		if (total > obj1->kind->base->max_stack) {
			return false;
		}
		/* The quiver can impose stricter limits. */
		if (mode & OSTACK_QUIVER) {
			if (tval_is_ammo(obj1)) {
				if (total > z_info->quiver_slot_size) {
					return false;
				}
			} else {
				if (total > z_info->quiver_slot_size /
						z_info->thrown_quiver_mult) {
					return false;
				}
			}
		}
	}

	return object_stackable(obj1, obj2, mode);
}

/**
 * Combine the origins of two objects
 */
void object_origin_combine(struct object *obj1, const struct object *obj2)
{
	if (obj1->origin_race != obj2->origin_race) {
		bool uniq1 = (obj1->origin_race
			&& rf_has(obj1->origin_race->flags, RF_UNIQUE));
		bool uniq2 = (obj2->origin_race
			&& rf_has(obj2->origin_race->flags, RF_UNIQUE));

		if (uniq1 && !uniq2) {
			/* Favour keeping record for a unique */
			;
		} else if (uniq2 && !uniq1) {
			/* Favour keeping record for a unique */
			obj1->origin = obj2->origin;
			obj1->origin_depth = obj2->origin_depth;
			obj1->origin_race = obj2->origin_race;
		} else {
			/* Different monsters, neither or both unique, mixed origin */
			obj1->origin = ORIGIN_MIXED;
		}
	} else if (obj1->origin != obj2->origin
			|| obj1->origin_depth != obj2->origin_depth) {
		obj1->origin = ORIGIN_MIXED;
	}
}

/**
 * Allow one item to "absorb" another, assuming they are similar.
 *
 * The blending of the "note" field assumes that either (1) one has an
 * inscription and the other does not, or (2) neither has an inscription.
 * In both these cases, we can simply use the existing note, unless the
 * blending object has a note, in which case we use that note.
 *
* These assumptions are enforced by the "object_mergeable()" code.
 */
static void object_absorb_merge(struct object *obj1, const struct object *obj2,
		bool combine_charges_timeouts)
{
	int total;

	/* First object gains any extra knowledge from second */
	if (obj1->known && obj2->known) {
		if (obj2->known->effect)
			obj1->known->effect = obj1->effect;
		player_know_object(player, obj1);
	}

	/* Merge inscriptions */
	if (obj2->note)
		obj1->note = obj2->note;

	if (combine_charges_timeouts) {
		/* Combine timeouts for rod stacking */
		if (tval_can_have_timeout(obj1))
			obj1->timeout += obj2->timeout;

		/* Combine pvals for wands and staves */
		if (tval_can_have_charges(obj1) || tval_is_money(obj1)) {
			total = obj1->pval + obj2->pval;
			obj1->pval = total >= MAX_PVAL ? MAX_PVAL : total;
		}
	}

	/* Combine origin data as best we can */
	object_origin_combine(obj1, obj2);
}

/**
 * Merge a smaller stack into a larger stack, leaving two uneven stacks.
 * \param obj1 Is the first of the stacks to combine.  When the stacking
 * limits (from mode1 and mode2) are the same, this stack will be larger
 * when the function returns.
 * \param obj2 Is the second of the stacks to combine.
 * \param mode1 Describes the behavior, most notably the upper limit on size,
 * for the first stack. Can not include OSTACK_STORE, which typically has no
 * limit on the stack size.
 * \param mode2 Describes the behavior, most notable the upper limit on size,
 * for the second stack.  Can not include OSTACK_STORE, which typically has
 * no limit on the stack size.
 */
void object_absorb_partial(struct object *obj1, struct object *obj2,
	object_stack_t mode1, object_stack_t mode2)
{
	int smallest = MIN(obj1->number, obj2->number);
	int largest = MAX(obj1->number, obj2->number);
	int newsz1, newsz2;

	assert(!(mode1 & OSTACK_STORE) && !(mode2 & OSTACK_STORE));

	/* The quiver can have stricter limits. */
	if (mode1 & OSTACK_QUIVER) {
		int limit = z_info->quiver_slot_size /
			(tval_is_ammo(obj1) ?
			1 : z_info->thrown_quiver_mult);

		if (mode2 & OSTACK_QUIVER) {
			int difference = limit - largest;

			newsz1 = largest + difference;
			newsz2 = smallest - difference;
		} else {
			/* Handle the possibly different limits. */
			newsz1 = limit;
			newsz2 = (largest + smallest) - limit;
			assert(newsz2 < obj1->kind->base->max_stack);
		}
	} else if (mode2 & OSTACK_QUIVER) {
		/* Handle the possibly different limits. */
		int limit = z_info->quiver_slot_size /
			(tval_is_ammo(obj2) ?
			1 : z_info->thrown_quiver_mult);

		newsz1 = (largest + smallest) - limit;
		newsz2 = limit;
		assert(newsz1 < obj1->kind->base->max_stack);
	} else {
		int difference = obj1->kind->base->max_stack - largest;

		newsz1 = largest + difference;
		newsz2 = smallest - difference;
	}

	distribute_charges(obj2, obj1, obj2->number - newsz2, false);
	obj1->number = newsz1;
	obj2->number = newsz2;

	object_absorb_merge(obj1, obj2, tval_is_money(obj1));
}

/**
 * Merge two stacks into one stack.
 */
void object_absorb(struct object *obj1, struct object *obj2)
{
	struct object *known = obj2->known;
	int total = obj1->number + obj2->number;

	/* Add together the item counts */
	obj1->number = MIN(total, obj1->kind->base->max_stack);

	object_absorb_merge(obj1, obj2, true);
	if (known) {
		if (!loc_is_zero(known->grid)) {
			square_excise_object(player->cave, known->grid, known);
		}
		delist_object(player->cave, known);
		object_delete(player->cave, NULL, &known);
	}
	object_delete(cave, player->cave, &obj2);
}

/**
 * Wipe an object clean.
 */
void object_wipe(struct object *obj)
{
	/* Free slays and brands */
	mem_free(obj->slays);
	mem_free(obj->brands);
	mem_free(obj->curses);

	/* Wipe the structure */
	memset(obj, 0, sizeof(*obj));
}


/**
 * Prepare an object based on an existing object
 */
void object_copy(struct object *dest, const struct object *src)
{
	/* Copy the structure */
	memcpy(dest, src, sizeof(struct object));

	if (src->slays) {
		dest->slays = mem_zalloc(z_info->slay_max * sizeof(bool));
		memcpy(dest->slays, src->slays, z_info->slay_max * sizeof(bool));
	}
	if (src->brands) {
		dest->brands = mem_zalloc(z_info->brand_max * sizeof(bool));
		memcpy(dest->brands, src->brands, z_info->brand_max * sizeof(bool));
	}
	if (src->curses) {
		size_t array_size = z_info->curse_max * sizeof(struct curse_data);
		dest->curses = mem_zalloc(array_size);
		memcpy(dest->curses, src->curses, array_size);
	}

	/* Detach from any pile */
	dest->prev = NULL;
	dest->next = NULL;
}

/**
 * Prepare an object `dst` representing `amt` objects,  based on an existing
 * object `src` representing at least `amt` objects.
 *
 * Takes care of the charge redistribution concerns of stacked items.
 */
void object_copy_amt(struct object *dest, struct object *src, int amt)
{
	int charge_time = randcalc(src->time, 0, AVERAGE), max_time;

	/* Get a copy of the object */
	object_copy(dest, src);

	/* Modify quantity */
	dest->number = amt;
	dest->note = src->note;

	/*
	 * If the item has charges/timeouts, set them to the correct level
	 * too. We split off the same amount as distribute_charges.
	 */
	if (tval_can_have_charges(src))
		dest->pval = src->pval * amt / src->number;

	if (tval_can_have_timeout(src)) {
		max_time = charge_time * amt;

		if (src->timeout > max_time)
			dest->timeout = max_time;
		else
			dest->timeout = src->timeout;
	}
}

/**
 * Split off 'amt' items from 'src' and return.
 *
 * Where object_copy_amt() makes `amt` new objects, this function leaves the
 * total number unchanged; otherwise the two functions are similar.
 *
 * This function should only be used when amt < src->number
 */
struct object *object_split(struct object *src, int amt)
{
	struct object *dest = object_new(), *dest_known;

	/* Get a copy of the object */
	object_copy(dest, src);

	/* Do we need a new known object? */
	if (src->known) {
		/* Ensure numbers are aligned (should not be necessary, but safer) */
		src->known->number = src->number;

		/* Make the new object */
		dest_known = object_new();
		object_copy(dest_known, src->known);
		dest->known = dest_known;
	}

	/* Check legality */
	assert(src->number > amt);

	/* Distribute charges of wands, staves, or rods */
	distribute_charges(src, dest, amt, true);
	if (src->known)
		distribute_charges(src->known, dest->known, amt, true);

	/* Modify quantity */
	dest->number = amt;
	src->number -= amt;
	if (src->note)
		dest->note = src->note;
	if (src->known) {
		dest->known->number = dest->number;
		src->known->number = src->number;
		dest->known->note = src->known->note;
	}

	/* Remove any index */
	if (dest->known)
		dest->known->oidx = 0;
	dest->oidx = 0;

	return dest;
}

/**
 * Describe the charges on an item on the floor.
 */
void floor_item_charges(struct object *obj)
{
	/* Require staff/wand */
	if (!tval_can_have_charges(obj)) return;

	/* Require known item */
	if (!object_flavor_is_aware(obj)) return;

	/* Print a message */
	msg("There %s %d charge%s remaining.", (obj->pval != 1) ? "are" : "is",
	     obj->pval, (obj->pval != 1) ? "s" : "");
}



/**
 * Get a list of the objects at the player's location.
 *
 * Return the number of objects acquired.
 */
int scan_floor(struct object **items, int max_size, struct player *p,
		object_floor_t mode, item_tester tester)
{
	struct object *obj;
	int num = 0;

	/* Sanity */
	if (!square_in_bounds(cave, p->grid)) return 0;

	/* Scan all objects in the grid */
	for (obj = square_object(cave, p->grid); obj; obj = obj->next) {
		/* Enforce limit */
		if (num >= max_size) break;

		/* Item tester */
		if ((mode & OFLOOR_TEST) && !object_test(tester, obj)) continue;

		/* Sensed or known */
		if ((mode & OFLOOR_SENSE) && (!obj->known)) continue;

		/* Visible */
		if ((mode & OFLOOR_VISIBLE) && !is_unknown(obj)
				&& ignore_item_ok(p, obj)) continue;

		/* Accept this item */
		items[num++] = obj;

		/* Only one */
		if (mode & OFLOOR_TOP) break;
	}

	return num;
}

/**
 * Get a list of the known objects at the given location.
 *
 * Return the number of objects acquired.
 */
int scan_distant_floor(struct object **items, int max_size, struct player *p,
		struct loc grid)
{
	struct object *obj;
	int num = 0;

	/* Sanity */
	if (!square_in_bounds(p->cave, grid)) return 0;

	/* Scan all objects in the grid */
	for (obj = square_object(p->cave, grid); obj; obj = obj->next) {
		/* Enforce limit */
		if (num >= max_size) break;

		/* Known */
		if (obj->kind == unknown_item_kind) continue;

		/* Visible */
		if (ignore_known_item_ok(p, obj)) continue;

		/* Accept this item's base object */
		items[num++] = cave->objects[obj->oidx];
	}

	return num;
}

/**
 * Get a list of "valid" objects.
 *
 * Fills item_list[] with items that are "okay" as defined by the
 * provided tester function, etc.  mode determines what combination of
 * inventory, equipment, quiver and player's floor location should be used
 * when drawing up the list.
 *
 * Returns the number of items placed into the list.
 *
 * Maximum space that can be used is
 * z_info->pack_size + z_info->quiver_size + player->body.count +
 * z_info->floor_size,
 * though practically speaking much smaller numbers are likely.
 */
int scan_items(struct object **item_list, size_t item_max, struct player *p,
		int mode, item_tester tester)
{
	bool use_inven = ((mode & USE_INVEN) ? true : false);
	bool use_equip = ((mode & USE_EQUIP) ? true : false);
	bool use_quiver = ((mode & USE_QUIVER) ? true : false);
	bool use_floor = ((mode & USE_FLOOR) ? true : false);

	int floor_max = z_info->floor_size;
	struct object **floor_list = mem_zalloc(floor_max * sizeof(struct object *));
	int floor_num;

	int i;
	size_t item_num = 0;

	if (use_inven)
		for (i = 0; i < z_info->pack_size && item_num < item_max; i++) {
			if (object_test(tester, p->upkeep->inven[i]))
				item_list[item_num++] = p->upkeep->inven[i];
		}

	if (use_equip)
		for (i = 0; i < p->body.count && item_num < item_max; i++) {
			if (object_test(tester, slot_object(p, i)))
				item_list[item_num++] = slot_object(p, i);
		}

	if (use_quiver)
		for (i = 0; i < z_info->quiver_size && item_num < item_max; i++) {
			if (object_test(tester, p->upkeep->quiver[i]))
				item_list[item_num++] = p->upkeep->quiver[i];
		}

	/* Scan all non-gold objects in the grid */
	if (use_floor) {
		floor_num = scan_floor(floor_list, floor_max, p,
			OFLOOR_TEST | OFLOOR_SENSE | OFLOOR_VISIBLE, tester);

		for (i = 0; i < floor_num && item_num < item_max; i++)
			item_list[item_num++] = floor_list[i];
	}

	mem_free(floor_list);
	return item_num;
}


/**
 * Check if the given item is available for the player to use.
 */
bool item_is_available(struct object *obj)
{
	if (object_is_carried(player, obj)) return true;
	if (cave && square_holds_object(cave, player->grid, obj))
		return true;
	return false;
}
