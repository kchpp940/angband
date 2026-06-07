/**
 * \file perception.c
 * \brief Unified monster perception service - readonly queries.
 *
 * Consolidates visibility, ESP, invisibility, camouflage, hallucination
 * and target validity checks that were previously ad-hoc in target.c,
 * ui-target.c and elsewhere.  Callers should use these functions instead
 * of hand-rolling conditions from MFLAG_*, square_* and player->timed[].
 *
 * Copyright (c) 1997-2007 Angband contributors
 * Copyright (c) 2026 Perception refactor contributors
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
#include "mon-lore.h"
#include "mon-predicate.h"
#include "monster.h"
#include "obj-ignore.h"
#include "perception.h"
#include "player.h"
#include "player-calcs.h"
#include "player-timed.h"
#include "player-util.h"
#include "project.h"
#include "target.h"
#include "z-bitflag.h"

/**
 * Is the monster perceptible by the player in any way?
 *
 * Covers direct sight, ESP / telepathy, prior detection (MFLAG_MARK),
 * infravision and see-invisible.  This is the weakest predicate; use
 * it for things like "should the monster appear in the monster list".
 */
bool perception_mon_is_sensible(const struct monster *mon)
{
	if (!mon || !mon->race) return false;

	if (monster_is_visible(mon)) return true;

	if (mflag_has(mon->mflag, MFLAG_MARK)) return true;

	return false;
}

/**
 * Is the monster obviously a monster to the player?
 *
 * Combines "visible to the player" with "not camouflaged as an item or
 * terrain".  Use this for UI prompts that describe the grid contents.
 */
bool perception_mon_is_obvious(const struct monster *mon)
{
	return monster_is_obvious(mon);
}

/**
 * Is the monster currently in the player's direct line of sight?
 *
 * Corresponds to MFLAG_VIEW (set by update_view / update_mon).  A
 * monster can be in view but not SEEN (e.g. the player is blind, or
 * the monster is invisible and the player lacks see-invisible).
 */
bool perception_mon_is_in_view(const struct monster *mon)
{
	return monster_is_in_view(mon);
}

/**
 * Is the monster currently visible to the player?
 *
 * Corresponds to MFLAG_VISIBLE (set by update_mon).  Visible means the
 * player can perceive the monster somehow - by sight, infravision, ESP
 * or a prior detection mark.  Does NOT imply the monster is in direct
 * LOS, or that the player recognises it as a monster (see
 * perception_mon_is_obvious for that).
 */
bool perception_mon_is_visible(const struct monster *mon)
{
	return monster_is_visible(mon);
}

/**
 * Is the monster a valid projectile target right now?
 *
 * Unifies the target_able() check: the monster must exist, be obvious
 * to the player, lie on a projectable path from the player for the
 * given projection flags, and the player must not be hallucinating.
 * Call this from both the target logic and any UI that offers "target
 * this monster" choices so the two layers never disagree.
 *
 * \param mon The monster to evaluate.
 * \param proj_flags PROJECT_* bitmask forwarded to projectable() - use
 *        PROJECT_NONE for a standard line-of-fire check, or combine
 *        PROJECT_THRU / PROJECT_SHORT / etc for specialised spells or
 *        ranged attack modes.
 */
bool perception_mon_is_targetable(const struct monster *mon, int proj_flags)
{
	if (!mon || !mon->race) return false;

	if (!perception_mon_is_obvious(mon)) return false;

	if (!perception_mon_is_projectable(mon, proj_flags)) return false;

	if (perception_player_is_hallucinating()) return false;

	return true;
}

/**
 * Does an unobstructed projection path exist from the player to the monster?
 *
 * Thin wrapper around projectable() so callers don't have to reach into
 * project.h for a simple "can I hit this" check.
 *
 * \param mon The target monster.
 * \param proj_flags PROJECT_* bitmask - PROJECT_NONE for standard LOS,
 *        PROJECT_THRU to pass through monsters, PROJECT_SHORT for
 *        monster-range-only etc.
 */
bool perception_mon_is_projectable(const struct monster *mon, int proj_flags)
{
	assert(mon);
	assert(cave);
	assert(player);

	return projectable(cave, player->grid, mon->grid, proj_flags);
}

/**
 * Is the player currently hallucinating (seeing imaginary things)?
 */
bool perception_player_is_hallucinating(void)
{
	assert(player);
	return player->timed[TMD_IMAGE] != 0;
}

/**
 * Is the player currently blind?
 */
bool perception_player_is_blind(void)
{
	assert(player);
	return player->timed[TMD_BLIND] != 0;
}

/**
 * Return the appropriate detect-phrase for a monster on the given grid.
 *
 * Used by look / target UI.  Returns one of:
 *   "You see "    - monster is on a directly seen grid
 *   "You sense "  - monster is obvious but off the direct view path
 *                   (detected via ESP or a detection spell)
 *   "You recall " - grid is memorised but nothing there is perceptible
 */
const char *perception_mon_detect_phrase(const struct monster *mon,
		struct chunk *c)
{
	assert(c);

	if (square_isseen(c, mon->grid)) {
		return "You see ";
	}

	if (mon && perception_mon_is_obvious(mon)) {
		return "You sense ";
	}

	return "You recall ";
}

/**
 * Analyse the path from player to a seen monster and forget any grids
 * which would have blocked line of sight.
 */
static void perception_path_analyse(struct chunk *c, struct loc grid)
{
	int path_n, i;
	struct loc path_g[256];

	if (c != cave) {
		return;
	}

	path_n = project_path(c, path_g, z_info->max_range, player->grid,
		grid, PROJECT_NONE);

	for (i = 0; i < path_n - 1; ++i) {
		if (!square_allowslos(player->cave, path_g[i])) {
			sqinfo_off(square(c, path_g[i])->info, SQUARE_SEEN);
			square_forget(c, path_g[i]);
			square_light_spot(c, path_g[i]);
		}
	}
}

/**
 * Pure computation: calculate whether a monster is sensible and/or in view,
 * without writing any flags or triggering side effects.
 *
 * This is the single source of truth for monster perception computation.
 * Both the refresh path (which writes MFLAG_VISIBLE/MFLAG_VIEW) and the
 * readonly query paths ultimately rely on the same logic implemented here.
 *
 * \param mon The monster to evaluate (must be non-NULL and have a race).
 * \param c The current chunk.
 * \param compute_distance If true, recalculate mon->cdis; otherwise reuse it.
 * \return A perception_result_t with .sensible, .in_view and .distance.
 */
perception_result_t perception_compute_mon_visibility(const struct monster *mon,
		struct chunk *c, bool compute_distance)
{
	perception_result_t result = { false, false, 0 };
	struct monster_lore *lore;

	bool telepathy_ok;
	struct loc pgrid;
	int d;

	assert(mon != NULL);
	assert(mon->race != NULL);
	assert(c != NULL);

	lore = get_lore(mon->race);
	telepathy_ok = player_of_has(player, OF_TELEPATHY);

	pgrid = character_dungeon ? player->grid : loc(c->width / 2, c->height / 2);

	if (compute_distance) {
		int dy = ABS(pgrid.y - mon->grid.y);
		int dx = ABS(pgrid.x - mon->grid.x);
		d = (dy > dx) ? (dy + (dx >> 1)) : (dx + (dy >> 1));
		if (d > 255) d = 255;
	} else {
		d = mon->cdis;
	}
	result.distance = d;

	if (mflag_has(mon->mflag, MFLAG_MARK)) {
		result.sensible = true;
	}

	if (square_isno_esp(c, mon->grid) || square_isno_esp(c, pgrid)) {
		telepathy_ok = false;
	}

	if (d <= z_info->max_sight) {
		if (telepathy_ok && monster_is_esp_detectable(mon)) {
			result.sensible = true;
			if (square_isview(c, mon->grid)) {
				result.in_view = true;
			}
		}

		if (square_isview(c, mon->grid) && !perception_player_is_blind()) {
			if (d <= player->state.see_infra) {
				rf_on(lore->flags, RF_COLD_BLOOD);
				if (!rf_has(mon->race->flags, RF_COLD_BLOOD)) {
					result.in_view = true;
					result.sensible = true;
				}
			}

			if (square_isseen(c, mon->grid)) {
				rf_on(lore->flags, RF_INVISIBLE);
				if (monster_is_invisible(mon)) {
					if (player_of_has(player, OF_SEE_INVIS)) {
						result.in_view = true;
						result.sensible = true;
					}
				} else {
					result.in_view = true;
					result.sensible = true;
				}
			}
		}
	}

	if (monster_is_mimicking(mon)) {
		struct object *obj = mon->mimicked_obj;
		if (ignore_item_ok(player, obj)) {
			result.in_view = false;
			result.sensible = false;
		}
	}

	return result;
}

/**
 * Refresh perception state for a single monster.
 *
 * Recomputes visibility via perception_compute_mon_visibility(), then
 * writes MFLAG_VISIBLE / MFLAG_VIEW, triggers redraws and disturbance,
 * updates lore sightings, and invalidates tracked targets if needed.
 * This replaces the hand-rolled logic that used to live in update_mon().
 */
void perception_refresh_mon(struct monster *mon, struct chunk *c, bool full)
{
	perception_result_t res;
	struct monster_lore *lore;
	bool was_visible, was_in_view;
	bool telepathy_ok;

	assert(mon != NULL);
	assert(c != NULL);

	if (c != cave) {
		return;
	}

	lore = get_lore(mon->race);
	was_visible = monster_is_visible(mon);
	was_in_view = monster_is_in_view(mon);

	if (full) {
		struct loc pgrid = character_dungeon ? player->grid :
			loc(c->width / 2, c->height / 2);
		int dy = ABS(pgrid.y - mon->grid.y);
		int dx = ABS(pgrid.x - mon->grid.x);
		int d = (dy > dx) ? (dy + (dx >> 1)) : (dx + (dy >> 1));
		if (d > 255) d = 255;
		mon->cdis = d;
	}

	res = perception_compute_mon_visibility(mon, c, false);

	telepathy_ok = player_of_has(player, OF_TELEPATHY);
	if (square_isno_esp(c, mon->grid) ||
			square_isno_esp(c, character_dungeon ? player->grid :
				loc(c->width / 2, c->height / 2))) {
		telepathy_ok = false;
	}

	if (res.in_view && square_isview(c, mon->grid) && !perception_player_is_blind()) {
		perception_path_analyse(c, mon->grid);
	}

	if (res.sensible) {
		if (telepathy_ok) {
			flags_set(lore->flags, RF_SIZE, RF_EMPTY_MIND, RF_WEIRD_MIND,
					  RF_SMART, RF_STUPID, FLAG_END);
		}

		if (!was_visible) {
			mflag_on(mon->mflag, MFLAG_VISIBLE);
			square_light_spot(c, mon->grid);
			if (player->upkeep->health_who == mon)
				player->upkeep->redraw |= (PR_HEALTH);
			if (lore->sights < SHRT_MAX)
				lore->sights++;
			player->upkeep->redraw |= PR_MONLIST;
		}
	} else if (was_visible) {
		if (!mon->mimicked_obj
				|| ignore_item_ok(player, mon->mimicked_obj)) {
			mflag_off(mon->mflag, MFLAG_VISIBLE);
			square_light_spot(c, mon->grid);
			if (player->upkeep->health_who == mon)
				player->upkeep->redraw |= (PR_HEALTH);
			player->upkeep->redraw |= PR_MONLIST;
		}
	}

	if (res.in_view) {
		if (!was_in_view) {
			mflag_on(mon->mflag, MFLAG_VIEW);
			if (OPT(player, disturb_near))
				disturb(player);
			player->upkeep->redraw |= PR_MONLIST;
		}
	} else {
		if (was_in_view) {
			mflag_off(mon->mflag, MFLAG_VIEW);
			if (OPT(player, disturb_near) && !monster_is_camouflaged(mon))
				disturb(player);
			player->upkeep->redraw |= PR_MONLIST;
		}
	}
}

/**
 * Refresh perception for every live monster on the current level,
 * then clean up any targets that have become invalid.
 */
void perception_refresh_all(bool full)
{
	int i;

	for (i = 1; i < cave_monster_max(cave); i++) {
		struct monster *mon = cave_monster(cave, i);
		if (mon->race)
			perception_refresh_mon(mon, cave, full);
	}

	perception_invalidate_targets();
}

/**
 * Invalidate tracked targets that are no longer perceptible.
 *
 * Clears the health bar trackee and the current monster target when
 * the underlying monster ceases to exist or is no longer targetable.
 * Called automatically by perception_refresh_all(), but may also be
 * invoked directly after events that can make a monster disappear.
 */
void perception_invalidate_targets(void)
{
	struct monster *health_mon = player->upkeep->health_who;
	if (health_mon) {
		if (!health_mon->race || !perception_mon_is_sensible(health_mon)) {
			player->upkeep->health_who = NULL;
			player->upkeep->redraw |= (PR_HEALTH);
		}
	}

	if (!target_okay()) {
		target_set_monster(NULL);
	}
}

/**
 * Unified world refresh - refresh grid-level view and monster perception in one call.
 *
 * Guarantees that after this function returns, SQUARE_VIEW/SEEN and
 * MFLAG_VISIBLE/VIEW are consistent with each other, so any query done
 * via the perception_*() interfaces will see a self-consistent snapshot
 * of the world rather than stale MFLAG_* after a fresh update_view().
 *
 * Replaces the old pattern of:
 *   update_view(cave, p);        // PU_UPDATE_VIEW
 *   ...                           // <- stale window if interrupted here
 *   update_monsters(full);            // PU_DISTANCE / PU_MONSTERS
 *
 * \param need_view_update If true, run update_view() to recompute grid-level
 *        SQUARE_VIEW / SQUARE_SEEN before refreshing monster flags.
 * \param full_distance If true, recompute every monster's cdis field
 *        (cdis is the "classic" distance used internally for sight-range
 *        checks - when player position has changed significantly.
 */
void perception_update_world(bool need_view_update, bool full_distance)
{
	assert(player);
	assert(cave);

	if (need_view_update) {
		update_view(cave, player);
	}

	perception_refresh_all(full_distance);
}
