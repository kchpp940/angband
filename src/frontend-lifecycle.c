/**
 * \file frontend-lifecycle.c
 * \brief Unified frontend initialization lifecycle implementation
 *
 * Executes the per-stage init functions in fixed order and, on failure
 * at any stage, rolls back every stage that completed.  Also installs
 * a wrapping quit hook so that adapter->shutdown is reliably invoked
 * when the game exits.
 *
 * Copyright (c) 2026
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

#include "frontend-lifecycle.h"
#include "ui-term.h"
#include "z-util.h"

static const struct frontend_adapter *s_active_adapter = NULL;
static void (*s_saved_quit_aux)(const char *) = NULL;

const char *frontend_stage_name(frontend_stage stage)
{
	switch (stage) {
	case FE_STAGE_PARSE_ARGS:   return "parse_args";
	case FE_STAGE_CAPABILITY:   return "check_capability";
	case FE_STAGE_RESOURCES:    return "load_resources";
	case FE_STAGE_TERMS:        return "register_terms";
	case FE_STAGE_EVENTS:       return "subscribe_events";
	case FE_STAGE_READY:        return "finalize_ready";
	default:                    return "unknown";
	}
}

const struct frontend_adapter *frontend_get_active_adapter(void)
{
	return s_active_adapter;
}

/**
 * Wrapper quit hook.  Installed on successful lifecycle completion so
 * that adapter->shutdown is called before the previously-installed
 * quit hook (saved in s_saved_quit_aux).
 */
static void lifecycle_quit_hook(const char *s)
{
	const struct frontend_adapter *a = s_active_adapter;

	if (a && a->shutdown) {
		a->shutdown();
	}

	if (s_saved_quit_aux) {
		s_saved_quit_aux(s);
	}
}

/**
 * Roll back stages FE_STAGE_PARSE_ARGS .. last_completed (inclusive)
 * in reverse order, calling each stage's cleanup function if present.
 */
static void rollback_stages(const struct frontend_adapter *adapter,
							frontend_stage last_completed)
{
	frontend_stage s;

	for (s = last_completed; s >= FE_STAGE_PARSE_ARGS; s = (frontend_stage)(s - 1)) {
		fe_cleanup_fn cleanup = NULL;

		switch (s) {
		case FE_STAGE_PARSE_ARGS: cleanup = adapter->cleanup_parse_args; break;
		case FE_STAGE_CAPABILITY: cleanup = adapter->cleanup_capability; break;
		case FE_STAGE_RESOURCES:  cleanup = adapter->cleanup_resources;  break;
		case FE_STAGE_TERMS:      cleanup = adapter->cleanup_terms;      break;
		case FE_STAGE_EVENTS:     cleanup = adapter->cleanup_events;     break;
		case FE_STAGE_READY:      cleanup = adapter->cleanup_ready;      break;
		default: break;
		}

		if (cleanup) {
			cleanup();
		}
	}
}

/**
 * Invoke a single stage's init function, if present.  A NULL init
 * function is treated as unconditional success.
 */
static errr invoke_stage(const struct frontend_adapter *adapter,
						 frontend_stage s, int argc, char **argv)
{
	fe_stage_fn init = NULL;

	switch (s) {
	case FE_STAGE_PARSE_ARGS:   init = adapter->init_parse_args;        break;
	case FE_STAGE_CAPABILITY:   init = adapter->init_check_capability;  break;
	case FE_STAGE_RESOURCES:    init = adapter->init_load_resources;    break;
	case FE_STAGE_TERMS:        init = adapter->init_register_terms;    break;
	case FE_STAGE_EVENTS:       init = adapter->init_subscribe_events;  break;
	case FE_STAGE_READY:        init = adapter->init_finalize_ready;    break;
	default: break;
	}

	if (!init) return 0;
	return init(argc, argv);
}

errr frontend_run_lifecycle(const struct frontend_adapter *adapter,
							int argc, char **argv)
{
	frontend_stage s;
	frontend_stage last_completed;

	if (!adapter) return -1;

	s_saved_quit_aux = quit_aux;

	last_completed = (frontend_stage)(FE_STAGE_PARSE_ARGS - 1);

	for (s = FE_STAGE_PARSE_ARGS; s < FE_STAGE_MAX;
		 s = (frontend_stage)(s + 1)) {
		errr rc = invoke_stage(adapter, s, argc, argv);
		if (rc != 0) {
			rollback_stages(adapter, last_completed);
			quit_aux = s_saved_quit_aux;
			return rc;
		}
		last_completed = s;
	}

	s_active_adapter = adapter;
	quit_aux = lifecycle_quit_hook;

	return 0;
}
