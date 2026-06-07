/* danger/danger.c */

#include "unit-test.h"
#include "unit-test-data.h"
#include "test-utils.h"

#include <stdio.h>
#include "angband.h"
#include "cave.h"
#include "game-event.h"
#include "game-world.h"
#include "generate.h"
#include "init.h"
#include "option.h"
#include "player.h"
#include "player-birth.h"
#include "player-timed.h"
#include "savefile.h"
#include "ui-danger.h"
#include "z-util.h"
#include "z-color.h"

static void println(const char *str) {
	printf("%s\n", str);
}

int setup_tests(void **state) {
	plog_aux = println;

	set_file_paths();
	init_angband();
#ifdef UNIX
	create_needed_dirs();
#endif

	danger_warnings_init();

	return 0;
}

int teardown_tests(void *state) {
	danger_warnings_free();
	if (player) {
		player = NULL;
	}
	cleanup_angband();
	return 0;
}

static int test_config_defaults(void *state) {
	const struct danger_warning_options *opts;

	opts = danger_config_get();
	require(opts != NULL);

	eq(opts->low_hp.enabled, true);
	eq(opts->critical_hp.enabled, true);
	eq(opts->surrounded.enabled, true);
	eq(opts->powerful_monster.enabled, true);
	eq(opts->dangerous_terrain.enabled, true);

	noteq(opts->low_hp.warn_modes, DANGER_WARN_NONE);
	noteq(opts->critical_hp.warn_modes, DANGER_WARN_NONE);

	eq(opts->low_hp.intensity, DANGER_INTENSITY_MEDIUM);
	eq(opts->critical_hp.intensity, DANGER_INTENSITY_HIGH);

	eq(opts->low_hp.threshold, 30);
	eq(opts->critical_hp.threshold, 10);
	eq(opts->surrounded.threshold, 3);
	eq(opts->powerful_monster.threshold, 5);
	eq(opts->dangerous_terrain.threshold, 2);

	eq(opts->suppress_when_resting, true);
	eq(opts->suppress_when_running, true);
	eq(opts->suppress_when_repeating, false);
	eq(opts->suppress_in_subwindows, true);

	eq(opts->global_intensity_cap, DANGER_INTENSITY_HIGH);
	eq(opts->cooldown_turns, 10);

	ok;
}

static int test_state_init_reset(void *state) {
	struct danger_state *st;

	danger_warnings_reset_state();
	st = danger_get_state();
	require(st != NULL);

	eq(st->active_types, DANGER_NONE);
	eq(st->max_intensity, DANGER_INTENSITY_OFF);
	eq(st->suppressed, false);

	eq(danger_is_active(DANGER_LOW_HP), false);
	eq(danger_is_active(DANGER_CRITICAL_HP), false);
	eq(danger_is_active(DANGER_SURROUNDED), false);

	eq(danger_get_max_intensity(), DANGER_INTENSITY_OFF);
	eq(danger_get_active_modes(), DANGER_WARN_NONE);

	ok;
}

static int test_suppress(void *state) {
	danger_warnings_reset_state();

	eq(danger_is_suppressed(), false);

	danger_suppress(true);
	eq(danger_is_suppressed(), true);

	danger_suppress(false);
	eq(danger_is_suppressed(), false);

	ok;
}

static int test_message_color_clear(void *state) {
	uint8_t color;

	danger_warnings_reset_state();

	color = danger_get_message_color();
	eq(color, COLOUR_WHITE);

	danger_suppress(true);
	color = danger_get_message_color();
	eq(color, COLOUR_WHITE);

	danger_suppress(false);
	color = danger_get_message_color();
	eq(color, COLOUR_WHITE);

	ok;
}

static int test_event_handler_registration(void *state) {
	danger_warnings_init();
	ok;
}

static int test_option_enabled(void *state) {
	struct danger_warning_options opts;
	const struct danger_warning_options *cur;

	danger_config_set_defaults(&opts);
	danger_config_update(&opts);

	cur = danger_config_get();
	require(cur != NULL);

	eq(danger_option_enabled(DANGER_LOW_HP), true);
	eq(danger_option_enabled(DANGER_CRITICAL_HP), true);
	eq(danger_option_enabled(DANGER_SURROUNDED), true);
	eq(danger_option_enabled(DANGER_POWERFUL_MONSTER), true);
	eq(danger_option_enabled(DANGER_DANGEROUS_TERRAIN), true);

	opts.low_hp.enabled = false;
	danger_config_update(&opts);
	eq(danger_option_enabled(DANGER_LOW_HP), false);
	eq(danger_option_enabled(DANGER_CRITICAL_HP), true);

	ok;
}

static int test_intensity_cap(void *state) {
	struct danger_warning_options opts;

	danger_config_set_defaults(&opts);

	opts.global_intensity_cap = DANGER_INTENSITY_MEDIUM;
	opts.critical_hp.intensity = DANGER_INTENSITY_HIGH;
	danger_config_update(&opts);

	eq(danger_option_intensity(DANGER_CRITICAL_HP), DANGER_INTENSITY_MEDIUM);

	opts.global_intensity_cap = DANGER_INTENSITY_LOW;
	danger_config_update(&opts);
	eq(danger_option_intensity(DANGER_CRITICAL_HP), DANGER_INTENSITY_LOW);

	opts.global_intensity_cap = DANGER_INTENSITY_OFF;
	danger_config_update(&opts);
	eq(danger_option_intensity(DANGER_CRITICAL_HP), DANGER_INTENSITY_OFF);

	ok;
}

const char *suite_name = "danger/danger";
struct test tests[] = {
	{ "config_defaults", test_config_defaults },
	{ "state_init_reset", test_state_init_reset },
	{ "suppress", test_suppress },
	{ "message_color_clear", test_message_color_clear },
	{ "event_handler_registration", test_event_handler_registration },
	{ "option_enabled", test_option_enabled },
	{ "intensity_cap", test_intensity_cap },
	{ NULL, NULL }
};
