/**
 * \file mon-group.c
 * \brief Monster group behaviours
 *
 * Copyright (c) 2018 Nick McConnell
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
#include "game-world.h"
#include "init.h"
#include "mon-group.h"
#include "mon-make.h"
#include "mon-util.h"
#include "monster.h"
#include "player-util.h"

/**
 * Allocate a new monster group
 */
struct monster_group *monster_group_new(void)
{
	struct monster_group *group = mem_zalloc(sizeof(struct monster_group));
	return group;
}

/**
 * Free a monster group
 */
void monster_group_free(struct chunk *c, struct monster_group *group)
{
	/* Free the member list */
	while (group->member_list) {
		struct mon_group_list_entry *next = group->member_list->next;
		mem_free(group->member_list);
		group->member_list = next;
	}

	mem_free(group);
}

/**
 * Break a monster group into race-based pieces
 */
static void monster_group_split(struct chunk *c, struct monster_group *group,
								struct monster *leader)
{
	struct mon_group_list_entry *entry;

	/* Keep a list of groups made for easy checking */
	int *temp = mem_zalloc(z_info->level_monster_max * sizeof(int));
	int current = 0;

	/* Go through the monsters in the group */
	for (entry = group->member_list; entry; entry = entry->next) {
		int i;
		struct monster *mon = &c->monsters[entry->midx];

		/* Check all groups to see if they contain a monster of this race */
		for (i = 0; i < current; i++) {
			struct monster_group *new_group = c->monster_groups[temp[i]];

			/* If it's the right group, add the monster and stop checking */
			if (c->monsters[new_group->member_list->midx].race == mon->race) {
				mon->group_info[PRIMARY_GROUP].index = temp[i];
				mon->group_info[PRIMARY_GROUP].role = MON_GROUP_MEMBER;
				monster_add_to_group(c, mon, new_group);
				break;
			}
		}

		/* If the monster's still in the old group, make a new one */
		if (mon->group_info[PRIMARY_GROUP].index == group->index) {
			monster_group_start(c, mon, 0);

			/* Store the new index */
			temp[current++] = mon->group_info[PRIMARY_GROUP].index;
		}
	}
	mem_free(temp);
}


/**
 * Handle the leader of a group being removed
 */
static void monster_group_remove_leader(struct chunk *c, struct monster *leader,
										struct monster_group *group)
{
	struct mon_group_list_entry *list_entry = group->member_list;
	int poss_leader = 0;

	/* Look for another leader */
	while (list_entry) {
		struct monster *mon = cave_monster(c, list_entry->midx);

		if (!mon) {
			list_entry = list_entry->next;
			continue;
		}

		/* Monsters of the same race can take over as leader */
		if ((leader->race == mon->race) && !poss_leader
			&& (mon->group_info[PRIMARY_GROUP].role != MON_GROUP_SUMMON)) {
			poss_leader = mon->midx;
		}

		/* Uniques always take over */
		if (monster_is_unique(mon)) {
			poss_leader = mon->midx;
		}
		list_entry = list_entry->next;
	}

	/* If no new leader, group fractures and old group is removed */
	if (!poss_leader) {
		monster_group_split(c, group, leader);
		c->monster_groups[group->index] = NULL;
		monster_group_free(c, group);
	} else {
		/* If there is a successor, appoint them and finalise changes */
		group->leader = poss_leader;
		list_entry = group->member_list;
		assert(list_entry);
		while (list_entry) {
			struct monster *mon = cave_monster(c, list_entry->midx);

			/* Record the leader */
			if (mon->midx == poss_leader) {
				mon->group_info[PRIMARY_GROUP].role = MON_GROUP_LEADER;
				break;
			}
			list_entry = list_entry->next;
		}
	}
	monster_groups_verify(c);
}

/**
 * Remove a monster from a monster group, deleting the group if it's empty.
 * Deal with removal of the leader.
 */
void monster_remove_from_groups(struct chunk *c, struct monster *mon)
{
	int i;
	struct monster_group *group;
	struct mon_group_list_entry *list_entry;

	for (i = 0; i < GROUP_MAX; i++) {
		group =	c->monster_groups[mon->group_info[i].index];

		/* Most monsters won't have a second group */
		if (!group) return;
		list_entry = group->member_list;

		/* Check if the first entry is the one we want */
		if (list_entry->midx == mon->midx) {
			if (!list_entry->next) {
				/* If it's the only monster, remove the group */
				monster_group_free(c, group);
				c->monster_groups[mon->group_info[i].index] = NULL;
				continue;
			} else {
				/* Otherwise remove the first entry */
				group->member_list = list_entry->next;
				mem_free(list_entry);
				if (group->leader == mon->midx) {
					monster_group_remove_leader(c, mon, group);
				}
				continue;
			}
		}

		/* Check - necessary? */
		if (list_entry->next == NULL) {
			quit_fmt("Bad group: index=%d, monster=%d",
					 mon->group_info[i].index, mon->midx);
		}

		/* We have to look further down the member list */
		while (list_entry->next) {
			if (list_entry->next->midx == mon->midx) {
				struct mon_group_list_entry *remove = list_entry->next;
				list_entry->next = list_entry->next->next;
				mem_free(remove);
				if (group->leader == mon->midx) {
					monster_group_remove_leader(c, mon, group);
				}
				break;
			}
			list_entry = list_entry->next;
		}
	}
	monster_groups_verify(c);
}

/**
 * Get the next available monster group index
 */
int monster_group_index_new(struct chunk *c)
{
	int index;

	for (index = 1; index < z_info->level_monster_max; index++) {
		if (!(c->monster_groups[index])) return index;
	}

	/* Fail, very unlikely */
	return 0;
}

/**
 * Add a monster to an existing monster group
 */
void monster_add_to_group(struct chunk *c, struct monster *mon,
						  struct monster_group *group)
{
	struct mon_group_list_entry *list_entry;

	/* Confirm we're adding to the right group */
	assert(mon->group_info[PRIMARY_GROUP].index == group->index);

	/* Make a new list entry and add it to the start of the list */
	list_entry = mem_zalloc(sizeof(struct mon_group_list_entry));
	list_entry->midx = mon->midx;
	list_entry->next = group->member_list;
	group->member_list = list_entry;
}


/**
 * Make a monster group for a single monster
 */
void monster_group_start(struct chunk *c, struct monster *mon, int which)
{
	/* Get a group and a group index */
	struct monster_group *group = monster_group_new();
	int index = monster_group_index_new(c);
	assert(index);

	/* Put the group in the group list */
	c->monster_groups[index] = group;

	/* Fill out the group */
	group->index = index;
	group->leader = mon->midx;
	group->member_list = mem_zalloc(sizeof(struct mon_group_list_entry));
	group->member_list->midx = mon->midx;

	/* Write the index to the monster's group info, make it leader */
	mon->group_info[which].index = index;
	mon->group_info[which].role = MON_GROUP_LEADER;
}

/**
 * Assign a monster to a monster group
 */
void monster_group_assign(struct chunk *c, struct monster *mon,
						  struct monster_group_info *info, bool loading)
{
	int index = info[PRIMARY_GROUP].index;
	struct monster_group *group = monster_group_by_index(c, index);

	if (!loading) {
		/* For newly created monsters, use the group start and add functions */
		if (group) {
			monster_add_to_group(c, mon, group);
		} else {
			monster_group_start(c, mon, 0);
		}
	} else {
		/* For loading from a savefile, build by hand */
		int i;

		for (i = 0; i < GROUP_MAX; i++) {
			struct mon_group_list_entry *entry = mem_zalloc(sizeof(*entry));

			/* Check the index */
			index = info[i].index;
			if (!index) {
				if (i == PRIMARY_GROUP) {
					/* Everything should have a primary group */
					quit_fmt("Monster %d has no group", mon->midx);
				} else {
					/* Plenty of things have no summon group */
					mem_free(entry);
					return;
				}
			}

			/* Fill out the group, creating if necessary */
			group = monster_group_by_index(c, index);
			if (!group) {
				group = monster_group_new();
				group->index = index;
				c->monster_groups[index] = group;
			}
			if (info[i].role == MON_GROUP_LEADER) {
				group->leader = mon->midx;
			}

			/* Add this monster */
			entry->midx = mon->midx;
			entry->next = group->member_list;
			group->member_list = entry;
		}
	}
}

/**
 * Get the index of a monster group
 */
int monster_group_index(struct monster_group *group)
{
	return group->index;
}

/**
 * Get a monster group from its index
 */
struct monster_group *monster_group_by_index(struct chunk *c, int index)
{
	return c->monster_groups[index];
}

/**
 * Change the group record of the index of a monster (for one or two groups)
 */
bool monster_group_change_index(struct chunk *c, int new, int old)
{
	int index0 = cave_monster(c, old)->group_info[PRIMARY_GROUP].index;
	int index1 = cave_monster(c, old)->group_info[SUMMON_GROUP].index;
	struct monster_group *group0 = monster_group_by_index(c, index0);
	struct monster_group *group1 = monster_group_by_index(c, index1);
	struct mon_group_list_entry *entry = group0->member_list;

	if (group0->leader == old) {
		group0->leader = new;
	}
	while (entry) {
		if (entry->midx == old) {
			entry->midx = new;
			if (!group1) {
				return true;
			}
		}
		entry = entry->next;
	}

	if (group1) {
		if (group1->leader == old) {
			group1->leader = new;
		}
		entry = group1->member_list;
		while (entry) {
			if (entry->midx == old) {
				entry->midx = new;
				return true;
			}
			entry = entry->next;
		}
	}

	return false;
}

/**
 * Get the group of summons of a monster
 */
struct monster_group *summon_group(struct chunk *c, int midx)
{
	struct monster *mon = cave_monster(c, midx);
	int index;

	if (!mon) return NULL;

	/* If the monster is leader of its primary group, return that group */
	if (mon->group_info[PRIMARY_GROUP].role == MON_GROUP_LEADER) {
		index = mon->group_info[PRIMARY_GROUP].index;
	} else {
		/* Get the (distinct) summon group */
		index = mon->group_info[SUMMON_GROUP].index;

		/* Make a group if there isn't one already */
		if (!index) {
			monster_group_start(c, mon, 1);
			index = mon->group_info[SUMMON_GROUP].index;
		}
	}

	return monster_group_by_index(c, index);
}


/**
 * Monster who is aware of the player tries to let its group know
 */
void monster_group_rouse(struct chunk *c, struct monster *mon)
{
	int index = mon->group_info[PRIMARY_GROUP].index;
	struct monster_group *group = c->monster_groups[index];
	struct mon_group_list_entry *entry = group->member_list;

	/* Not aware means don't rouse */
	if (!mflag_has(mon->mflag, MFLAG_AWARE)) return;

	while (entry) {
		struct monster *friend = &c->monsters[entry->midx];
		struct loc fgrid = friend->grid;
		if (friend->m_timed[MON_TMD_SLEEP] && monster_can_see(c, mon, fgrid)) {
			int dist = distance(mon->grid, fgrid);

			/* Closer means more likely to be roused */
			if (one_in_(dist * 20)) {
				monster_wake(friend, true, 50);
			}
		}
		entry = entry->next;
	}
}

/**
 * Get the size of a monster's primary group
 */
int monster_primary_group_size(struct chunk *c, const struct monster *mon)
{
	int count = 0;
	int index = mon->group_info[PRIMARY_GROUP].index;
	struct monster_group *group = c->monster_groups[index];
	struct mon_group_list_entry *entry = group->member_list;

	while (entry) {
		count++;
		entry = entry->next;
	}
	return count;
}

/**
 * Find a group monster which is tracking
 */
struct monster *group_monster_tracking(struct chunk *c,
									   const struct monster *mon)
{
	int index = mon->group_info[PRIMARY_GROUP].index;
	struct monster_group *group = c->monster_groups[index];
	struct mon_group_list_entry *entry = group->member_list;

	while (entry) {
		struct monster *tracker = cave_monster(c, entry->midx);
		if (tracker != mon &&
				mflag_has(tracker->mflag, MFLAG_TRACKING) &&
				mflag_has(tracker->mflag, MFLAG_ACTIVE)) {
			return tracker;
		}
		entry = entry->next;
	}

	return NULL;
}

/**
 * Get the index of the leader of a monster group
 */
int monster_group_leader_idx(struct monster_group *group)
{
	return group->leader;
}

/**
 * Get the leader of a monster group
 */
struct monster *monster_group_leader(struct chunk *c, struct monster *mon)
{
	int index = mon->group_info[PRIMARY_GROUP].index;
	struct monster_group *group = c->monster_groups[index];
	return cave_monster(c, group->leader);
}

/**
 * Verify the integrity of all the monster groups
 */
void monster_groups_verify(struct chunk *c)
{
	int i;

	for (i = 0; i < z_info->level_monster_max; i++) {
		if (c->monster_groups[i]) {
			struct monster_group *group = c->monster_groups[i];
			struct mon_group_list_entry *entry = group->member_list;
			while (entry) {
				struct monster *mon = cave_monster(c, entry->midx);
				struct monster_group_info *info = mon->group_info;
				if (info[PRIMARY_GROUP].index != i) {
					if (info[SUMMON_GROUP].index) {
						if (info[SUMMON_GROUP].index != i) {
							quit_fmt("Bad group index: group: %d, monster: %d",
									 i, info[SUMMON_GROUP].index);
						}
						if (info[SUMMON_GROUP].role != MON_GROUP_LEADER) {
							quit_fmt("Bad monster role: group: %d, monster: %d",
									 i, info[SUMMON_GROUP].index);
						}
					} else {
						quit_fmt("Bad group index: group: %d, monster: %d",
								 i, info[PRIMARY_GROUP].index);
					}
				}
				entry = entry->next;
			}
		}
	}
}

/**
 * Check if tactical cooperation is enabled via runtime option
 */
bool monster_tactical_cooperation_enabled(void)
{
	return OPT(player, ai_tactical_coop);
}

/**
 * Single-monster eligibility check for tactical cooperation:
 *  - not unique, not immobile
 *  - aware of the player, not nice/neutral
 *  - not confused / sleeping / stunned / terrified
 *
 * Does NOT judge whether a monster is an ally; see monsters_share_alliance().
 */
bool monster_is_tactically_eligible(const struct monster *mon)
{
	if (!mon) return false;
	if (monster_is_unique(mon)) return false;
	if (rf_has(mon->race->flags, RF_NEVER_MOVE)) return false;

	if (mon->m_timed[MON_TMD_CONF] || mon->m_timed[MON_TMD_FEAR] ||
		mon->m_timed[MON_TMD_SLEEP] || mon->m_timed[MON_TMD_STUN]) {
		return false;
	}

	if (!mflag_has(mon->mflag, MFLAG_AWARE)) return false;

	if (mflag_has(mon->mflag, MFLAG_NICE)) return false;

	return true;
}

/**
 * Check whether two monsters belong to the same tactical alliance.
 * Hierarchy (strongest to weakest):
 *   1. Same exact race (family)
 *   2. Shared summon group (same natural-born kin naturally-spawned kin)
 *   3. Shared PRIMARY_GROUP (same natural group naturally spawn with shared PRIMARY_GROUP with same base type OR shared race flag identity (orc/troll/demon/undead/animal/evil/nonliving)
 *
 * Excludes: cross-ecology clashes (animals vs demons), lone summon members are allowed only when explicitly share a summon group.
 */
bool monsters_share_alliance(const struct monster *a, const struct monster *b)
{
	if (!a || !b || a == b) return false;
	if (!monster_is_tactically_eligible(a) ||
		!monster_is_tactically_eligible(b)) {
		return false;
	}

	if (a->race == b->race) return true;

	if (a->group_info[SUMMON_GROUP].index &&
		b->group_info[SUMMON_GROUP].index &&
		a->group_info[SUMMON_GROUP].index ==
		b->group_info[SUMMON_GROUP].index) {
		return true;
	}

	if (a->race->base == b->race->base) return true;

	if (rf_has(a->race->flags, RF_ORC)    && rf_has(b->race->flags, RF_ORC))    return true;
	if (rf_has(a->race->flags, RF_TROLL)  && rf_has(b->race->flags, RF_TROLL))  return true;
	if (rf_has(a->race->flags, RF_GIANT)  && rf_has(b->race->flags, RF_GIANT))  return true;
	if (rf_has(a->race->flags, RF_DRAGON) && rf_has(b->race->flags, RF_DRAGON)) return true;
	if (rf_has(a->race->flags, RF_DEMON)  && rf_has(b->race->flags, RF_DEMON))  return true;
	if (rf_has(a->race->flags, RF_UNDEAD) && rf_has(b->race->flags, RF_UNDEAD)) return true;

	if (rf_has(a->race->flags, RF_ANIMAL) && rf_has(b->race->flags, RF_ANIMAL)) return true;

	return false;
}

/**
 * Check if a monster is primarily a melee attacker
 */
bool monster_is_melee(const struct monster *mon)
{
	if (!mon) return false;
	if (mon->race->blow) return true;
	return false;
}

/**
 * Check if a monster has ranged attack capabilities (archery or offensive spells)
 */
bool monster_is_ranged_attacker(const struct monster *mon)
{
	if (!mon) return false;
	if (monster_loves_archery(mon)) return true;
	if (mon->race->freq_spell > 0 && !rf_has(mon->race->flags, RF_NEVER_BLOW)) {
		return true;
	}
	if (mon->race->freq_innate > 0) return true;
	return false;
}

/**
 * Check if a monster is a spell caster (has non-innate spells, or a variety of spells)
 * Excludes monsters whose only ranged ability is archery / simple breath.
 */
bool monster_is_spell_caster(const struct monster *mon)
{
	if (!mon) return false;
	if (monster_loves_archery(mon)) return false;
	if (mon->race->freq_spell > 0) return true;
	return false;
}

/**
 * Count nearby allies using the stable alliance check.
 */
int monster_count_nearby_allies(struct chunk *c, const struct monster *mon, int range, bool same_race_only)
{
	int count = 0;
	int i;

	if (!monster_is_tactically_eligible(mon)) return 0;

	for (i = 1; i < c->mon_max; i++) {
		struct monster *other = cave_monster(c, i);
		if (!other || other == mon) continue;

		if (distance(mon->grid, other->grid) > range) continue;

		if (same_race_only) {
			if (monsters_share_alliance(mon, other) &&
				other->race == mon->race) {
				count++;
			}
		} else {
			if (monsters_share_alliance(mon, other)) {
				count++;
			}
		}
	}
	return count;
}

/**
 * Measure the corridor width around a position
 */
int monster_measure_corridor_width(struct chunk *c, const struct monster *mon)
{
	int width = 0;
	int i;

	for (i = 0; i < 4; i++) {
		struct loc grid1 = loc_sum(mon->grid, ddgrid_ddd[i * 2]);
		struct loc grid2 = loc_sum(mon->grid, ddgrid_ddd[(i * 2 + 4) % 8]);

		if (square_in_bounds(c, grid1) && square_ispassable(c, grid1)) {
			width++;
		}
		if (square_in_bounds(c, grid2) && square_ispassable(c, grid2)) {
			width++;
		}
	}
	return width;
}

/**
 * Find a nearby allied caster to escort (within range)
 */
struct monster *monster_find_nearby_caster(struct chunk *c, const struct monster *mon, int range)
{
	int i;
	struct monster *best_caster = NULL;
	int best_dist = range + 1;

	for (i = 1; i < c->mon_max; i++) {
		struct monster *other = cave_monster(c, i);
		int dist;

		if (!other || other == mon) continue;
		if (!monsters_share_alliance(mon, other)) continue;
		if (!monster_is_spell_caster(other)) continue;

		dist = distance(mon->grid, other->grid);
		if (dist <= range) {
			if (dist < best_dist) {
				best_caster = other;
				best_dist = dist;
			}
		}
	}
	return best_caster;
}

/**
 * Find a nearby allied melee monster (for casters to position behind)
 */
struct monster *monster_find_nearby_melee(struct chunk *c, const struct monster *mon, int range)
{
	int i;
	struct monster *best_melee = NULL;
	int best_dist = range + 1;

	for (i = 1; i < c->mon_max; i++) {
		struct monster *other = cave_monster(c, i);
		int dist;

		if (!other || other == mon) continue;
		if (!monsters_share_alliance(mon, other)) continue;
		if (!monster_is_melee(other)) continue;
		if (monster_is_spell_caster(other)) continue;

		dist = distance(mon->grid, other->grid);
		if (dist <= range) {
			if (dist < best_dist) {
				best_melee = other;
				best_dist = dist;
			}
		}
	}
	return best_melee;
}

/**
 * Calculate tactical context for a monster based on current surroundings
 */
struct tactical_context monster_calculate_tactical_context(struct chunk *c, const struct monster *mon)
{
	struct tactical_context ctx = { 0 };

	if (!monster_tactical_cooperation_enabled()) return ctx;
	if (!monster_is_tactically_eligible(mon)) return ctx;

	ctx.nearby_allies_same_race = monster_count_nearby_allies(c, mon, 5, true);
	ctx.nearby_allies_same_base = monster_count_nearby_allies(c, mon, 5, false);
	ctx.player_hp_percent = (player->chp * 100) / player->mhp;
	ctx.corridor_width = monster_measure_corridor_width(c, mon);
	ctx.is_ranged = monster_is_ranged_attacker(mon);
	ctx.is_caster = monster_is_spell_caster(mon);
	ctx.has_melee = monster_is_melee(mon);
	ctx.hp_percent = (mon->hp * 100) / mon->maxhp;
	ctx.distance_to_player = mon->cdis;

	return ctx;
}

/**
 * Determine tactical stance based on context and monster role
 * Returns a stable, non-random stance based on current conditions
 */
enum monster_tactical_stance monster_determine_tactical_stance(const struct tactical_context *ctx, const struct monster *mon)
{
	int total_allies = ctx->nearby_allies_same_base;

	if (!monster_is_tactically_eligible(mon)) {
		return TACTICAL_STANCE_NONE;
	}

	if (total_allies < 2) {
		return TACTICAL_STANCE_NONE;
	}

	if (ctx->is_caster) {
		if (ctx->player_hp_percent < 40) {
			return TACTICAL_STANCE_FOCUS_FIRE;
		}

		if (ctx->hp_percent < 60) {
			struct monster *melee = monster_find_nearby_melee(cave, mon, 4);
			if (melee) {
				return TACTICAL_STANCE_ESCORT_CASTER;
			}
		}

		return TACTICAL_STANCE_NONE;
	}

	if (ctx->has_melee && !ctx->is_ranged) {
		if (ctx->player_hp_percent < 30) {
			return TACTICAL_STANCE_FOCUS_FIRE;
		}

		if (ctx->corridor_width >= 6 && total_allies >= 3) {
			return TACTICAL_STANCE_SURROUND;
		}

		if (ctx->corridor_width <= 2 && ctx->hp_percent < 50 && total_allies >= 2) {
			return TACTICAL_STANCE_RETREAT;
		}

		if (total_allies >= 3) {
			struct monster *caster = monster_find_nearby_caster(cave, mon, 5);
			if (caster) {
				return TACTICAL_STANCE_ESCORT_CASTER;
			}
		}

		if (total_allies >= 2 && ctx->player_hp_percent < 50) {
			return TACTICAL_STANCE_SURROUND;
		}

		return TACTICAL_STANCE_NONE;
	}

	if (ctx->is_ranged) {
		if (ctx->player_hp_percent < 40) {
			return TACTICAL_STANCE_FOCUS_FIRE;
		}

		if (ctx->corridor_width <= 2 && total_allies >= 2) {
			return TACTICAL_STANCE_RETREAT;
		}

		return TACTICAL_STANCE_NONE;
	}

	return TACTICAL_STANCE_NONE;
}
