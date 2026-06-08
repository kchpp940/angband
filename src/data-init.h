/**
 * \file data-init.h
 * \brief Unified staged data initialization / preflight system
 *
 * Provides a single entry point for game data loading that is shared by
 * the normal game startup (main.c), test front end (main-test.c),
 * unit test utilities (test-utils.c), and spoiler generator (main-spoil.c).
 *
 * Each initialization stage tracks:
 *   - status (pending / in_progress / complete / failed)
 *   - dependencies on earlier stages
 *   - error code and human-readable message on failure
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

#ifndef INCLUDED_DATA_INIT_H
#define INCLUDED_DATA_INIT_H

#include "h-basic.h"

/*
 * Initialization mode.  Controls which stages are executed.
 */
typedef enum {
	DINIT_MODE_FULL = 0,   /* Normal game: every stage */
	DINIT_MODE_TEST,       /* Unit tests: paths + messages + constants + arrays */
	DINIT_MODE_SPOIL       /* Spoiler tool: everything that the spoiler needs */
} dinit_mode_t;

/*
 * Stage identifiers.  Ordered by dependency.
 *
 * These are fine-grained enough that a failure in a specific data file
 * (projection, class, spell, etc.)  can be reported directly instead of
 * surfacing later as a NULL pointer or "resource not found".
 */
typedef enum {
	DINIT_STAGE_NOT_STARTED = 0,

	/* --- Very early core subsystems (no data files, no paths needed) --- */
	DINIT_STAGE_QUARKS,           /* quark string table */

	/* --- Path validation --- */
	DINIT_STAGE_PATHS_CONFIG,     /* config path is set */
	DINIT_STAGE_PATHS_LIB,        /* lib path is set */
	DINIT_STAGE_PATHS_DATA,       /* data / gamedata path is set */

	/* --- Directory capability checks --- */
	DINIT_STAGE_DIRS_GAMEDATA,    /* lib/gamedata is readable */
	DINIT_STAGE_DIRS_USER,        /* user dir exists / is writable */
	DINIT_STAGE_DIRS_SAVE,        /* save dir exists / is writable */
	DINIT_STAGE_DIRS_SCORES,      /* scores dir exists */
	DINIT_STAGE_DIRS_ARCHIVE,     /* archive dir exists */
	DINIT_STAGE_DIRS_PANIC,       /* panic dir exists */

	/* --- Core subsystems (no data files) --- */
	DINIT_STAGE_MESSAGES,         /* message type table */

	/* --- Critical data files tracked individually --- */
	DINIT_STAGE_CONSTANTS,        /* constants.txt -> z_info */
	DINIT_STAGE_WORLD,            /* world.txt */
	DINIT_STAGE_PROJECTIONS,      /* projection.txt */
	DINIT_STAGE_UI_ENTRY_RENDER,  /* ui entry renderers */
	DINIT_STAGE_UI_ENTRIES,       /* ui entries */
	DINIT_STAGE_PLAYER_PROPS,     /* player properties */
	DINIT_STAGE_FEATURES,         /* terrain features */
	DINIT_STAGE_OBJECT_BASES,     /* object base kinds */
	DINIT_STAGE_SLAYS,            /* slay table */
	DINIT_STAGE_BRANDS,           /* brand table */
	DINIT_STAGE_MON_PAIN,         /* monster pain messages */
	DINIT_STAGE_MON_BASES,        /* monster base races */
	DINIT_STAGE_SUMMONS,          /* summon table */
	DINIT_STAGE_CURSES,           /* curse table */
	DINIT_STAGE_SHAPES,           /* player shapes */
	DINIT_STAGE_OBJECTS,          /* object kinds (object.txt) */
	DINIT_STAGE_ACTIVATIONS,      /* object activations */
	DINIT_STAGE_EGO_ITEMS,        /* ego item kinds */
	DINIT_STAGE_HISTORY,          /* player history charts */
	DINIT_STAGE_BODIES,           /* monster bodies */
	DINIT_STAGE_PLAYER_RACES,     /* player races */
	DINIT_STAGE_REALMS,           /* magic realms */
	DINIT_STAGE_PLAYER_CLASSES,   /* player classes (including spells!) */
	DINIT_STAGE_ARTIFACTS,        /* artifact definitions */
	DINIT_STAGE_OBJ_PROPERTIES,   /* object power calculation properties */
	DINIT_STAGE_TIMED_EFFECTS,    /* player timed effects */
	DINIT_STAGE_BLOW_METHODS,     /* monster blow methods */
	DINIT_STAGE_BLOW_EFFECTS,     /* monster blow effects */
	DINIT_STAGE_MON_SPELLS,       /* monster spells */
	DINIT_STAGE_MONSTERS,         /* monster races */
	DINIT_STAGE_MON_PITS,         /* monster pits */
	DINIT_STAGE_MON_LORE,         /* monster lore defaults */
	DINIT_STAGE_TRAPS,            /* traps */
	DINIT_STAGE_CHEST_TRAPS,      /* chest traps */
	DINIT_STAGE_QUESTS,           /* quests */
	DINIT_STAGE_FLAVOURS,         /* object flavours */
	DINIT_STAGE_HINTS,            /* hints */
	DINIT_STAGE_RANDOM_NAMES,     /* random name components */

	/* --- UI visuals (load after constants) --- */
	DINIT_STAGE_VISUALS,          /* ui visuals module */

	/* --- Post-array subsystems --- */
	DINIT_STAGE_PLAYER,           /* player module */
	DINIT_STAGE_GENERATE,         /* dungeon generation */
	DINIT_STAGE_RUNES,            /* rune system */
	DINIT_STAGE_OBJ_MAKE,         /* object creation */
	DINIT_STAGE_IGNORE,           /* object ignore */
	DINIT_STAGE_MON_MAKE,         /* monster creation */
	DINIT_STAGE_STORE,            /* store subsystem */
	DINIT_STAGE_OPTIONS,          /* options */
	DINIT_STAGE_UI_PLAYER,        /* UI player display */
	DINIT_STAGE_UI_EQUIP_CMP,     /* UI equipment comparison */
	DINIT_STAGE_LISTS,            /* monster / object lists */
	DINIT_STAGE_RNG,              /* RNG init */

	DINIT_STAGE_COMPLETE,         /* all stages done */

	DINIT_STAGE_MAX
} dinit_stage_t;

/*
 * Stage execution status.
 */
typedef enum {
	DINIT_STATUS_PENDING = 0,
	DINIT_STATUS_IN_PROGRESS,
	DINIT_STATUS_COMPLETE,
	DINIT_STATUS_FAILED
} dinit_status_t;

/*
 * Information for a single stage.
 */
#define DINIT_MAX_DEPS       4
#define DINIT_ERRMSG_LEN     384
#define DINIT_DATAFILE_LEN   64

struct dinit_stage_info {
	dinit_stage_t  stage;
	const char    *name;
	const char    *description;
	const char    *datafile;       /* e.g. "projection.txt", may be NULL */
	dinit_status_t status;
	errr           error_code;
	char           error_message[DINIT_ERRMSG_LEN];
	int            dependency_count;
	dinit_stage_t  dependencies[DINIT_MAX_DEPS];
};

/*
 * Full result of a data initialization run.
 */
#define DINIT_SUMMARY_LEN  768

struct dinit_result {
	bool                 success;
	dinit_mode_t         mode;
	dinit_stage_t        failed_stage;
	char                 error_summary[DINIT_SUMMARY_LEN];
	struct dinit_stage_info stages[DINIT_STAGE_MAX];
};

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

/* Lifecycle */
extern struct dinit_result *dinit_create_result(void);
extern void                  dinit_destroy_result(struct dinit_result *r);
extern void                  dinit_reset_result(struct dinit_result *r);

/* Introspection helpers */
extern const char *dinit_stage_name(dinit_stage_t s);
extern const char *dinit_status_name(dinit_status_t s);
extern bool        dinit_stage_done(struct dinit_result *r, dinit_stage_t s);
extern bool        dinit_deps_met(struct dinit_result *r, dinit_stage_t s);

/* Recording errors / state */
extern void dinit_set_status(struct dinit_result *r,
                              dinit_stage_t s, dinit_status_t status);
extern void dinit_record_error(struct dinit_result *r, dinit_stage_t s,
                                errr code, const char *fmt, ...);

/* The single shared entry point used by every binary */
extern bool dinit_run(struct dinit_result *r, dinit_mode_t mode);

/* Pretty print the stage report (to plog / stdout) */
extern void dinit_print_report(struct dinit_result *r);

/* Path helpers – feed externally-derived paths into the result. */
extern void dinit_mark_paths_ready(struct dinit_result *r);

/* Access the global result pointer (used by parser-level hooks). */
extern struct dinit_result *dinit_global(void);
extern void                  dinit_set_global(struct dinit_result *r);
extern void                  dinit_clear_global(void);

/* Backwards-compatible convenience wrappers (used by old call sites). */
extern bool dinit_run_full(struct dinit_result *r);
extern bool dinit_run_test(struct dinit_result *r);
extern bool dinit_run_spoil(struct dinit_result *r);

/*
 * Parser-level hook called from init_arrays() for each pl[] entry.
 * Runs run_parser() and records the per-stage result on the current
 * dinit_global() object (if any).  Returns the errr from run_parser().
 */
struct file_parser;
extern errr dinit_run_parser(const char *parser_name, struct file_parser *parser);

#endif /* INCLUDED_DATA_INIT_H */
