/**
 * \file post-load.h
 * \brief Post-savefile-load state restoration
 *
 * After savefile data has been deserialized by savefile_load(), the
 * runtime state (derived values, view, monster visibility, UI
 * dirty flags) must be recomputed in a well-defined order.  This module
 * owns that choreography.
 *
 * The savefile module itself only deserializes data.  The caller of
 * savefile_load() is responsible for invoking post_load() afterwards
 * when the runtime engine is ready to reconcile the deserialized state.
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

#ifndef POST_LOAD_H
#define POST_LOAD_H

/**
 * Mode for savefile_load_and_restore(): determines how much of the runtime
 * state is rebuilt after deserialization.
 */
typedef enum {
	LOAD_RESTORE_NONE = 0,
	LOAD_RESTORE_GAME = 1,
} load_restore_mode;

/**
 * Unified entry point: deserialize a savefile, then (optionally) restore
 * runtime state.  Replaces the open-coded "savefile_load + flags +
 * post_load" pattern that used to be duplicated across ui-game, tests, and
 * spoiler mode.
 *
 * Returns true on success.  On failure the savefile could not be
 * deserialized and no state markers are set.
 */
bool savefile_load_and_restore(const char *path, bool cheat_death,
			       load_restore_mode mode);

/**
 * Recompute derived runtime state after a successful savefile_load().
 *
 * Executes six strictly-ordered steps, internally decomposed into:
 *   1. post_load_inventory()       - equip/inven layout + pack reconcile (combine/autoignore)
 *   2. post_load_derived_stats()    - mark PU_* for bonus / HP / mana / spells
 *   3. post_load_timed_effects() - side effects of timed flags (light fuel)
 *   4. post_load_view_and_monsters() - light radius, FOV, monster visibility
 *   5. post_load_object_knowledge() - curse / object knowledge alignment
 *   6. post_load_ui_redraw()    - mark UI dirty flags + event signals
 *
 * Safe to call when the player is dead (no-op).
 */
void post_load(void);

#endif /* !POST_LOAD_H */
