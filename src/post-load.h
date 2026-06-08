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
 * Recompute derived runtime state after a successful savefile_load().
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
 *
 * Safe to call when the player is dead (no-op).
 */
void post_load(void);

#endif /* !POST_LOAD_H */
