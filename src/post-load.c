/**
 * \file post-load.c
 * \brief Post-savefile-load state restoration
 *
 * After savefile data has been deserialized, this module runs a fixed
 * sequence of recomputations so the runtime state (derived values, view,
 * monster visibility, UI dirty flags) is consistent.  The UI layer only
 * reacts to the resulting state / event signals; it must not carry any
 * load-repair logic of its own.
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
#include "game-world.h"
#include "obj-knowledge.h"
#include "player-calcs.h"
#include "player-util.h"
#include "post-load.h"
#include "savefile.h"

/**
 * Unified entry point: deserialize a savefile, then (optionally) restore
 * runtime state.  Replaces the open-coded "savefile_load + flags +
 * post_load" pattern that used to be duplicated across ui-game, tests, and
 * spoiler mode.
 *
 * - LOAD_RESTORE_NONE: only set character_generated / playing markers;
 *   useful for non-interactive tools that just need deserialized data
 *   (artifact spoiler generation, etc.) and do not want UI side effects.
 * - LOAD_RESTORE_GAME: full post_load() state restoration including view,
 *   monster visibility, and UI dirty flags; use for normal game startup
 *   and tests that exercise game systems.
 *
 * Returns true on success.  On failure the savefile could not be
 * deserialized and no state markers are set.
 */
bool savefile_load_and_restore(const char *path, bool cheat_death,
			       load_restore_mode mode)
{
	bool ok = savefile_load(path, cheat_death);
	if (!ok)
		return false;

	character_generated = true;
	player->upkeep->playing = true;

	if (mode == LOAD_RESTORE_GAME)
		post_load();

	return true;
}

/**
 * Post-load state restoration.
 *
 * After savefile data has been deserialized, this function runs a fixed
 * sequence of recomputations so the runtime state (derived values, view,
 * monster visibility, UI dirty flags) is consistent.  The UI layer only
 * reacts to the resulting state / event signals; it must not carry any
 * load-repair logic of its own.
 *
 * Each step below is named to make the dependency order obvious; do not
 * reorder them.
 */

static void post_load_inventory(struct player *p)
{
	calc_inventory(p);

	p->upkeep->notice |= (PN_COMBINE | PN_IGNORE);
	notice_stuff(p);
}

static void post_load_derived_stats(struct player *p)
{
	p->upkeep->update |= (PU_INVEN | PU_BONUS | PU_HP | PU_MANA | PU_SPELLS);
}

static void post_load_timed_effects(struct player *p)
{
	player_update_light(p);
}

static void post_load_view_and_monsters(struct player *p)
{
	p->upkeep->update |= (PU_TORCH | PU_UPDATE_VIEW | PU_DISTANCE | PU_PANEL);
	update_stuff(p);
}

static void post_load_object_knowledge(struct player *p)
{
	update_player_object_knowledge(p);
}

static void post_load_ui_redraw(struct player *p)
{
	p->upkeep->redraw |= (PR_BASIC | PR_EXTRA | PR_SUBWINDOW |
			      PR_MAP | PR_INVEN | PR_EQUIP |
			      PR_MESSAGE | PR_FEELING | PR_LIGHT);
	redraw_stuff(p);
}

void post_load(void)
{
	if (player->is_dead)
		return;

	post_load_inventory(player);
	post_load_derived_stats(player);
	post_load_timed_effects(player);
	post_load_view_and_monsters(player);
	post_load_object_knowledge(player);
	post_load_ui_redraw(player);
}
