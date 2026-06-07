/**
 * \file ui-danger.h
 * \brief Danger warning visualization system
 *
 * Provides visual and audio warnings for dangerous game states:
 * - Low hitpoints
 * - Surrounded by monsters
 * - Powerful monsters entering view
 * - Dangerous terrain nearby
 *
 * Copyright (c) 2026 Angband contributors
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

#ifndef INCLUDED_UI_DANGER_H
#define INCLUDED_UI_DANGER_H

#include "h-basic.h"

/**
 * Types of danger conditions that can trigger warnings
 */
typedef enum danger_type {
	DANGER_NONE = 0,
	DANGER_LOW_HP = 0x01,
	DANGER_CRITICAL_HP = 0x02,
	DANGER_SURROUNDED = 0x04,
	DANGER_POWERFUL_MONSTER = 0x08,
	DANGER_DANGEROUS_TERRAIN = 0x10,
	DANGER_ALL = 0xFF
} danger_type;

/**
 * Warning visualization intensity levels
 */
typedef enum danger_intensity {
	DANGER_INTENSITY_OFF = 0,
	DANGER_INTENSITY_LOW = 1,
	DANGER_INTENSITY_MEDIUM = 2,
	DANGER_INTENSITY_HIGH = 3
} danger_intensity;

/**
 * Warning display modes (bitflags, combinable)
 */
typedef enum danger_warn_mode {
	DANGER_WARN_NONE = 0,
	DANGER_WARN_BORDER_FLASH = 0x01,
	DANGER_WARN_STATUSBAR_EMPHASIS = 0x02,
	DANGER_WARN_MESSAGE_HIGHLIGHT = 0x04,
	DANGER_WARN_SOUND = 0x08,
	DANGER_WARN_ALL = 0x0F
} danger_warn_mode;

/**
 * Current danger state - aggregated from all active danger conditions
 */
struct danger_state {
	uint32_t active_types;
	danger_intensity max_intensity;
	uint32_t last_update_turn;
	bool suppressed;
};

/**
 * Danger configuration per type
 */
struct danger_config {
	bool enabled;
	danger_intensity intensity;
	uint8_t warn_modes;
	int threshold;
};

/**
 * Complete danger warning configuration
 */
struct danger_warning_options {
	struct danger_config low_hp;
	struct danger_config critical_hp;
	struct danger_config surrounded;
	struct danger_config powerful_monster;
	struct danger_config dangerous_terrain;

	bool suppress_when_resting;
	bool suppress_when_running;
	bool suppress_when_repeating;
	bool suppress_in_subwindows;

	uint8_t global_intensity_cap;
	uint16_t cooldown_turns;
};

void danger_warnings_init(void);
void danger_warnings_free(void);
void danger_warnings_reset_state(void);

void danger_check_all(void);
struct danger_state *danger_get_state(void);
bool danger_state_changed(void);

bool danger_is_active(danger_type type);
danger_intensity danger_get_max_intensity(void);
uint8_t danger_get_active_modes(void);

void danger_suppress(bool suppress);
bool danger_is_suppressed(void);

void danger_config_set_defaults(struct danger_warning_options *opts);
const struct danger_warning_options *danger_config_get(void);
void danger_config_update(const struct danger_warning_options *opts);

bool danger_option_enabled(danger_type type);
uint8_t danger_option_modes(danger_type type);
danger_intensity danger_option_intensity(danger_type type);

void danger_signal_warning(danger_type type, danger_intensity intensity,
	const char *message);

struct player_options;
void danger_sync_from_options(const struct player_options *opts);
void danger_apply_display(void);
void danger_handle_event(void);
void danger_register_handler(void);

uint8_t danger_get_message_color(void);

#endif /* INCLUDED_UI_DANGER_H */
