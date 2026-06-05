/**
 * \file target.c
 * \brief Targetting code
 *
 * Copyright (c) 1997-2007 Angband contributors
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
#include "game-input.h"
#include "init.h"
#include "mon-desc.h"
#include "mon-util.h"
#include "monster.h"
#include "obj-ignore.h"
#include "player-calcs.h"
#include "player-timed.h"
#include "project.h"
#include "target.h"

/**
 * Is the target set?
 */
static bool target_set;

/**
 * Is the target fixed (for the duration of a spell)?
 */
static bool target_fixed;

/**
 * Player target
 */
static struct target target;

/**
 * Old player target
 */
static struct target old_target;

/**
 * Current targeting context projectile flags
 *
 * This is set before targeting begins (e.g., before calling get_aim_dir()
 * or target_set_interactive()) and is used throughout the targeting process
 * to ensure consistent checks between target list generation, path preview,
 * target validation, and target invalidation.
 *
 * Defaults to PROJECT_STOP as a conservative default.
 */
static int target_context_proj_flags = PROJECT_STOP;

/**
 * Set the current targeting context projectile flags.
 *
 * Call this before initiating any targeting action to ensure all subsequent
 * target checks use the same projectile flags as the actual action will use.
 * Also re-validates any existing target under the new flags.
 */
void target_set_context_proj_flags(int proj_flags)
{
	target_context_proj_flags = proj_flags;

	/*
	 * Re-validate existing target under new flags. If the target was
	 * selected with different rules (e.g. PROJECT_JUMP from a previous
	 * strike spell) and is not valid under the new rules (e.g.
	 * PROJECT_STOP for a bolt), clear it so the player must choose
	 * again.
	 */
	if (target_set && target.midx > 0) {
		struct monster *mon = cave_monster(cave, target.midx);
		if (mon && target_able_with_flags(mon, proj_flags)) {
			target.proj_flags = proj_flags;
		} else if (!target_fixed) {
			target_set = false;
			target.midx = 0;
			target.grid.y = 0;
			target.grid.x = 0;
			target.proj_flags = 0;
		}
	} else if (target_set && target.grid.x && target.grid.y) {
		target.proj_flags = proj_flags;
	}
}

/**
 * Begin a targeting action with specific projectile flags.
 *
 * This is the UNIFIED ENTRY POINT for any command that needs to aim, fire,
 * cast a spell, or otherwise perform an action with targeting. It sets the
 * context flags and re-validates any existing target under the new rules.
 *
 * Always call target_action_end() when done to avoid state leakage between
 * different actions.
 *
 * Example:
 *   target_action_begin(PROJECT_STOP);
 *   // ... do targeting/firing/casting ...
 *   target_action_end();
 */
void target_action_begin(int proj_flags)
{
	target_set_context_proj_flags(proj_flags);
}

/**
 * End a targeting action.
 *
 * Resets the context to default (PROJECT_STOP) to prevent state leakage
 * between different actions. Always pair this with target_action_begin().
 */
void target_action_end(void)
{
	target_reset_context();
}

/**
 * Get the current targeting context projectile flags.
 */
int target_get_context_proj_flags(void)
{
	return target_context_proj_flags;
}

/**
 * Reset the targeting context to the default flags (PROJECT_STOP).
 *
 * Prefer target_action_end() for command-level code. This function is
 * mainly for internal use.
 */
void target_reset_context(void)
{
	target_context_proj_flags = PROJECT_STOP;
}

/**
 * Monster health description
 */
void look_mon_desc(char *buf, size_t max, int m_idx)
{
	struct monster *mon = cave_monster(cave, m_idx);

	bool living = true;

	if (!mon) return;

	/* Determine if the monster is "living" (vs "undead") */
	if (monster_is_destroyed(mon)) living = false;

	/* Assess health */
	if (mon->hp >= mon->maxhp) {
		/* No damage */
		my_strcpy(buf, (living ? "unhurt" : "undamaged"), max);
	} else {
		/* Calculate a health "percentage" */
		int perc = 100L * mon->hp / mon->maxhp;

		if (perc >= 60)
			my_strcpy(buf, (living ? "somewhat wounded" : "somewhat damaged"),
					  max);
		else if (perc >= 25)
			my_strcpy(buf, (living ? "wounded" : "damaged"), max);
		else if (perc >= 10)
			my_strcpy(buf, (living ? "badly wounded" : "badly damaged"), max);
		else
			my_strcpy(buf, (living ? "almost dead" : "almost destroyed"), max);
	}

	/* Effect status */
	if (mon->m_timed[MON_TMD_SLEEP]) my_strcat(buf, ", asleep", max);
	if (mon->m_timed[MON_TMD_HOLD]) my_strcat(buf, ", held", max);
	if (mon->m_timed[MON_TMD_DISEN]) my_strcat(buf, ", disenchanted", max);
	if (mon->m_timed[MON_TMD_CONF]) my_strcat(buf, ", confused", max);
	if (mon->m_timed[MON_TMD_FEAR]) my_strcat(buf, ", afraid", max);
	if (mon->m_timed[MON_TMD_STUN]) my_strcat(buf, ", stunned", max);
	if (mon->m_timed[MON_TMD_SLOW]) my_strcat(buf, ", slowed", max);
	if (mon->m_timed[MON_TMD_FAST]) my_strcat(buf, ", hasted", max);
}



/**
 * Determine if a monster makes a reasonable target
 *
 * The concept of "targetting" was stolen from "Morgul" (?)
 *
 * The player can target any location, or any "target-able" monster.
 *
 * Currently, a monster is "target_able" if it is visible, and if
 * the player can hit it with a projection, and the player is not
 * hallucinating.  This allows use of "use closest target" macros.
 */
bool target_able(struct monster *m)
{
	return target_able_with_flags(m, PROJECT_NONE);
}

/**
 * Determine if a monster is targetable with specific projection flags.
 *
 * This allows callers to match the projection flags of the actual action
 * (e.g., PROJECT_STOP for bolts, PROJECT_JUMP for strike effects).
 */
bool target_able_with_flags(struct monster *m, int proj_flags)
{
	return m && m->race && monster_is_obvious(m) &&
		projectable(cave, player->grid, m->grid, proj_flags) &&
		!player->timed[TMD_IMAGE];
}



/**
 * Check if the current target is valid under the given projection flags.
 *
 * This is a PURE PREDICATE: it only returns true/false and does NOT modify
 * any global state. Use this for UI polling, right-click menus, parameter
 * validation, and anywhere you just want to know if a target is currently
 * targetable.
 *
 * Does NOT clear the target, does NOT update target.proj_flags.
 */
bool target_check_okay(int proj_flags)
{
	/* No target */
	if (!target_set) return false;

	/* Check "monster" targets */
	if (target.midx > 0) {
		struct monster *mon = cave_monster(cave, target.midx);
		if (target_able_with_flags(mon, proj_flags)) {
			return true;
		}
	} else if (target.grid.x && target.grid.y) {
		/* Allow a direction without a monster */
		return true;
	}

	return false;
}


/**
 * Validate and refresh the target for actual use.
 *
 * This HAS SIDE EFFECTS: if the target is valid under the current context
 * flags, target.proj_flags is updated to match. If not valid, the
 * target is cleared.
 *
 * Call this ONLY at action confirmation points: when the user presses '5'
 * to confirm target, when actually firing/casting, or when you intend to
 * USE the target for real.
 *
 * Do NOT call this for UI polling or parameter validation.
 */
bool target_okay(void)
{
	/* No target */
	if (!target_set) return false;

	/* Always validate with current context flags */
	int proj_flags = target_context_proj_flags;

	/* Check "monster" targets */
	if (target.midx > 0) {
		struct monster *mon = cave_monster(cave, target.midx);
		if (target_able_with_flags(mon, proj_flags)) {
			/* Get the monster location */
			target.grid = mon->grid;

			/* Update stored flags to match current context */
			target.proj_flags = proj_flags;

			/* Good target */
			return true;
		}
	} else if (target.grid.x && target.grid.y) {
		/* Allow a direction without a monster */
		target.proj_flags = proj_flags;
		return true;
	}

	/* Target not valid under current rules */
	target_set = false;
	target.midx = 0;
	target.grid.y = 0;
	target.grid.x = 0;
	target.proj_flags = 0;

	return false;
}


/**
 * Set the target to a monster (or nobody); if target is fixed, don't unset
 * Uses current context projectile flags for validation.
 */
bool target_set_monster(struct monster *mon)
{
	return target_set_monster_with_flags(mon, target_context_proj_flags);
}

/**
 * Set the target to a monster (or nobody) with specific projectile flags.
 * If target is fixed, don't unset.
 */
bool target_set_monster_with_flags(struct monster *mon, int proj_flags)
{
	/* Acceptable target */
	if (mon && target_able_with_flags(mon, proj_flags)) {
		target_set = true;
		target.midx = mon->midx;
		target.grid = mon->grid;
		target.proj_flags = proj_flags;
		return true;
	} else if (target_fixed) {
		/* If a monster has died during a spell, this maintains its grid as
		 * the target in case further effects of the spell need it */
		target.midx = 0;
		return true;
	}

	/* Reset target info */
	target_set = false;
	target.midx = 0;
	target.grid.y = 0;
	target.grid.x = 0;
	target.proj_flags = 0;

	return false;
}


/**
 * Set the target to a location.
 * Uses current context projectile flags.
 */
void target_set_location(int y, int x)
{
	target_set_location_with_flags(y, x, target_context_proj_flags);
}

/**
 * Set the target to a location with specific projectile flags.
 */
void target_set_location_with_flags(int y, int x, int proj_flags)
{
	struct loc grid = loc(x, y);

	/* Legal target */
	if (square_in_bounds_fully(cave, grid)) {
		/* Save target info */
		target_set = true;
		target.midx = 0;
		target.grid = grid;
		target.proj_flags = proj_flags;
		return;
	}

	/* Reset target info */
	target_set = false;
	target.midx = 0;
	target.grid.y = 0;
	target.grid.x = 0;
	target.proj_flags = 0;
}

/**
 * Tell the UI the target is set
 */
bool target_is_set(void)
{
	return target_set;
}

/**
 * Fix the target
 */
void target_fix(void)
{
	old_target = target;
	target_fixed = true;
}

/**
 * Release the target
 *
 * Always validates against the current context flags, NOT stored flags.
 * This ensures that if the action context has changed since target_fix(),
 * the target is re-validated under the new rules.
 */
void target_release(void)
{
	target_fixed = false;

	/* If the old target is no longer targetable, cancel its grid */
	if (old_target.midx != 0) {
		struct monster *mon = cave_monster(cave, old_target.midx);
		if (!target_able_with_flags(mon, target_context_proj_flags)) {
			target.grid.y = 0;
			target.grid.x = 0;
		}
	}

	/* Also verify the current target is still valid under current context */
	if (target.midx != 0) {
		struct monster *mon = cave_monster(cave, target.midx);
		if (!target_able_with_flags(mon, target_context_proj_flags)) {
			target_set_monster(NULL);
		}
	}
}

/**
 * Sorting hook -- comp function -- by "distance to player"
 *
 * We use "u" and "v" to point to arrays of "x" and "y" positions,
 * and sort the arrays by double-distance to the player.
 */
int cmp_distance(const void *a, const void *b)
{
	int py = player->grid.y;
	int px = player->grid.x;

	const struct loc *pa = a;
	const struct loc *pb = b;

	int da, db, kx, ky;

	/* Absolute distance components */
	kx = pa->x; kx -= px; kx = ABS(kx);
	ky = pa->y; ky -= py; ky = ABS(ky);

	/* Approximate Double Distance to the first point */
	da = ((kx > ky) ? (kx + kx + ky) : (ky + ky + kx));

	/* Absolute distance components */
	kx = pb->x; kx -= px; kx = ABS(kx);
	ky = pb->y; ky -= py; ky = ABS(ky);

	/* Approximate Double Distance to the first point */
	db = ((kx > ky) ? (kx + kx + ky) : (ky + ky + kx));

	/* Compare the distances */
	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

/**
 * Help select a location.  This function picks the closest from a set in 
 *(roughly) a given direction.
 */
int16_t target_pick(int y1, int x1, int dy, int dx, struct point_set *targets)
{
	int i, v;

	int x2, y2, x3, y3, x4, y4;

	int b_i = -1, b_v = 9999;


	/* Scan the locations */
	for (i = 0; i < point_set_size(targets); i++) {
		/* Point 2 */
		x2 = targets->pts[i].x;
		y2 = targets->pts[i].y;

		/* Directed distance */
		x3 = (x2 - x1);
		y3 = (y2 - y1);

		/* Verify quadrant */
		if (dx && (x3 * dx <= 0)) continue;
		if (dy && (y3 * dy <= 0)) continue;

		/* Absolute distance */
		x4 = ABS(x3);
		y4 = ABS(y3);

		/* Verify quadrant */
		if (dy && !dx && (x4 > y4)) continue;
		if (dx && !dy && (y4 > x4)) continue;

		/* Approximate Double Distance */
		v = ((x4 > y4) ? (x4 + x4 + y4) : (y4 + y4 + x4));

		/* Track best */
		if ((b_i >= 0) && (v >= b_v)) continue;

		/* Track best */
		b_i = i; b_v = v;
	}

	/* Result */
	return (b_i);
}


/**
 * Determine if a given location is "interesting"
 */
bool target_accept(int y, int x)
{
	struct loc grid = loc(x, y);
	struct object *obj;

	/* Player grids are always interesting */
	if (square(cave, grid)->mon < 0) return true;

	/* Handle hallucination */
	if (player->timed[TMD_IMAGE]) return false;

	/* Obvious monsters */
	if (square(cave, grid)->mon > 0) {
		struct monster *mon = square_monster(cave, grid);
		if (monster_is_obvious(mon)) {
			return true;
		}
	}

	/* Traps */
	if (square_isvisibletrap(player->cave, grid)) return true;

	/* Scan all objects in the grid */
	for (obj = square_object(player->cave, grid); obj; obj = obj->next) {
		/* Memorized object */
		if (obj->kind == unknown_item_kind
				 || !ignore_known_item_ok(player, obj)) {
			return true;
		}
	}

	/* Interesting memorized features */
	if (square_isknown(cave, grid)
			&& square_isinteresting(player->cave, grid)) {
		return true;
	}

	/* Nope */
	return false;
}

/**
 * Describe a location relative to the player position.
 * e.g. "12 S 35 W" or "0 N, 33 E" or "0 N, 0 E"
 */
void coords_desc(char *buf, int size, int y, int x)
{
	const char *east_or_west;
	const char *north_or_south;

	int py = player->grid.y;
	int px = player->grid.x;

	if (y > py)
		north_or_south = "S";
	else
		north_or_south = "N";

	if (x < px)
		east_or_west = "W";
	else
		east_or_west = "E";

	strnfmt(buf, size, "%d %s, %d %s",
		ABS(y - py), north_or_south, ABS(x-px), east_or_west);
}

/**
 * Obtains the location the player currently targets.
 */
void target_get(struct loc *grid)
{
	assert(grid);
	*grid = target.grid;
}


/**
 * Returns the currently targeted monster index.
 */
struct monster *target_get_monster(void)
{
	return cave_monster(cave, target.midx);
}


/**
 * True if the player's current target is in LOS.
 *
 * Uses pure predicate target_check_okay() to avoid side effects.
 */
bool target_sighted(void)
{
	return target_check_okay(target_context_proj_flags) &&
			panel_contains(target.grid.y, target.grid.x) &&
			 /* either the target is a grid and is visible, or it is a monster
			  * that is visible */
		((!target.midx && square_isseen(cave, target.grid)) ||
		 (target.midx && monster_is_visible(cave_monster(cave, target.midx))));
}


#define TS_INITIAL_SIZE	20

/**
 * Return a target set of interesting locations including monsters, objects,
 * traps, and features.
 *
 * \param mode If mode includes TARGET_KILL, only target_able monsters matching
 * pred are included
 * \param pred The monster predicate used to filter monsters (optional)
 * \param restrict_to_panel Restricts the interesting points to the current
 * panel
 */
struct point_set *target_get_monsters(int mode, monster_predicate pred,
		bool restrict_to_panel)
{
	int y, x;
	int min_y, min_x, max_y, max_x;
	struct point_set *targets = point_set_new(TS_INITIAL_SIZE);

	if (restrict_to_panel) {
		/* Get the current panel */
		get_panel(&min_y, &min_x, &max_y, &max_x);
	} else {
		min_y = player->grid.y - z_info->max_range;
		max_y = player->grid.y + z_info->max_range + 1;
		min_x = player->grid.x - z_info->max_range;
		max_x = player->grid.x + z_info->max_range + 1;
	}

	/* Scan for targets */
	for (y = min_y; y < max_y; y++) {
		for (x = min_x; x < max_x; x++) {
			struct loc grid = loc(x, y);

			/* Check bounds */
			if (!square_in_bounds_fully(cave, grid)) continue;

			/* Require "interesting" contents */
			if (!target_accept(y, x)) continue;

			/* Special mode */
			if (mode & (TARGET_KILL)) {
				struct monster *mon = square_monster(cave, grid);
				int proj_flags = target_context_proj_flags;

				/* Must contain a monster */
				if (mon == NULL) continue;

				/*
				 * Must be a targettable monster using the current
				 * context's projectile flags (e.g., PROJECT_STOP
				 * for bolts, PROJECT_BEAM for beams, PROJECT_JUMP
				 * for strike effects)
				 */
				if (!target_able_with_flags(mon, proj_flags)) continue;

				/* Must be the right sort of monster */
				if (pred && !pred(mon)) continue;
			}

			/* Save the location */
			add_to_point_set(targets, grid);
		}
	}

	sort(targets->pts, point_set_size(targets), sizeof(*(targets->pts)),
		 cmp_distance);
	return targets;
}


/**
 * Set target to closest monster.
 */
bool target_set_closest(int mode, monster_predicate pred)
{
	struct monster *mon;
	char m_name[80];
	struct point_set *targets;

	/* Cancel old target */
	target_set_monster(NULL);

	/* Get ready to do targetting */
	targets = target_get_monsters(mode, pred, false);

	/* If nothing was prepared, then return */
	if (point_set_size(targets) < 1) {
		msg("No Available Target.");
		point_set_dispose(targets);
		return false;
	}

	/* Find the first monster in the queue */
	mon = square_monster(cave, targets->pts[0]);
	
	/* Target the monster, if possible (using context flags for consistency) */
	if (!target_able_with_flags(mon, target_context_proj_flags)) {
		msg("No Available Target.");
		point_set_dispose(targets);
		return false;
	}

	/* Target the monster */
	monster_desc(m_name, sizeof(m_name), mon, MDESC_CAPITAL | MDESC_COMMA);
	if (!(mode & TARGET_QUIET))
		msg("%s is targeted.", m_name);

	/* Set up target information */
	monster_race_track(player->upkeep, mon->race);
	health_track(player->upkeep, mon);
	target_set_monster(mon);

	point_set_dispose(targets);
	return true;
}
