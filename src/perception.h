/**
 * \file perception.h
 * \brief Unified monster perception service - readonly queries for
 *        whether the player can sense, see, or target a monster.
 *
 * Centralises the visibility/ESP/invisibility/camouflage/hallucination
 * checks that were previously spread across mon-util, target, cave-view
 * and ui-target so that UI and logic layers agree on what the player
 * can actually perceive.
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

#ifndef PERCEPTION_H
#define PERCEPTION_H

#include "monster.h"

struct player;
struct chunk;

bool perception_mon_is_sensible(const struct monster *mon);
bool perception_mon_is_obvious(const struct monster *mon);
bool perception_mon_is_in_view(const struct monster *mon);
bool perception_mon_is_visible(const struct monster *mon);
bool perception_mon_is_targetable(const struct monster *mon);
bool perception_mon_is_projectable(const struct monster *mon);

bool perception_player_is_hallucinating(void);
bool perception_player_is_blind(void);

const char *perception_mon_detect_phrase(const struct monster *mon,
		struct chunk *c);

#endif /* PERCEPTION_H */
