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
 * Find an objective in a chunk by its objective_id (1-based).
 * Returns NULL if not found.
 */
static struct floor_objective *floor_obj_find_by_id(struct chunk *c, uint16_t obj_id)
{
	int i;
	if (!c || obj_id == 0) return NULL;

	for (i = 0; i < c->floor_obj.count; i++) {
		if (c->floor_obj.objs[i].objective_id == obj_id) {
			return &c->floor_obj.objs[i];
		}
	}
	return NULL;
}

/**
 * Initialize floor objectives for a chunk
 */
void floor_obj_init(struct chunk *c)
{
	int i;

	if (!c) return;

	c->floor_obj.count = 0;
	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		c->floor_obj.objs[i].objective_id = 0;
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
 * Generate a CLEAR_NEST objective.
 * Each spawned monster has its floor_obj_id set to the objective's id.
 * Only those monsters count toward the kill count.
 */
static bool floor_obj_gen_clear_nest(struct chunk *c, struct player *p,
									 struct floor_objective *obj)
{
	struct loc center;
	int nest_half = 5 + randint0(4);
	int tries = 0;
	int x, y;
	int mon_count;
	struct monster_race *race;
	struct monster_group_info info = { 0, 0 };

	while (tries < 50) {
		center = loc(rand_range(nest_half + 2, c->width - nest_half - 3),
					 rand_range(nest_half + 2, c->height - nest_half - 3));

		if (square_isfloor(c, center)) break;
		tries++;
	}
	if (tries >= 50) return false;

	race = get_mon_num(c->depth, c->depth);
	if (!race) return false;

	obj->type = FLOOR_OBJ_CLEAR_NEST;
	obj->target_grid = loc(center.x - nest_half, center.y - nest_half);
	obj->target_grid2 = loc(center.x + nest_half, center.y + nest_half);
	obj->data.nest.total_kills = 0;
	obj->data.nest.current_kills = 0;

	for (x = obj->target_grid.x; x <= obj->target_grid2.x; x++) {
		for (y = obj->target_grid.y; y <= obj->target_grid2.y; y++) {
			struct loc mg = loc(x, y);
			if (square_in_bounds(c, mg) && (square_isfloor(c, mg) || square_isempty(c, mg))) {
				if (one_in_(3)) {
					square_set_feat(c, mg, FEAT_FLOOR);
				}
			}
		}
	}

	mon_count = 4 + randint0(4);
	tries = 0;

	while (tries < mon_count * 4 && obj->data.nest.total_kills < mon_count) {
		struct loc mg;
		mg = loc(rand_range(obj->target_grid.x, obj->target_grid2.x),
				 rand_range(obj->target_grid.y, obj->target_grid2.y));
		if (square_isempty(c, mg)) {
			if (place_new_monster(c, mg, race, true, true, info, ORIGIN_DROP)) {
				struct monster *new_mon = square_monster(c, mg);
				if (new_mon) {
					new_mon->floor_obj_id = (int16_t)obj->objective_id;
					obj->data.nest.total_kills++;
				}
			}
		}
		tries++;
	}

	if (obj->data.nest.total_kills == 0) return false;

	obj->description = string_make("清理怪物巢穴");
	obj->hint = string_make("你隐约感觉到附近有怪物聚集...");
	return true;
}

/**
 * Generate a FIND_HIDDEN objective.
 */
static bool floor_obj_gen_find_hidden(struct chunk *c, struct player *p,
									  struct floor_objective *obj)
{
	struct loc center;
	int tries = 0;
	int room_size = 4 + randint0(3);

	while (tries < 30) {
		center = loc(rand_range(room_size + 3, c->width - room_size - 4),
					 rand_range(room_size + 3, c->height - room_size - 4));

		bool all_rock = true;
		int x, y;
		for (y = center.y - room_size - 1; y <= center.y + room_size + 1; y++) {
			for (x = center.x - room_size - 1; x <= center.x + room_size + 1; x++) {
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
	obj->target_grid = loc(center.x - room_size, center.y - room_size);
	obj->target_grid2 = loc(center.x + room_size, center.y + room_size);
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
				case 0: door_grid = loc(center.x, obj->target_grid.y); break;
				case 1: door_grid = loc(center.x, obj->target_grid2.y); break;
				case 2: door_grid = loc(obj->target_grid.x, center.y); break;
				default: door_grid = loc(obj->target_grid2.x, center.y); break;
			}
			square_set_feat(c, door_grid, FEAT_SECRET);
			sqinfo_on(square(c, door_grid)->info, SQUARE_ROOM);
		}

		{
			int obj_level = c->depth > 0 ? c->depth : 1;
			place_object(c, center, obj_level, one_in_(3), one_in_(6), ORIGIN_SPECIAL, 0);
			if (one_in_(3)) {
				place_gold(c, center, obj_level, ORIGIN_SPECIAL);
			}
		}
	}

	obj->description = string_make("寻找隐藏房间");
	obj->hint = string_make("这层似乎藏着一间隐秘的房间...");
	return true;
}

/**
 * Generate a RETRIEVE_ITEM objective.
 * The spawned object has its floor_obj_id set to the objective's id.
 * Only picking up that specific object (by id) counts as completion.
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
	obj->data.item.picked_up = false;

	special_obj->origin = ORIGIN_SPECIAL;
	special_obj->origin_depth = c->depth;
	special_obj->floor_obj_id = (int16_t)obj->objective_id;

	square_set_obj(c, grid, special_obj);
	list_object(c, special_obj);

	obj->description = string_make("回收特殊物品");
	obj->hint = string_make("有一件珍贵的物品遗落在这层某处...");
	return true;
}

/**
 * Generate a REACH_AREA objective.
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

	obj->description = string_make("抵达指定区域");
	obj->hint = string_make("远处有一处值得探索的区域...");
	return true;
}

/**
 * Generate floor objectives for a level.
 * Assigns objective_id = count + 1 (1-based) so it matches the floor_obj_id
 * written into spawned monsters and objects.
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

		obj->objective_id = (uint16_t)(c->floor_obj.count + 1);

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
		} else {
			obj->objective_id = 0;
		}
	}

	return (c->floor_obj.count > 0);
}

/**
 * Check monster kills against floor objectives.
 * ONLY counts monsters whose floor_obj_id matches an active objective.
 * This prevents the player from killing normal monsters of the same race
 * to satisfy the objective.
 */
void floor_obj_check_monster_kill(struct player *p, const struct monster *m)
{
	struct chunk *c = cave;
	struct floor_objective *obj;

	if (!c || !p || !m) return;
	if (m->floor_obj_id <= 0) return;

	obj = floor_obj_find_by_id(c, (uint16_t)m->floor_obj_id);
	if (!obj) return;
	if (obj->state != FLOOR_OBJ_ACTIVE) return;
	if (obj->type != FLOOR_OBJ_CLEAR_NEST) return;

	obj->data.nest.current_kills++;

	if (obj->data.nest.current_kills >= obj->data.nest.total_kills) {
		obj->state = FLOOR_OBJ_COMPLETED;
		msgt(MSG_GENERIC, "你清理了怪物巢穴！");
	}

	floor_obj_claim_rewards(p);
}

/**
 * Check item pickups against floor objectives.
 * ONLY counts items whose floor_obj_id matches an active objective.
 * This prevents the player from picking up normal items to complete objectives.
 */
void floor_obj_check_item_pickup(struct player *p, const struct object *obj_picked)
{
	struct chunk *c = cave;
	struct floor_objective *obj;

	if (!c || !p || !obj_picked) return;
	if (obj_picked->floor_obj_id <= 0) return;

	obj = floor_obj_find_by_id(c, (uint16_t)obj_picked->floor_obj_id);
	if (!obj) return;
	if (obj->state != FLOOR_OBJ_ACTIVE) return;
	if (obj->type != FLOOR_OBJ_RETRIEVE_ITEM) return;

	obj->data.item.picked_up = true;
	obj->state = FLOOR_OBJ_COMPLETED;
	msgt(MSG_GENERIC, "你回收了目标物品！");

	floor_obj_claim_rewards(p);
}

/**
 * Check player movement against area-reach and hidden-room objectives.
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
				msgt(MSG_GENERIC, "你抵达了目标区域！");
			}
		}

		if (obj->type == FLOOR_OBJ_FIND_HIDDEN) {
			if (!obj->data.hidden.found) {
				int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
				int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
				int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
				int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);

				if (p->grid.x >= x1 && p->grid.x <= x2 &&
					p->grid.y >= y1 && p->grid.y <= y2) {
					obj->data.hidden.found = true;
					obj->state = FLOOR_OBJ_COMPLETED;
					msgt(MSG_GENERIC, "你发现了隐藏房间！");
				}
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Check room discovery (called when player sees a new square).
 */
void floor_obj_check_room_discovery(struct player *p, struct loc grid)
{
	int i;
	struct chunk *c = cave;

	if (!c || !p) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		if (obj->state != FLOOR_OBJ_ACTIVE) continue;
		if (obj->type != FLOOR_OBJ_FIND_HIDDEN) continue;
		if (obj->data.hidden.found) continue;

		{
			int x1 = MIN(obj->target_grid.x, obj->target_grid2.x);
			int x2 = MAX(obj->target_grid.x, obj->target_grid2.x);
			int y1 = MIN(obj->target_grid.y, obj->target_grid2.y);
			int y2 = MAX(obj->target_grid.y, obj->target_grid2.y);

			if (grid.x >= x1 && grid.x <= x2 &&
				grid.y >= y1 && grid.y <= y2) {
				if (square_isview(c, grid)) {
					obj->data.hidden.found = true;
					obj->state = FLOOR_OBJ_COMPLETED;
					msgt(MSG_GENERIC, "你发现了隐藏房间！");
				}
			}
		}
	}

	floor_obj_claim_rewards(p);
}

/**
 * Claim all completed floor objective rewards.
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
				msgt(MSG_MONEY1, "你获得了 %d 枚金币作为奖励！", obj->reward_value);
				break;
			}
			case FLOOR_REWARD_EXP:
			{
				player_exp_gain(p, obj->reward_value);
				msgt(MSG_LEVEL, "你获得了 %d 点经验！", obj->reward_value);
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
					msgt(MSG_GENERIC, "一件奖励出现在你脚边！");
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
				msgt(MSG_RECOVER, "你感觉好多了！(恢复了 %d 点生命)", heal_amt);

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
 * Get status text for a floor objective.
 * VAGUE: never shows coordinates, grid distances, or monster race names.
 */
const char *floor_obj_get_status_text(struct chunk *c, int idx)
{
	static char buf[120];
	struct floor_objective *obj;

	if (!c || idx < 0 || idx >= c->floor_obj.count) return NULL;

	obj = &c->floor_obj.objs[idx];

	if (obj->state == FLOOR_OBJ_COMPLETED) {
		if (obj->reward_claimed) {
			strnfmt(buf, sizeof(buf), "[已完成] %s",
				obj->description ? obj->description : "楼层目标");
		} else {
			strnfmt(buf, sizeof(buf), "[奖励已就绪] %s",
				obj->description ? obj->description : "楼层目标");
		}
		return buf;
	}

	if (obj->state == FLOOR_OBJ_FAILED) {
		strnfmt(buf, sizeof(buf), "[已失效] %s",
			obj->description ? obj->description : "楼层目标");
		return buf;
	}

	if (obj->state != FLOOR_OBJ_ACTIVE) return NULL;

	switch (obj->type) {
		case FLOOR_OBJ_CLEAR_NEST:
			strnfmt(buf, sizeof(buf), "%s: 已击杀 %d/%d",
				obj->description ? obj->description : "清理巢穴",
				obj->data.nest.current_kills,
				obj->data.nest.total_kills);
			break;
		case FLOOR_OBJ_FIND_HIDDEN:
			strnfmt(buf, sizeof(buf), "%s: 搜索中...",
				obj->description ? obj->description : "寻找隐藏房间");
			break;
		case FLOOR_OBJ_RETRIEVE_ITEM:
			if (obj->data.item.picked_up) {
				strnfmt(buf, sizeof(buf), "%s: 已回收",
					obj->description ? obj->description : "回收物品");
			} else {
				strnfmt(buf, sizeof(buf), "%s: 遗落在此层",
					obj->description ? obj->description : "回收物品");
			}
			break;
		case FLOOR_OBJ_REACH_AREA:
			strnfmt(buf, sizeof(buf), "%s: 探索中...",
				obj->description ? obj->description : "抵达区域");
			break;
		default:
			if (obj->description) {
				my_strcpy(buf, obj->description, sizeof(buf));
			} else {
				strnfmt(buf, sizeof(buf), "楼层目标");
			}
			break;
	}

	return buf;
}

/**
 * Check if there are any active floor objectives, or completed ones
 * with unclaimed rewards.
 */
bool floor_obj_has_active(struct chunk *c)
{
	int i;
	if (!c) return false;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];
		if (obj->state == FLOOR_OBJ_ACTIVE) {
			return true;
		}
		if (obj->state == FLOOR_OBJ_COMPLETED && !obj->reward_claimed) {
			return true;
		}
	}
	return false;
}

/**
 * Get grids to highlight on the map.
 * INTENTIONALLY EMPTY: no spoilers via map highlights.
 */
int floor_obj_get_highlight_grid(struct chunk *c, struct loc *grid)
{
	return 0;
}

/**
 * Check if a grid should be highlighted.
 * INTENTIONALLY RETURNS FALSE: no spoilers via map highlights.
 */
bool floor_obj_is_highlight_grid(struct chunk *c, struct loc grid)
{
	return false;
}

/**
 * Write floor objectives to savefile.
 * Format per objective:
 *   u16 objective_id
 *   u8  type
 *   u8  state
 *   str description
 *   str hint
 *   u8  target_grid.x (internal, not shown to player)
 *   u8  target_grid.y
 *   u8  target_grid2.x
 *   u8  target_grid2.y
 *   [type-specific data with progress]
 *   u8  reward_type
 *   s32 reward_value
 *   u8  reward_claimed
 *
 * The actual monster/object floor_obj_id bindings are saved alongside
 * each monster and object in the monsters/objects save blocks.
 */
void wr_floor_obj(struct chunk *c)
{
	int i;
	uint16_t map_count;

	if (!c) {
		wr_u16b(0);
		return;
	}

	wr_u16b(c->floor_obj.count);

	for (i = 0; i < FLOOR_OBJ_MAX_PER_LEVEL; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		wr_u16b(obj->objective_id);
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
				wr_s16b(obj->data.nest.total_kills);
				wr_s16b(obj->data.nest.current_kills);
				break;
			case FLOOR_OBJ_FIND_HIDDEN:
				wr_byte(obj->data.hidden.found ? 1 : 0);
				wr_s16b(obj->data.hidden.room_id);
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
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

	/* Save monster -> floor_obj_id mapping (only non-zero) */
	map_count = 0;
	for (i = 0; i < c->mon_max; i++) {
		if (c->monsters[i].floor_obj_id > 0) {
			map_count++;
		}
	}
	wr_u16b(map_count);
	for (i = 0; i < c->mon_max; i++) {
		if (c->monsters[i].floor_obj_id > 0) {
			wr_u16b(c->monsters[i].midx);
			wr_s16b(c->monsters[i].floor_obj_id);
		}
	}

	/* Save object -> floor_obj_id mapping (only non-zero) */
	map_count = 0;
	for (i = 0; i < c->obj_max; i++) {
		if (c->objects[i] && c->objects[i]->floor_obj_id > 0) {
			map_count++;
		}
	}
	wr_u16b(map_count);
	for (i = 0; i < c->obj_max; i++) {
		if (c->objects[i] && c->objects[i]->floor_obj_id > 0) {
			wr_u16b(c->objects[i]->oidx);
			wr_s16b(c->objects[i]->floor_obj_id);
		}
	}
}

/**
 * Read floor objectives from savefile.
 * The monster/object floor_obj_id bindings are restored from the
 * monsters/objects save blocks.
 */
void rd_floor_obj(struct chunk *c)
{
	int i;
	uint16_t count;
	uint8_t tmp8u;
	int16_t tmp16s;
	uint16_t map_count;

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
		char buf[120];
		uint16_t obj_id;

		rd_u16b(&obj_id);
		obj->objective_id = obj_id;

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
				obj->data.nest.total_kills = tmp16s;
				rd_s16b(&tmp16s);
				obj->data.nest.current_kills = tmp16s;
				break;
			case FLOOR_OBJ_FIND_HIDDEN:
				rd_byte(&tmp8u);
				obj->data.hidden.found = tmp8u ? true : false;
				rd_s16b(&tmp16s);
				obj->data.hidden.room_id = tmp16s;
				break;
			case FLOOR_OBJ_RETRIEVE_ITEM:
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

	/* Restore monster -> floor_obj_id mapping */
	rd_u16b(&map_count);
	for (i = 0; i < map_count; i++) {
		uint16_t midx;
		int16_t obj_id;
		rd_u16b(&midx);
		rd_s16b(&obj_id);
		if (midx < c->mon_max) {
			c->monsters[midx].floor_obj_id = obj_id;
		}
	}

	/* Restore object -> floor_obj_id mapping */
	rd_u16b(&map_count);
	for (i = 0; i < map_count; i++) {
		uint16_t oidx;
		int16_t obj_id;
		rd_u16b(&oidx);
		rd_s16b(&obj_id);
		if (oidx < c->obj_max && c->objects[oidx]) {
			c->objects[oidx]->floor_obj_id = obj_id;
		}
	}
}

/**
 * Validate active objectives' bound entities still exist after load.
 * If target entities are gone with no progress, mark the objective FAILED.
 * If some target entities vanished but progress was made, adjust totals
 * down so the objective can still be completed with what remains.
 */
void floor_obj_validate(struct chunk *c, struct player *p)
{
	int i;
	if (!c || c->floor_obj.count <= 0) return;

	for (i = 0; i < c->floor_obj.count; i++) {
		struct floor_objective *obj = &c->floor_obj.objs[i];

		/* Only validate active objectives */
		if (obj->state != FLOOR_OBJ_ACTIVE) continue;
		if (obj->objective_id == 0) continue;

		switch (obj->type) {
			case FLOOR_OBJ_CLEAR_NEST: {
				int j, remaining = 0;
				int expected;

				for (j = 1; j < c->mon_max; j++) {
					if (c->monsters[j].midx == 0) continue;
					if (c->monsters[j].floor_obj_id == (int16_t)obj->objective_id) {
						remaining++;
					}
				}

				expected = obj->data.nest.current_kills + remaining;

				/* No monsters left and no kills ever made: objective lost */
				if (expected == 0) {
					obj->state = FLOOR_OBJ_FAILED;
					obj->data.nest.total_kills = 0;
					obj->data.nest.current_kills = 0;
				} else if (expected != obj->data.nest.total_kills) {
					/* Adjust total to match reality (some monsters vanished) */
					obj->data.nest.total_kills = expected;
					if (obj->data.nest.current_kills >= obj->data.nest.total_kills) {
						obj->state = FLOOR_OBJ_COMPLETED;
					}
				}
				break;
			}

			case FLOOR_OBJ_RETRIEVE_ITEM: {
				bool found = false;
				int j;

				if (obj->data.item.picked_up) break;

				/* Search chunk objects (floor) */
				for (j = 1; j < c->obj_max; j++) {
					if (!c->objects[j]) continue;
					if (c->objects[j]->floor_obj_id == (int16_t)obj->objective_id) {
						found = true;
						break;
					}
				}

				/* Search player gear (pack + equipment + quiver) */
				if (!found && p) {
					struct object *gear_obj;
					for (gear_obj = p->gear; gear_obj; gear_obj = gear_obj->next) {
						if (gear_obj->floor_obj_id == (int16_t)obj->objective_id) {
							found = true;
							break;
						}
					}
				}

				if (!found) {
					obj->state = FLOOR_OBJ_FAILED;
				}
				break;
			}

			case FLOOR_OBJ_FIND_HIDDEN:
			case FLOOR_OBJ_REACH_AREA:
			default:
				/* Location-based objectives have no entities to validate */
				break;
		}
	}
}
