/**
 * \file ui-danger.c
 * \brief Danger warning visualization system - core implementation
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

#include "angband.h"
#include "cave.h"
#include "game-event.h"
#include "game-world.h"
#include "mon-list.h"
#include "mon-predicate.h"
#include "monster.h"
#include "option.h"
#include "player.h"
#include "player-util.h"
#include "trap.h"
#include "ui-danger.h"
#include "ui-term.h"
#include "message.h"
#include "z-type.h"
#include "z-virt.h"

static struct danger_warning_options danger_opts;
static struct danger_state current_state;
static uint32_t prev_active_types = DANGER_NONE;
static danger_intensity prev_max_intensity = DANGER_INTENSITY_OFF;
static bool state_changed_flag = false;
static bool event_handler_registered = false;
static uint8_t cached_message_color = COLOUR_WHITE;
static bool message_color_cached = false;
static uint32_t last_warn_turn[5];
static uint32_t last_sound_turn = 0;
static uint32_t last_render_turn = 0;
static bool *powerful_monster_seen_before = NULL;
static int powerful_monster_seen_count = 0;
static bool render_pending = false;
static void danger_event_callback(game_event_type event_type, game_event_data *data, void *user);

#define DANGER_TYPE_IDX(t) \
	((t) == DANGER_LOW_HP ? 0 : \
	 (t) == DANGER_CRITICAL_HP ? 1 : \
	 (t) == DANGER_SURROUNDED ? 2 : \
	 (t) == DANGER_POWERFUL_MONSTER ? 3 : \
	 (t) == DANGER_DANGEROUS_TERRAIN ? 4 : 0)

static int count_adjacent_visible_monsters(void)
{
	int count = 0;
	struct loc grid;
	int dy, dx;

	if (!cave || !player) return 0;

	for (dy = -1; dy <= 1; dy++) {
		for (dx = -1; dx <= 1; dx++) {
			struct monster *mon;

			if (dy == 0 && dx == 0) continue;

			grid.y = player->grid.y + dy;
			grid.x = player->grid.x + dx;

			if (!square_in_bounds(cave, grid)) continue;

			mon = square_monster(cave, grid);
			if (mon && monster_is_visible(mon)) {
				count++;
			}
		}
	}

	return count;
}

static void ensure_powerful_monster_tracker(int max_idx)
{
	if (max_idx >= powerful_monster_seen_count) {
		int new_count = max_idx + 32;
		powerful_monster_seen_before = mem_realloc(powerful_monster_seen_before,
			new_count * sizeof(bool));
		memset(powerful_monster_seen_before + powerful_monster_seen_count, 0,
			(new_count - powerful_monster_seen_count) * sizeof(bool));
		powerful_monster_seen_count = new_count;
	}
}

static bool check_powerful_monster_in_view(void)
{
	int i;
	bool found = false;
	int mon_max;

	if (!cave || !player) return false;

	mon_max = cave_monster_max(cave);
	ensure_powerful_monster_tracker(mon_max);

	for (i = 1; i < mon_max; i++) {
		struct monster *mon = cave_monster(cave, i);
		int level_diff;

		if (!mon || !monster_is_obvious(mon)) continue;

		level_diff = (int)mon->race->level - (int)player->lev;

		if (level_diff >= danger_opts.powerful_monster.threshold) {
			if (!powerful_monster_seen_before[i]) {
				powerful_monster_seen_before[i] = true;
				found = true;
			} else {
				found = true;
			}
		}
	}

	return found;
}

static bool check_dangerous_terrain_nearby(void)
{
	struct loc grid;
	int dy, dx;
	int range = danger_opts.dangerous_terrain.threshold;

	if (!cave || !player) return false;

	for (dy = -range; dy <= range; dy++) {
		for (dx = -range; dx <= range; dx++) {
			grid.y = player->grid.y + dy;
			grid.x = player->grid.x + dx;

			if (!square_in_bounds(cave, grid)) continue;
			if (!square_isfloor(cave, grid) && !square_isrock(cave, grid)) continue;

			if (square_isvisibletrap(cave, grid)) {
				return true;
			}

			if (tf_has(square_feat(cave, grid)->flags, TF_FIERY)) {
				return true;
			}
		}
	}

	return false;
}

static bool should_suppress(void)
{
	if (!player) return true;

	if (current_state.suppressed) return true;

	if (danger_opts.suppress_when_resting && player_resting_count(player) > 0) {
		return true;
	}

	if (danger_opts.suppress_when_running && player->upkeep->running > 0) {
		return true;
	}

	return false;
}

static bool check_cooldown(danger_type type)
{
	int idx = DANGER_TYPE_IDX(type);

	if (!player) return false;

	if ((uint32_t)player->total_energy - last_warn_turn[idx]
		< (uint32_t)danger_opts.cooldown_turns * 100) {
		return false;
	}

	return true;
}

static void mark_warn_time(danger_type type)
{
	int idx = DANGER_TYPE_IDX(type);
	if (player) {
		last_warn_turn[idx] = player->total_energy;
	}
}

static struct danger_config *get_config_for_type(danger_type type)
{
	switch (type) {
		case DANGER_LOW_HP: return &danger_opts.low_hp;
		case DANGER_CRITICAL_HP: return &danger_opts.critical_hp;
		case DANGER_SURROUNDED: return &danger_opts.surrounded;
		case DANGER_POWERFUL_MONSTER: return &danger_opts.powerful_monster;
		case DANGER_DANGEROUS_TERRAIN: return &danger_opts.dangerous_terrain;
		default: return NULL;
	}
}

void danger_config_set_defaults(struct danger_warning_options *opts)
{
	if (!opts) return;

	opts->low_hp.enabled = true;
	opts->low_hp.intensity = DANGER_INTENSITY_MEDIUM;
	opts->low_hp.warn_modes = DANGER_WARN_BORDER_FLASH
		| DANGER_WARN_STATUSBAR_EMPHASIS
		| DANGER_WARN_MESSAGE_HIGHLIGHT;
	opts->low_hp.threshold = 30;

	opts->critical_hp.enabled = true;
	opts->critical_hp.intensity = DANGER_INTENSITY_HIGH;
	opts->critical_hp.warn_modes = DANGER_WARN_ALL;
	opts->critical_hp.threshold = 10;

	opts->surrounded.enabled = true;
	opts->surrounded.intensity = DANGER_INTENSITY_MEDIUM;
	opts->surrounded.warn_modes = DANGER_WARN_BORDER_FLASH
		| DANGER_WARN_MESSAGE_HIGHLIGHT;
	opts->surrounded.threshold = 3;

	opts->powerful_monster.enabled = true;
	opts->powerful_monster.intensity = DANGER_INTENSITY_MEDIUM;
	opts->powerful_monster.warn_modes = DANGER_WARN_BORDER_FLASH
		| DANGER_WARN_MESSAGE_HIGHLIGHT
		| DANGER_WARN_SOUND;
	opts->powerful_monster.threshold = 5;

	opts->dangerous_terrain.enabled = true;
	opts->dangerous_terrain.intensity = DANGER_INTENSITY_LOW;
	opts->dangerous_terrain.warn_modes = DANGER_WARN_MESSAGE_HIGHLIGHT;
	opts->dangerous_terrain.threshold = 2;

	opts->suppress_when_resting = true;
	opts->suppress_when_running = true;
	opts->suppress_when_repeating = false;
	opts->suppress_in_subwindows = true;

	opts->global_intensity_cap = DANGER_INTENSITY_HIGH;
	opts->cooldown_turns = 10;
}

void danger_warnings_init(void)
{
	int i;

	danger_config_set_defaults(&danger_opts);

	current_state.active_types = DANGER_NONE;
	current_state.max_intensity = DANGER_INTENSITY_OFF;
	current_state.last_update_turn = 0;
	current_state.suppressed = false;

	prev_active_types = DANGER_NONE;
	prev_max_intensity = DANGER_INTENSITY_OFF;
	state_changed_flag = false;
	render_pending = false;
	last_sound_turn = 0;
	last_render_turn = 0;
	cached_message_color = COLOUR_WHITE;
	message_color_cached = false;

	for (i = 0; i < 5; i++) {
		last_warn_turn[i] = 0;
	}

	if (powerful_monster_seen_before) {
		mem_free(powerful_monster_seen_before);
	}
	powerful_monster_seen_before = NULL;
	powerful_monster_seen_count = 0;

	if (!event_handler_registered) {
		danger_register_handler();
	}
}

void danger_warnings_free(void)
{
	if (powerful_monster_seen_before) {
		mem_free(powerful_monster_seen_before);
		powerful_monster_seen_before = NULL;
	}
	powerful_monster_seen_count = 0;
}

void danger_warnings_reset_state(void)
{
	current_state.active_types = DANGER_NONE;
	current_state.max_intensity = DANGER_INTENSITY_OFF;
	current_state.suppressed = false;
	prev_active_types = DANGER_NONE;
	prev_max_intensity = DANGER_INTENSITY_OFF;
	state_changed_flag = false;
	render_pending = false;
	cached_message_color = COLOUR_WHITE;
	message_color_cached = false;
}

bool danger_state_changed(void)
{
	return state_changed_flag;
}

void danger_suppress(bool suppress)
{
	current_state.suppressed = suppress;
}

bool danger_is_suppressed(void)
{
	return current_state.suppressed;
}

struct danger_state *danger_get_state(void)
{
	return &current_state;
}

bool danger_is_active(danger_type type)
{
	return (current_state.active_types & (uint32_t)type) != 0;
}

danger_intensity danger_get_max_intensity(void)
{
	if (should_suppress()) return DANGER_INTENSITY_OFF;
	if (current_state.max_intensity > danger_opts.global_intensity_cap) {
		return (danger_intensity)danger_opts.global_intensity_cap;
	}
	return current_state.max_intensity;
}

uint8_t danger_get_active_modes(void)
{
	uint8_t modes = 0;
	uint32_t types = current_state.active_types;
	danger_type all_types[] = {
		DANGER_LOW_HP, DANGER_CRITICAL_HP, DANGER_SURROUNDED,
		DANGER_POWERFUL_MONSTER, DANGER_DANGEROUS_TERRAIN
	};
	size_t i;

	if (should_suppress()) return 0;

	for (i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
		if (types & (uint32_t)all_types[i]) {
			struct danger_config *cfg = get_config_for_type(all_types[i]);
			if (cfg && cfg->enabled) {
				modes |= cfg->warn_modes;
			}
		}
	}

	return modes;
}

const struct danger_warning_options *danger_config_get(void)
{
	return &danger_opts;
}

void danger_config_update(const struct danger_warning_options *opts)
{
	if (!opts) return;
	danger_opts = *opts;
}

bool danger_option_enabled(danger_type type)
{
	struct danger_config *cfg = get_config_for_type(type);
	return cfg ? cfg->enabled : false;
}

uint8_t danger_option_modes(danger_type type)
{
	struct danger_config *cfg = get_config_for_type(type);
	return cfg ? cfg->warn_modes : 0;
}

danger_intensity danger_option_intensity(danger_type type)
{
	struct danger_config *cfg = get_config_for_type(type);
	danger_intensity base;
	if (!cfg) return DANGER_INTENSITY_OFF;
	base = cfg->intensity;
	if (danger_opts.global_intensity_cap < base) {
		return danger_opts.global_intensity_cap;
	}
	return base;
}

void danger_signal_warning(danger_type type, danger_intensity intensity,
	const char *message)
{
	if (should_suppress()) return;
	if (!check_cooldown(type)) return;

	event_signal_message(EVENT_DANGER_WARNING, (int)type,
		message ? message : "");

	mark_warn_time(type);
}

void danger_check_all(void)
{
	uint32_t active = DANGER_NONE;
	danger_intensity max_int = DANGER_INTENSITY_OFF;
	bool suppress;

	if (!player || player->is_dead) {
		active = DANGER_NONE;
		max_int = DANGER_INTENSITY_OFF;
	} else {
		if (danger_opts.critical_hp.enabled) {
			int critical_pct = danger_opts.critical_hp.threshold;
			if (player->mhp > 0 && player->chp * 100 <= player->mhp * critical_pct) {
				active |= (uint32_t)DANGER_CRITICAL_HP;
				if (danger_opts.critical_hp.intensity > max_int) {
					max_int = danger_opts.critical_hp.intensity;
				}
			}
		}

		if (danger_opts.low_hp.enabled) {
			int low_pct = danger_opts.low_hp.threshold;
			if (player->mhp > 0 && player->chp * 100 <= player->mhp * low_pct
				&& !(active & (uint32_t)DANGER_CRITICAL_HP)) {
				active |= (uint32_t)DANGER_LOW_HP;
				if (danger_opts.low_hp.intensity > max_int) {
					max_int = danger_opts.low_hp.intensity;
				}
			}
		}

		if (danger_opts.surrounded.enabled) {
			int adj = count_adjacent_visible_monsters();
			if (adj >= danger_opts.surrounded.threshold) {
				active |= (uint32_t)DANGER_SURROUNDED;
				if (danger_opts.surrounded.intensity > max_int) {
					max_int = danger_opts.surrounded.intensity;
				}
			}
		}

		if (danger_opts.powerful_monster.enabled) {
			if (check_powerful_monster_in_view()) {
				active |= (uint32_t)DANGER_POWERFUL_MONSTER;
				if (danger_opts.powerful_monster.intensity > max_int) {
					max_int = danger_opts.powerful_monster.intensity;
				}
			}
		}

		if (danger_opts.dangerous_terrain.enabled) {
			if (check_dangerous_terrain_nearby()) {
				active |= (uint32_t)DANGER_DANGEROUS_TERRAIN;
				if (danger_opts.dangerous_terrain.intensity > max_int) {
					max_int = danger_opts.dangerous_terrain.intensity;
				}
			}
		}
	}

	suppress = should_suppress();

	state_changed_flag = false;

	if (active != prev_active_types || max_int != prev_max_intensity
		|| suppress != current_state.suppressed) {
		state_changed_flag = true;
	}

	prev_active_types = current_state.active_types;
	prev_max_intensity = current_state.max_intensity;

	current_state.active_types = active;
	current_state.max_intensity = max_int;
	current_state.suppressed = suppress;
	current_state.last_update_turn = player ? player->total_energy : 0;

	if (state_changed_flag) {
		uint8_t modes = danger_get_active_modes();
		danger_intensity intensity = danger_get_max_intensity();
		bool any_active = (active != DANGER_NONE) && (intensity > DANGER_INTENSITY_OFF)
			&& (modes != DANGER_WARN_NONE);

		if (suppress || !any_active) {
			cached_message_color = COLOUR_WHITE;
		} else if (modes & DANGER_WARN_MESSAGE_HIGHLIGHT) {
			if (intensity >= DANGER_INTENSITY_HIGH) {
				cached_message_color = COLOUR_RED;
			} else if (intensity >= DANGER_INTENSITY_MEDIUM) {
				cached_message_color = COLOUR_L_RED;
			} else if (intensity >= DANGER_INTENSITY_LOW) {
				cached_message_color = COLOUR_ORANGE;
			} else {
				cached_message_color = COLOUR_WHITE;
			}
		} else {
			cached_message_color = COLOUR_WHITE;
		}
		message_color_cached = true;

		if (!suppress && (any_active || prev_active_types != DANGER_NONE)) {
			event_signal(EVENT_DANGER_WARNING);
			render_pending = true;
		}
	}
}

void danger_sync_from_options(const struct player_options *opts)
{
	uint8_t global_modes = DANGER_WARN_NONE;

	if (!opts) return;

	if (opts->opt[OPT_danger_warn_border])
		global_modes |= DANGER_WARN_BORDER_FLASH;
	if (opts->opt[OPT_danger_warn_statusbar])
		global_modes |= DANGER_WARN_STATUSBAR_EMPHASIS;
	if (opts->opt[OPT_danger_warn_message])
		global_modes |= DANGER_WARN_MESSAGE_HIGHLIGHT;
	if (opts->opt[OPT_danger_warn_sound])
		global_modes |= DANGER_WARN_SOUND;

	danger_opts.low_hp.enabled = opts->opt[OPT_danger_warnings];
	danger_opts.low_hp.warn_modes = global_modes;
	if (opts->danger_low_hp_pct > 0)
		danger_opts.low_hp.threshold = opts->danger_low_hp_pct;

	danger_opts.critical_hp.enabled = opts->opt[OPT_danger_warnings];
	danger_opts.critical_hp.warn_modes = global_modes;
	if (opts->danger_critical_hp_pct > 0)
		danger_opts.critical_hp.threshold = opts->danger_critical_hp_pct;

	danger_opts.surrounded.enabled = opts->opt[OPT_danger_warnings];
	danger_opts.surrounded.warn_modes = global_modes;
	if (opts->danger_surround_count > 0)
		danger_opts.surrounded.threshold = opts->danger_surround_count;

	danger_opts.powerful_monster.enabled = opts->opt[OPT_danger_warnings];
	danger_opts.powerful_monster.warn_modes = global_modes;
	if (opts->danger_mon_level_diff > 0)
		danger_opts.powerful_monster.threshold = opts->danger_mon_level_diff;

	danger_opts.dangerous_terrain.enabled = opts->opt[OPT_danger_warnings];
	danger_opts.dangerous_terrain.warn_modes = global_modes;
	if (opts->danger_terrain_range > 0)
		danger_opts.dangerous_terrain.threshold = opts->danger_terrain_range;

	danger_opts.suppress_when_resting = opts->opt[OPT_danger_suppress_rest];
	danger_opts.suppress_when_running = opts->opt[OPT_danger_suppress_run];

	danger_opts.global_intensity_cap = opts->danger_intensity;
}

void danger_apply_display(void)
{
	danger_intensity intensity;
	uint8_t modes;

	intensity = danger_get_max_intensity();
	modes = danger_get_active_modes();

	if (intensity == DANGER_INTENSITY_OFF || modes == DANGER_WARN_NONE) {
		Term_danger_clear();
		return;
	}

	if (modes & DANGER_WARN_BORDER_FLASH) {
		Term_danger_render(DANGER_RENDER_BORDER, intensity);
	}

	if (modes & DANGER_WARN_STATUSBAR_EMPHASIS) {
		Term_danger_render(DANGER_RENDER_STATUSBAR, intensity);
	}

	if (modes & DANGER_WARN_SOUND) {
		uint16_t cooldown = danger_opts.cooldown_turns > 0 ? danger_opts.cooldown_turns : 10;
		if (turn - last_sound_turn >= cooldown) {
			if (current_state.active_types & DANGER_CRITICAL_HP) {
				sound(MSG_HITPOINT_WARN);
			} else {
				sound(MSG_DANGER_WARNING);
			}
			last_sound_turn = turn;
		}
	}

	last_render_turn = turn;
	render_pending = false;
}

void danger_handle_event(void)
{
	danger_apply_display();
}

static void danger_event_callback(game_event_type event_type, game_event_data *data, void *user)
{
	(void)data;
	(void)user;

	if (event_type == EVENT_DANGER_WARNING) {
		danger_handle_event();
	}
}

void danger_register_handler(void)
{
	if (event_handler_registered) return;

	event_add_handler(EVENT_DANGER_WARNING, danger_event_callback, NULL);
	event_handler_registered = true;
}

uint8_t danger_get_message_color(void)
{
	if (!message_color_cached) {
		uint8_t modes = danger_get_active_modes();
		danger_intensity intensity = danger_get_max_intensity();

		if ((modes & DANGER_WARN_MESSAGE_HIGHLIGHT) && !should_suppress()) {
			if (intensity >= DANGER_INTENSITY_HIGH) {
				cached_message_color = COLOUR_RED;
			} else if (intensity >= DANGER_INTENSITY_MEDIUM) {
				cached_message_color = COLOUR_L_RED;
			} else if (intensity >= DANGER_INTENSITY_LOW) {
				cached_message_color = COLOUR_ORANGE;
			} else {
				cached_message_color = COLOUR_WHITE;
			}
		} else {
			cached_message_color = COLOUR_WHITE;
		}
		message_color_cached = true;
	}

	return cached_message_color;
}
