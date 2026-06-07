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
#include "mon-predicate.h"
#include "monster.h"
#include "perception.h"
#include "player.h"
#include "player-timed.h"
#include "project.h"

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
 * to the player, lie on a projectable path from the player, and the
 * player must not be hallucinating.  Call this from both the target
 * logic and any UI that offers "target this monster" choices so the
 * two layers never disagree.
 */
bool perception_mon_is_targetable(const struct monster *mon)
{
	if (!mon || !mon->race) return false;

	if (!perception_mon_is_obvious(mon)) return false;

	if (!perception_mon_is_projectable(mon)) return false;

	if (perception_player_is_hallucinating()) return false;

	return true;
}

/**
 * Does an unobstructed projection path exist from the player to the monster?
 *
 * Thin wrapper around projectable() so callers don't have to reach into
 * project.h for a simple "can I hit this" check.
 */
bool perception_mon_is_projectable(const struct monster *mon)
{
	assert(mon);
	assert(cave);
	assert(player);

	return projectable(cave, player->grid, mon->grid, PROJECT_NONE);
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
