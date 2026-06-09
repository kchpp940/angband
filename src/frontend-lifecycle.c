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
#include "z-form.h"
#include <stdarg.h>

static const struct frontend_adapter *s_active_adapter = NULL;
static void (*s_saved_quit_aux)(const char *) = NULL;
static struct frontend_lifecycle_result s_last_result;
static frontend_stage s_current_stage = FE_STAGE_MAX;

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
 *
 * Uses an int loop variable to avoid unsigned enum wraparound when
 * last_completed < FE_STAGE_PARSE_ARGS (i.e. no stages completed yet).
 * The bounds check at the top guards against that case explicitly.
 */
static void rollback_stages(const struct frontend_adapter *adapter,
							frontend_stage last_completed)
{
	int i;

	if ((int)last_completed < (int)FE_STAGE_PARSE_ARGS) {
		return;
	}

	for (i = (int)last_completed; i >= (int)FE_STAGE_PARSE_ARGS; i--) {
		fe_cleanup_fn cleanup = NULL;
		frontend_stage s = (frontend_stage)i;

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
							int argc, char **argv,
							struct frontend_lifecycle_result *out_result)
{
	frontend_stage s;
	frontend_stage last_completed;
	errr final_rc;

	memset(&s_last_result, 0, sizeof(s_last_result));
	s_last_result.success = true;
	s_last_result.failed_stage = FE_STAGE_MAX;

	if (!adapter) {
		s_last_result.success = false;
		s_last_result.error_code = -1;
		my_strcpy(s_last_result.error_message,
			"NULL adapter passed to frontend_run_lifecycle",
			sizeof(s_last_result.error_message));
		if (out_result) *out_result = s_last_result;
		return -1;
	}

	s_saved_quit_aux = quit_aux;

	last_completed = (frontend_stage)(FE_STAGE_PARSE_ARGS - 1);
	final_rc = 0;

	for (s = FE_STAGE_PARSE_ARGS; s < FE_STAGE_MAX;
		 s = (frontend_stage)(s + 1)) {
		errr rc;
		s_current_stage = s;
		rc = invoke_stage(adapter, s, argc, argv);
		if (rc != 0) {
			s_last_result.success = false;
			s_last_result.failed_stage = s;
			s_last_result.error_code = rc;
			if (s_last_result.error_message[0] == '\0') {
				strnfmt(s_last_result.error_message,
					sizeof(s_last_result.error_message),
					"Stage '%s' failed with code %d",
					frontend_stage_name(s), (int)rc);
			}
			rollback_stages(adapter, last_completed);
			quit_aux = s_saved_quit_aux;
			s_current_stage = FE_STAGE_MAX;
			final_rc = rc;
			goto done;
		}
		last_completed = s;
	}

	s_current_stage = FE_STAGE_MAX;
	s_active_adapter = adapter;
	quit_aux = lifecycle_quit_hook;
	final_rc = 0;

done:
	if (out_result) *out_result = s_last_result;
	return final_rc;
}

const struct frontend_lifecycle_result *frontend_get_last_result(void)
{
	return &s_last_result;
}

const char *frontend_last_error_message(void)
{
	return s_last_result.error_message;
}

frontend_stage frontend_last_failed_stage(void)
{
	return s_last_result.failed_stage;
}

void frontend_format_result(char *buf, size_t buf_len,
							const char *adapter_name,
							const struct frontend_lifecycle_result *res)
{
	const struct frontend_lifecycle_result *r = res ? res : &s_last_result;
	if (!buf || buf_len == 0) return;

	if (r->success) {
		if (adapter_name) {
			strnfmt(buf, buf_len, "frontend '%s' initialized successfully",
					adapter_name);
		} else {
			strnfmt(buf, buf_len, "frontend initialized successfully");
		}
		return;
	}

	if (adapter_name) {
		if (r->error_message[0] != '\0') {
			strnfmt(buf, buf_len,
				"frontend '%s' failed at stage '%s': %s",
				adapter_name, frontend_stage_name(r->failed_stage),
				r->error_message);
		} else {
			strnfmt(buf, buf_len,
				"frontend '%s' failed at stage '%s' (code %d)",
				adapter_name, frontend_stage_name(r->failed_stage),
				(int)r->error_code);
		}
	} else {
		if (r->error_message[0] != '\0') {
			strnfmt(buf, buf_len,
				"failed at stage '%s': %s",
				frontend_stage_name(r->failed_stage),
				r->error_message);
		} else {
			strnfmt(buf, buf_len,
				"failed at stage '%s' (code %d)",
				frontend_stage_name(r->failed_stage),
				(int)r->error_code);
		}
	}
}

void frontend_print_result(const char *adapter_name,
						   const struct frontend_lifecycle_result *res)
{
	char buf[512];
	frontend_format_result(buf, sizeof(buf), adapter_name, res);
	fprintf(stderr, "%s\n", buf);
}

void frontend_set_stage_error(const char *fmt, ...)
{
	va_list vp;
	if (!fmt) return;
	va_start(vp, fmt);
	vstrnfmt(s_last_result.error_message,
		sizeof(s_last_result.error_message), fmt, vp);
	va_end(vp);
}
