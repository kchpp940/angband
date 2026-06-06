/**
 * \file floor-obj.c
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

#include "angband.h"
#include "cave.h"
#include "generate.h"
#include "init.h"
#include "mon-make.h"
#include "mon-util.h"
#include "monster.h"
#include "obj-gear.h"
#include "obj-make.h"
#include "obj-pile.h"
#include "object.h"
#include "player-calcs.h"
#include "player-quest.h"
#include "player-util.h"
#include "savefile.h"
#include "z-type.h"
#include "z-virt.h"
#include "floor-obj.h"

/**
 * Initialize floor objectives for a chunk
 */
void floor_obj_init(struct chunk *c)
{
	int i;

	if (!c) return;

	c->floor_obj.count = 0;
	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		c->floor_obj.objs[i].type = FLOOR_OBJ_NONE;
		c->floor_obj.objs[i].state = FLOOR_OBJ_INACTIVE;
		c->floor_obj.objs[i].description = NULL;
		c->floor_obj.objs[i].hint = NULL;
		c->floor_obj.objs[i].target_grid = loc(0, 0);
		c->floor_obj.objs[i].target_grid2 = loc(0, 0);
		c->floor_obj.objs[i].reward_type = FLOOR_REWARD_GOLD;
		c->floor_obj.objs[i].reward_value = 0;
		c->floor_obj.objs[i].reward_claimed = false;
		memset(&c->floor_obj.objs[i].data, 0, sizeof(c->floor_obj.objs[i].data));
	}
}

/**
 * Free floor objectives for a chunk
 */
void floor_obj_free(struct chunk *c)
{
	int i;

	if (!c) return;

	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		if (c->floor_obj.objs[i].description) {
			string_free(c->floor_obj.objs[i].description);
			c->floor_obj.objs[i].description = NULL;
		}
		if (c->floor_obj.objs[i].hint) {
			string_free(c->floor_obj.objs[i].hint);
			c->floor_obj.objs[i].hint = NULL;
		}
	}
	c->floor_obj.count = 0;
}

/**
 * Get a random reward type based on depth
 */
static floor_reward_type floor_obj_random_reward(int depth)
{
	int roll = randint0(100);

	if (roll < 40) return FLOOR_REWARD_GOLD;
	if (roll < 75) return FLOOR_REWARD_EXP;
	if (roll < 90) return FLOOR_REWARD_OBJECT;
	return FLOOR_REWARD_HEAL;
}

/**
 * Calculate reward value based on depth and difficulty
 */
static int floor_obj_calc_reward_value(int depth, floor_reward_type type)
{
	switch (type) {
		case FLOOR_REWARD_GOLD:
			return (50 + depth * 20) * rand_range(1, 3);
		case FLOOR_REWARD_EXP:
			return (100 + depth * 50) * rand_range(1, 2);
		case FLOOR_REWARD_OBJECT:
			return depth + randint0(5);
		case FLOOR_REWARD_HEAL:
			return 30 + depth * 5;
		default:
			return 0;
	}
}

/**
 * Count monsters in a rectangular area
 */
static int floor_obj_count_monsters_in_area(struct chunk *c, struct loc g1, struct loc g2,
										   struct monster_race *race)
{
	int count = 0;
	int x, y;
	int x1 = MIN(g1.x, g2.x), x2 = MAX(g1.x, g2.x);
	int y1 = MIN(g1.y, g2.y), y2 = MAX(g1.y, g2.y);

	for (y = y1; y <= y2; y++) {
		for (x = x1; x <= x2; x++) {
			struct loc grid = loc(x, y);
			struct monster *m = square_monster(c, grid);
			if (m) {
				if (!race || m->race == race) {
					count++;
				}
			}
		}
	}
	return count;
}

/**
 * Generate a CLEAR_NEST objective
 */
static bool floor_obj_gen_clear_nest(struct chunk *c, struct player *p,
									 struct floor_objective *obj)
{
	struct loc grid;
	int x, y;
	int nest_half = 5 + randint0(4);
	int tries = 0;

	while (tries < 50) {
		grid = loc(rand_range(nest_half + 2, c->width - nest_half - 3),
				   rand_range(nest_half + 2, c->height - nest_half - 3));

		if (square_isfloor(c, grid)) break;
		tries++;
	}
	if (tries >= 50) return false;

	obj->type = FLOOR_OBJ_CLEAR_NEST;
	obj->target_grid = grid;
	obj->target_grid2 = loc(grid.x + nest_half, grid.y + nest_half);
	obj->target_grid = loc(grid.x - nest_half, grid.y - nest_half);
	obj->target_grid2 = loc(grid.x + nest_half, grid.y + nest_half);

	{
		int mon_count = 4 + randint0(4);
		struct monster_race *race = get_mon_num(c->depth, c->depth);
		struct monster_group_info info = { 0, 0 };

		if (!race) return false;

		obj->data.nest.race = race;
		obj->data.nest.total = 0;
		obj->data.nest.killed = 0;

		for (x = obj->target_grid.x; x <= obj->target_grid2.x; x++) {
			for (y = obj->target_grid.y; y <= obj->target_grid2.y; y++) {
				struct loc mg = loc(x, y);
				if (square_isfloor(c, mg) || square_isempty(c, mg)) {
					if (one_in_(3)) {
						square_set_feat(c, mg, FEAT_FLOOR);
					}
				}
			}
		}

		for (tries = 0; tries < mon_count * 3 && obj->data.nest.total < mon_count; tries++) {
			struct loc mg;
			mg = loc(rand_range(obj->target_grid.x, obj->target_grid2.x),
					 rand_range(obj->target_grid.y, obj->target_grid2.y));
			if (square_isempty(c, mg)) {
				if (place_new_monster(c, mg, race, true, true, info, ORIGIN_DROP)) {
					obj->data.nest.total++;
				}
			}
		}

		if (obj->data.nest.total == 0) return false;
	}

	obj->description = string_make(format("Clear the monster nest (%d creatures)", obj->data.nest.total));
	obj->hint = string_make("You sense a concentration of monsters nearby...");
	return true;
}

/**
 * Generate a FIND_HIDDEN objective
 */
static bool floor_obj_gen_find_hidden(struct chunk *c, struct player *p,
									  struct floor_objective *obj)
{
	struct loc grid;
	int tries = 0;
	int room_size = 4 + randint0(3);

	while (tries < 30) {
		grid = loc(rand_range(room_size + 3, c->width - room_size - 4),
				   rand_range(room_size + 3, c->height - room_size - 4));

		bool all_rock = true;
		int x, y;
		for (y = grid.y - room_size - 1; y <= grid.y + room_size + 1; y++) {
			for (x = grid.x - room_size - 1; x <= grid.x + room_size + 1; x++) {
				struct loc rg = loc(x, y);
				if (square_in_bounds(c, rg) && !square_isrock(c, rg) && !square_isperm(c, rg)) {
					all_rock = false;
					break;
				}
			}
			if (!all_rock) break;
		}
		if (all_rock) break;
		tries++;
	}
	if (tries >= 30) return false;

	obj->type = FLOOR_OBJ_FIND_HIDDEN;
	obj->target_grid = loc(grid.x - room_size, grid.y - room_size);
	obj->target_grid2 = loc(grid.x + room_size, grid.y + room_size);
	obj->data.hidden.found = false;
	obj->data.hidden.room_id = -1;

	{
		int x, y;
		for (y = obj->target_grid.y; y <= obj->target_grid2.y; y++) {
			for (x = obj->target_grid.x; x <= obj->target_grid2.x; x++) {
				struct loc rg = loc(x, y);
				if (square_in_bounds(c, rg)) {
					square_set_feat(c, rg, FEAT_FLOOR);
					sqinfo_on(square(c, rg)->info, SQUARE_ROOM);
				}
			}
		}

		{
			struct loc door_grid;
			int door_side = randint0(4);
			switch (door_side) {
				case 0:
					door_grid = loc(grid.x, obj->target_grid.y);
					break;
				case 1:
					door_grid = loc(grid.x, obj->target_grid2.y);
					break;
				case 2:
					door_grid = loc(obj->target_grid.x, grid.y);
					break;
				default:
					door_grid = loc(obj->target_grid2.x, grid.y);
					break;
			}
			square_set_feat(c, door_grid, FEAT_SECRET);
			sqinfo_on(square(c, door_grid)->info, SQUARE_ROOM);
		}

		{
			int obj_level = c->depth > 0 ? c->depth : 1;
			struct loc item_grid = grid;
			place_object(c, item_grid, obj_level, one_in_(3), one_in_(6), ORIGIN_SPECIAL, 0);
			if (one_in_(3)) {
				place_gold(c, item_grid, obj_level, ORIGIN_SPECIAL);
			}
		}
	}

	obj->description = string_make("Find the hidden chamber");
	obj->hint = string_make("There might be a concealed room somewhere on this level...");
	return true;
}

/**
 * Generate a RETRIEVE_ITEM objective
 */
static bool floor_obj_gen_retrieve_item(struct chunk *c, struct player *p,
										struct floor_objective *obj)
{
	struct loc grid;
	int tries = 0;
	struct object *special_obj;
	int obj_level = c->depth > 0 ? c->depth : 1;

	while (tries < 50) {
		grid = loc(rand_range(5, c->width - 6), rand_range(5, c->height - 6));
		if (square_isfloor(c, grid) && !square_monster(c, grid)) break;
		tries++;
	}
	if (tries >= 50) return false;

	special_obj = make_object(c, obj_level, true, one_in_(4), false, NULL, 0);
	if (!special_obj) return false;

	obj->type = FLOOR_OBJ_RETRIEVE_ITEM;
	obj->target_grid = grid;
	obj->target_grid2 = grid;
	obj->data.item.obj = special_obj;
	obj->data.item.oidx = special_obj->oidx;
	obj->data.item.picked_up = false;

	special_obj->origin = ORIGIN_SPECIAL;
	special_obj->origin_depth = c->depth;

	square_set_obj(c, grid, special_obj);
	list_object(c, special_obj);

	obj->description = string_make("Retrieve the special item");
	obj->hint = string_make("Something valuable was left somewhere on this level...");
	return true;
}

/**
 * Generate a REACH_AREA objective
 */
static bool floor_obj_gen_reach_area(struct chunk *c, struct player *p,
									 struct floor_objective *obj)
{
	struct loc grid;
	int tries = 0;

	while (tries < 50) {
		grid = loc(rand_range(3, c->width - 4), rand_range(3, c->height - 4));
		if (square_isfloor(c, grid) && !square_monster(c, grid)) {
			int dist = distance(grid, p->grid);
			if (dist > 15) break;
		}
		tries++;
	}
	if (tries >= 50) return false;

	obj->type = FLOOR_OBJ_REACH_AREA;
	obj->target_grid = grid;
	obj->target_grid2 = grid;
	obj->data.area.reached = false;
	obj->data.area.radius = 2;

	obj->description = string_make("Reach the unexplored region");
	obj->hint = string_make("A distant area beckons to be explored...");
	return true;
}

/**
 * Generate floor objectives for a level
 */
bool floor_obj_generate(struct chunk *c, struct player *p)
{
	int num_objs = 0;
	int i;

	if (!c || !p) return false;
	if (c->depth <= 0) return false;
	if (is_quest(p, c->depth)) return false;

	floor_obj_init(c);

	if (one_in_(3)) {
		num_objs = 1;
		if (c->depth >= 20 && one_in_(4)) {
			num_objs = FLOOR_OBJ_MAX_PER_LEVEL;
		}
	}

	for (i = 0; i < num_objs; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[c->floor_obj.count];
		bool generated = false;
		int attempts = 0;
		int obj_type;

		while (!generated && attempts < 10) {
			obj_type = FLOOR_OBJ_CLEAR_NEST + randint0(FLOOR_OBJ_MAX - 1);

			switch (obj_type) {
				case FLOOR_OBJ_CLEAR_NEST:
					generated = floor_obj_gen_clear_nest(c, p, obj);
					break;
				case FLOOR_OBJ_FIND_HIDDEN:
					generated = floor_obj_gen_find_hidden(c, p, obj);
					break;
				case FLOOR_OBJ_RETRIEVE_ITEM:
					generated = floor_obj_gen_retrieve_item(c, p, obj);
					break;
				case FLOOR_OBJ_REACH_AREA:
					generated = floor_obj_gen_reach_area(c, p, obj);
					break;
				default:
					break;
			}
			attempts++;
		}

		if (generated) {
			obj->state = FLOOR_OBJ_ACTIVE;
			obj->reward_type = floor_obj_random_reward(c->depth);
			obj->reward_value = floor_obj_calc_reward_value(c->depth, obj->reward_type);
			obj->reward_claimed = false;
			c->floor_obj.count++;

			if (obj->hint) {
				msgt(MSG_GENERIC, "%s", obj->hint);
			}
		}
	}

	return (c->floor_obj.count > 0);
}

/**
 * Check if a grid is within the bounds of a nest area
 */
static bool grid_in_nest_area(struct loc grid, struct floor_objective *obj)
{
	int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
	int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
	int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
	int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);

	return (grid.x >= x1 && grid.x <= x2 && grid.y >= y1 && grid.y <= y2);
}

/**
 * Check monster kills against floor objectives
 */
void floor_obj_check_monster_kill(struct player *p, const struct monster *m)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p || !m) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		if (obj->type == FLOOR_OBJ_CLEAR_NEST) {
			if (grid_in_nest_area(m->grid, obj)) {
				if (!obj->data.nest.race || m->race == obj->data.nest.race) {
					obj->data.nest.killed++;
					if (obj->data.nest.killed >= obj->data.nest.total) {
						int remaining = floor_obj_count_monsters_in_area(
							c, obj->target_grid, obj->target_grid2, obj->data.nest.race);
						if (remaining <= 0) {
							obj->state = FLOOR_OBJ_COMPLETED;
							msgt(MSG_GENERIC, "You have cleared the monster nest!");
						}
					}
				}
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Check item pickups against floor objectives
 */
void floor_obj_check_item_pickup(struct player *p, const struct object *obj_picked)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p || !obj_picked) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		if (obj->type == FLOOR_OBJ_RETRIEVE_ITEM) {
			if (obj->data.item.obj && (obj->data.item.obj == obj_picked ||
				obj->data.item.oidx == obj_picked->oidx)) {
				obj->data.item.picked_up = true;
				obj->state = FLOOR_OBJ_COMPLETED;
				msgt(MSG_GENERIC, "You have retrieved the special item!");
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Check player movement against floor objectives
 */
void floor_obj_check_player_move(struct player *p)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		if (obj->type == FLOOR_OBJ_REACH_AREA) {
			int dist = distance(p->grid, obj->target_grid);
			if (dist <= obj->data.area.radius) {
				obj->data.area.reached = true;
				obj->state = FLOOR_OBJ_COMPLETED;
				msgt(MSG_GENERIC, "You have reached the target area!");
			}
		}

		if (obj->type == FLOOR_OBJ_FIND_HIDDEN) {
			if (!obj->data.hidden.found && square_isview(c, p->grid)) {
				int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
				int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
				int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
				int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);

				if (p->grid.x >= x1 && p->grid.x <= x2 &&
					p->grid.y >= y1 && p->grid.y <= y2) {
					obj->data.hidden.found = true;
					obj->state = FLOOR_OBJ_COMPLETED;
					msgt(MSG_GENERIC, "You have discovered the hidden chamber!");
				}
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Check room discovery (called when player sees a new square)
 */
void floor_obj_check_room_discovery(struct player *p, struct loc grid)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		if (obj->type == FLOOR_OBJ_FIND_HIDDEN) {
			if (!obj->data.hidden.found) {
				int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
				int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
				int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
				int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);

				if (grid.x >= x1 && grid.x <= x2 &&
					grid.y >= y1 && grid.y <= y2) {
					if (square_isview(c, grid)) {
						obj->data.hidden.found = true;
						obj->state = FLOOR_OBJ_COMPLETED;
						msgt(MSG_GENERIC, "You have discovered the hidden chamber!");
					}
				}
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Claim all completed floor objective rewards
 */
void floor_obj_claim_rewards(struct player *p)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_COMPLETED) continue;
		if (obj->reward_claimed) continue;

		obj->reward_claimed = true;

		switch (obj->reward_type) {
			case FLOOR_REWARD_GOLD:
			{
				p->au += obj->reward_value;
				p->upkeep->redraw |= PR_GOLD;
				msgt(MSG_MONEY1, "You receive %d gold pieces worth of treasure!", obj->reward_value);
				break;
			}
			case FLOOR_REWARD_EXP:
			{
				player_exp_gain(p, obj->reward_value);
				msgt(MSG_LEVEL, "You gain %d experience points!", obj->reward_value);
				break;
			}
			case FLOOR_REWARD_OBJECT:
			{
				struct object *reward;
				struct loc grid = p->grid;
				int tries = 0;

				reward = make_object(c, obj->reward_value, true, true, false, NULL, 0);
				if (reward) {
					while (!square_canputitem(c, grid) && tries < 8) {
						scatter(c, &grid, p->grid, 1, false);
						tries++;
					}
					square_set_obj(c, grid, reward);
					list_object(c, reward);
					reward->origin = ORIGIN_SPECIAL;
					msgt(MSG_GENERIC, "A reward materializes at your feet!");
				}
				break;
			}
			case FLOOR_REWARD_HEAL:
			{
				int heal_amt = obj->reward_value;
				if (p->chp + heal_amt > p->mhp) {
					heal_amt = p->mhp - p->chp;
				}
				p->chp += heal_amt;
				p->upkeep->redraw |= PR_HP;
				msgt(MSG_RECOVER, "You feel better! (%d HP recovered)", heal_amt);

				if (p->csp < p->msp) {
					int mana_amt = obj->reward_value / 2;
					if (p->csp + mana_amt > p->msp) {
						mana_amt = p->msp - p->csp;
					}
					p->csp += mana_amt;
					p->upkeep->redraw |= PR_MANA;
				}
				break;
			}
			default:
				break;
		}
	}
}

/**
 * Get status text for a floor objective
 */
const char *floor_obj_get_status_text(struct chunk *c, int idx)
{
	static char buf[120];
	struct floor_objective *obj;

	if (!c || idx < 0 || idx >= c->floor_obj.count) return NULL;

	obj = &c->floor_obj.objs[idx];

	if (obj->state == FLOOR_OBJ_COMPLETED) {
		if (obj->description) {
			strnfmt(buf, sizeof(buf), "[Done] %s", obj->description);
		} else {
			strnfmt(buf, sizeof(buf), "[Done] Floor objective");
		}
		return buf;
	}

	if (obj->state != FLOOR_OBJ_ACTIVE) return NULL;

	switch (obj->type) {
		case FLOOR_OBJ_CLEAR_NEST:
			if (obj->description) {
				strnfmt(buf, sizeof(buf), "%s (%d/%d)",
					obj->description, obj->data.nest.killed, obj->data.nest.total);
			}
			break;
		case FLOOR_OBJ_FIND_HIDDEN:
			strnfmt(buf, sizeof(buf), "%s (searching...)",
				obj->description ? obj->description : "Find hidden room");
			break;
		case FLOOR_OBJ_RETRIEVE_ITEM:
			strnfmt(buf, sizeof(buf), "%s (on floor)",
				obj->description ? obj->description : "Retrieve item");
			break;
		case FLOOR_OBJ_REACH_AREA:
		{
			int dist = -1;
			if (player) {
				dist = distance(player->grid, obj->target_grid);
			}
			if (dist >= 0) {
				strnfmt(buf, sizeof(buf), "%s (%d grids away)",
					obj->description ? obj->description : "Reach area", dist);
			} else {
				strnfmt(buf, sizeof(buf), "%s",
					obj->description ? obj->description : "Reach area");
			}
			break;
		}
		default:
			if (obj->description) {
				my_strcpy(buf, obj->description, sizeof(buf));
			} else {
				strnfmt(buf, sizeof(buf), "Floor objective");
			}
			break;
	}

	return buf;
}

/**
 * Check if there are any active floor objectives
 */
bool floor_obj_has_active(struct chunk *c)
{
	int i;
	if (!c) return false;

	for (i = 0; i < c->floor_obj.count; i++) {
		if (c->floor_obj.objs[i].state == FLOOR_OBJ_ACTIVE) {
			return true;
		}
	}
	return false;
}

/**
 * Get grids to highlight on the map for active objectives
 * Returns the number of highlight grids filled
 */
int floor_obj_get_highlight_grid(struct chunk *c, struct loc *grid)
{
	int i;
	int count = 0;

	if (!c || !grid) return 0;

	for (i = 0; i < c->floor_obj.count && count < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		switch (obj->type) {
			case FLOOR_OBJ_REACH_AREA:
				grid[count++] = obj->target_grid;
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
				if (obj->data.item.obj && !obj->data.item.picked_up) {
					if (player && los(c, player->grid, obj->target_grid)) {
						grid[count++] = obj->target_grid;
					}
				}
				break;
			default:
				break;
		}
	}

	return count;
}

/**
 * Check if a single grid should be highlighted on the map
 */
bool floor_obj_is_highlight_grid(struct chunk *c, struct loc grid)
{
	int i;

	if (!c) return false;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;

		switch (obj->type) {
			case FLOOR_OBJ_REACH_AREA:
				if (loc_eq(obj->target_grid, grid)) return true;
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
				if (obj->data.item.obj && !obj->data.item.picked_up) {
					if (loc_eq(obj->target_grid, grid)) {
						if (player && los(c, player->grid, grid)) return true;
					}
				}
				break;
			case FLOOR_OBJ_CLEAR_NEST:
			{
				int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
				int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
				int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
				int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);
				if (grid.x >= x1 && grid.x <= x2 && grid.y >= y1 && grid.y <= y2) {
					return true;
				}
				break;
			}
			default:
				break;
		}
	}

	return false;
}

/**
 * Write floor objectives to savefile
 */
void wr_floor_obj(struct chunk *c)
{
	int i;

	if (!c) {
		wr_u16b(0);
		return;
	}

	wr_u16b(c->floor_obj.count);

	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		wr_byte(obj->type);
		wr_byte(obj->state);

		wr_string(obj->description ? obj->description : "");
		wr_string(obj->hint ? obj->hint : "");

		wr_byte(obj->target_grid.x);
		wr_byte(obj->target_grid.y);
		wr_byte(obj->target_grid2.x);
		wr_byte(obj->target_grid2.y);

		switch (obj->type) {
			case FLOOR_OBJ_CLEAR_NEST:
				wr_s16b(obj->data.nest.total);
				wr_s16b(obj->data.nest.killed);
				wr_string(obj->data.nest.race ? obj->data.nest.race->name : "");
				break;
			case FLOOR_OBJ_FIND_HIDDEN:
				wr_byte(obj->data.hidden.found ? 1 : 0);
				wr_s16b(obj->data.hidden.room_id);
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
				wr_u32b(obj->data.item.oidx);
				wr_byte(obj->data.item.picked_up ? 1 : 0);
				break;
			case FLOOR_OBJ_REACH_AREA:
				wr_byte(obj->data.area.reached ? 1 : 0);
				wr_byte(obj->data.area.radius);
				break;
			default:
				break;
		}

		wr_byte(obj->reward_type);
		wr_s32b(obj->reward_value);
		wr_byte(obj->reward_claimed ? 1 : 0);
	}
}

/**
 * Read floor objectives from savefile
 */
void rd_floor_obj(struct chunk *c)
{
	int i;
	uint16_t count;
	uint8_t tmp8u;
	int16_t tmp16s;
	uint32_t tmp32u;
	char buf[120];

	if (!c) {
		rd_u16b(&count);
		return;
	}

	floor_obj_init(c);

	rd_u16b(&count);
	if (count > FLOOR_OBJ_MAX_PER_LEVEL) {
		note(format("Too many (%u) floor objectives!", count));
		count = FLOOR_OBJ_MAX_PER_LEVEL;
	}
	c->floor_obj.count = count;

	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		rd_byte(&tmp8u);
		obj->type = (floor_obj_type)tmp8u;
		rd_byte(&tmp8u);
		obj->state = (floor_obj_state)tmp8u;

		rd_string(buf, sizeof(buf));
		if (buf[0]) {
			obj->description = string_make(buf);
		}
		rd_string(buf, sizeof(buf));
		if (buf[0]) {
			obj->hint = string_make(buf);
		}

		rd_byte(&tmp8u);
		obj->target_grid.x = tmp8u;
		rd_byte(&tmp8u);
		obj->target_grid.y = tmp8u;
		rd_byte(&tmp8u);
		obj->target_grid2.x = tmp8u;
		rd_byte(&tmp8u);
		obj->target_grid2.y = tmp8u;

		memset(&obj->data, 0, sizeof(obj->data));

		switch (obj->type) {
			case FLOOR_OBJ_CLEAR_NEST:
				rd_s16b(&tmp16s);
				obj->data.nest.total = tmp16s;
				rd_s16b(&tmp16s);
				obj->data.nest.killed = tmp16s;
				rd_string(buf, sizeof(buf));
				if (buf[0]) {
					obj->data.nest.race = lookup_monster(buf);
				}
				break;
			case FLOOR_OBJ_FIND_HIDDEN:
				rd_byte(&tmp8u);
				obj->data.hidden.found = tmp8u ? true : false;
				rd_s16b(&tmp16s);
				obj->data.hidden.room_id = tmp16s;
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
				rd_u32b(&tmp32u);
				obj->data.item.oidx = tmp32u;
				obj->data.item.obj = NULL;
				rd_byte(&tmp8u);
				obj->data.item.picked_up = tmp8u ? true : false;
				break;
			case FLOOR_OBJ_REACH_AREA:
				rd_byte(&tmp8u);
				obj->data.area.reached = tmp8u ? true : false;
				rd_byte(&tmp8u);
				obj->data.area.radius = tmp8u;
				break;
			default:
				break;
		}

		rd_byte(&tmp8u);
		obj->reward_type = (floor_reward_type)tmp8u;
		rd_s32b((int32_t *)&obj->reward_value);
		rd_byte(&tmp8u);
		obj->reward_claimed = tmp8u ? true : false;
	}
}
