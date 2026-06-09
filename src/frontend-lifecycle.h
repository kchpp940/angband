/**
 * \file frontend-lifecycle.h
 * \brief Unified frontend initialization lifecycle framework
 *
 * Standardizes the platform frontend initialization sequence across
 * SDL2, Windows, GCU, X11, SDL, and other frontends. Each platform
 * implements adapter functions for each lifecycle stage; the core
 * lifecycle manager ensures stages execute in fixed order and rolls
 * back completed stages on failure.
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

#ifndef INCLUDED_FRONTEND_LIFECYCLE_H
#define INCLUDED_FRONTEND_LIFECYCLE_H

#include "h-basic.h"
#include "angband.h"

/**
 * Lifecycle stages, executed in the order declared here.
 * On failure, all completed stages are rolled back in reverse order.
 */
typedef enum frontend_stage {
	FE_STAGE_PARSE_ARGS = 0,
	FE_STAGE_CAPABILITY,
	FE_STAGE_RESOURCES,
	FE_STAGE_TERMS,
	FE_STAGE_EVENTS,
	FE_STAGE_READY,
	FE_STAGE_MAX
} frontend_stage;

/**
 * Per-stage initialization function.
 * Returns 0 on success, non-zero on failure.
 */
typedef errr (*fe_stage_fn)(int argc, char **argv);

/**
 * Per-stage cleanup/rollback function.
 * Called for every stage whose init function succeeded, in reverse order.
 */
typedef void (*fe_cleanup_fn)(void);

/**
 * Frontend adapter.  Each platform fills in the operations it supports.
 * Any init function may be NULL (treated as always succeeding).
 * Any cleanup function may be NULL (treated as no-op).
 *
 * Ordering contract:
 *   parse_args -> check_capability -> load_resources ->
 *   register_terms -> subscribe_events -> finalize_ready
 *
 * Rollback order on failure after stage N:
 *   cleanup_ready(N/A) <- cleanup_events <- cleanup_terms <-
 *   cleanup_resources <- cleanup_capability <- cleanup_args
 */
struct frontend_adapter {
	const char *name;
	const char *help;
	bool hup_disconnects;
	bool tstp_default;

	fe_stage_fn     init_parse_args;
	fe_stage_fn     init_check_capability;
	fe_stage_fn     init_load_resources;
	fe_stage_fn     init_register_terms;
	fe_stage_fn     init_subscribe_events;
	fe_stage_fn     init_finalize_ready;

	fe_cleanup_fn   cleanup_parse_args;
	fe_cleanup_fn   cleanup_capability;
	fe_cleanup_fn   cleanup_resources;
	fe_cleanup_fn   cleanup_terms;
	fe_cleanup_fn   cleanup_events;
	fe_cleanup_fn   cleanup_ready;

	fe_cleanup_fn   shutdown;
};

/**
 * Result of a frontend_run_lifecycle() call.
 */
struct frontend_lifecycle_result {
	bool success;
	frontend_stage failed_stage;
	errr error_code;
	char error_message[256];
};

/**
 * Execute the full frontend lifecycle using the given adapter.
 * On success returns 0 and records the adapter for the quit hook.
 * On failure rolls back every stage that completed, restores quit_aux
 * to the value it had on entry, and returns a non-zero error code.
 *
 * If @p out_result is non-NULL, a copy of the lifecycle result is
 * written there on return (regardless of success or failure), which
 * avoids reliance on the thread-global last-result store when the
 * caller wants to log or propagate the error immediately.
 */
errr frontend_run_lifecycle(const struct frontend_adapter *adapter,
							int argc, char **argv,
							struct frontend_lifecycle_result *out_result);

/**
 * Return the adapter that was passed to the most recent successful
 * call to frontend_run_lifecycle(), or NULL if none.
 */
const struct frontend_adapter *frontend_get_active_adapter(void);

/**
 * Return the name of a lifecycle stage (for logging).
 */
const char *frontend_stage_name(frontend_stage stage);

/**
 * Return the result of the most recent frontend_run_lifecycle() call.
 * Valid after any call to frontend_run_lifecycle().
 *
 * Note: prefer the @p out_result argument of frontend_run_lifecycle()
 * when you need the result immediately after the call; this accessor
 * is provided for deferred lookup and for adapters that need to inspect
 * the last result from inside a stage function.
 */
const struct frontend_lifecycle_result *frontend_get_last_result(void);

/**
 * Return the last stored error message (empty string on success).
 * Convenience wrapper so callers don't have to dereference the
 * full result struct.
 */
const char *frontend_last_error_message(void);

/**
 * Return the stage at which the last run failed, or FE_STAGE_MAX
 * if the last run succeeded or no run has occurred.
 */
frontend_stage frontend_last_failed_stage(void);

/**
 * Format a human-readable one-line description of the last result
 * into the provided buffer.  Writes something like:
 *   "frontend 'sdl2' failed at stage 'load_resources': font not found"
 * on failure, or
 *   "frontend 'sdl2' initialized successfully"
 * on success.
 *
 * @p adapter_name may be NULL.
 */
void frontend_format_result(char *buf, size_t buf_len,
							const char *adapter_name,
							const struct frontend_lifecycle_result *res);

/**
 * Print the last lifecycle result (or a specific result) to stderr.
 * @p adapter_name may be NULL to omit the adapter name.
 * @p res may be NULL to use the last stored result.
 */
void frontend_print_result(const char *adapter_name,
						   const struct frontend_lifecycle_result *res);

/**
 * Set the error message for the current stage (called by adapter stage
 * implementations so the lifecycle result).  Uses printf-style formatting.
 */
void frontend_set_stage_error(const char *fmt, ...);

#endif /* INCLUDED_FRONTEND_LIFECYCLE_H */
