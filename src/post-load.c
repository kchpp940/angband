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
#include "obj-knowledge.h"
#include "player-calcs.h"
#include "player-util.h"
#include "post-load.h"

/**
 * Post-load state restoration.
 *
 * After savefile data has been deserialized, this function runs a fixed
 * sequence of recomputations so the runtime state (derived values, view,
 * monster visibility, UI dirty flags) is consistent.  The UI layer only
 * reacts to the resulting state / event signals; it must not carry any
 * load-repair logic of its own.
 *
 * Order matters:
 *   1. Inventory/equipment layout (calc_inventory)
 *   2. Pack reconciliation notices (combine, autoignore)
 *   3. Player bonuses, HP, mana, spells
 *   4. Timed-effect side effects (light fuel burn)
 *   5. Light radius calculation
 *   6. Field of view
 *   7. Monster visibility / distance updates
 *   8. Curse / object knowledge alignment
 *   9. UI dirty flags for a full redraw
 */
void post_load(void)
{
	if (player->is_dead)
		return;

	calc_inventory(player);

	player->upkeep->notice |= (PN_COMBINE | PN_IGNORE);
	notice_stuff(player);

	player->upkeep->update |= (PU_INVEN | PU_BONUS | PU_HP | PU_MANA | PU_SPELLS);

	player_update_light(player);

	player->upkeep->update |= (PU_TORCH | PU_UPDATE_VIEW | PU_DISTANCE | PU_PANEL);
	update_stuff(player);

	update_player_object_knowledge(player);

	player->upkeep->redraw |= (PR_BASIC | PR_EXTRA | PR_SUBWINDOW |
				   PR_MAP | PR_INVEN | PR_EQUIP |
				   PR_MESSAGE | PR_FEELING | PR_LIGHT);
	redraw_stuff(player);
}
