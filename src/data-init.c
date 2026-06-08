/**
 * \file data-init.c
 * \brief Unified staged data initialization / preflight system
 *
 * See data-init.h for design overview.  This implementation:
 *
 *   1. Provides a single entry point (dinit_run) shared by main.c,
 *      main-test.c, test-utils.c and main-spoil.c.
 *   2. Tracks every data file individually (paths, constants.txt,
 *      projection.txt, class.txt / spells, monster.txt, ...) so
 *      that a failure in any of them is reported immediately with
 *      the data file name, dependency stage, and parser error.
 *   3. Hooks into the existing init_arrays() pl[] loop so each of
 *      the ~36 parsers logs success/failure into its own stage.
 */

#include "angband.h"
#include "datafile.h"
#include "data-init.h"
#include "game-event.h"
#include "init.h"
#include "message.h"
#include "mon-list.h"
#include "obj-init.h"
#include "obj-list.h"
#include "ui-visuals.h"
#include "z-file.h"
#include "z-util.h"
#include "z-virt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================== */
/* Global state                                                          */
/* ===================================================================== */

static struct dinit_result *g_dinit = NULL;

struct dinit_result *dinit_global(void)       { return g_dinit; }
void                  dinit_set_global(struct dinit_result *r) { g_dinit = r; }
void                  dinit_clear_global(void)                  { g_dinit = NULL; }

/* Forward declaration for the parser-name -> stage lookup table. */
static dinit_stage_t dinit_parser_to_stage(const char *parser_name);

/* ===================================================================== */
/* Stage metadata – name, description, data file, dependencies          */
/* ===================================================================== */

#define DINIT_ADD_DEP(info, dep) do { \
		if ((info)->dependency_count < DINIT_MAX_DEPS) { \
			(info)->dependencies[(info)->dependency_count++] = (dep); \
		} \
	} while (0)

static void dinit_setup_defaults(struct dinit_result *r)
{
	int i;

	for (i = 0; i < DINIT_STAGE_MAX; i++) {
		r->stages[i].stage            = (dinit_stage_t)i;
		r->stages[i].status           = DINIT_STATUS_PENDING;
		r->stages[i].error_code       = 0;
		r->stages[i].error_message[0] = '\0';
		r->stages[i].dependency_count = 0;
		r->stages[i].name             = NULL;
		r->stages[i].description      = NULL;
		r->stages[i].datafile         = NULL;
	}

	/* ----- Quark string table – first, everything else may use it ----- */
	r->stages[DINIT_STAGE_QUARKS].name        = "quarks";
	r->stages[DINIT_STAGE_QUARKS].description = "Interned quark string table";

	/* ----- Path validation ----- */
	r->stages[DINIT_STAGE_PATHS_CONFIG].name        = "paths_config";
	r->stages[DINIT_STAGE_PATHS_CONFIG].description = "Config directory path set";
	r->stages[DINIT_STAGE_PATHS_LIB].name            = "paths_lib";
	r->stages[DINIT_STAGE_PATHS_LIB].description     = "Library root path set";
	r->stages[DINIT_STAGE_PATHS_DATA].name           = "paths_data";
	r->stages[DINIT_STAGE_PATHS_DATA].description    = "Gamedata path set";

	/* ----- Directory capability checks depend on PATHS_DATA ----- */
	#define DINIT_DIR_INIT(stage, id, desc, dep) \
		r->stages[stage].name        = id; \
		r->stages[stage].description = desc; \
		DINIT_ADD_DEP(&r->stages[stage], dep)

	DINIT_DIR_INIT(DINIT_STAGE_DIRS_GAMEDATA, "dirs_gamedata",
	               "lib/gamedata is readable",     DINIT_STAGE_PATHS_DATA);
	DINIT_DIR_INIT(DINIT_STAGE_DIRS_USER,     "dirs_user",
	               "User dir exists / writable",  DINIT_STAGE_PATHS_DATA);
	DINIT_DIR_INIT(DINIT_STAGE_DIRS_SAVE,     "dirs_save",
	               "Save dir exists / writable",  DINIT_STAGE_PATHS_DATA);
	DINIT_DIR_INIT(DINIT_STAGE_DIRS_SCORES,   "dirs_scores",
	               "Scores dir exists",           DINIT_STAGE_PATHS_DATA);
	DINIT_DIR_INIT(DINIT_STAGE_DIRS_ARCHIVE,  "dirs_archive",
	               "Archive dir exists",          DINIT_STAGE_PATHS_DATA);
	DINIT_DIR_INIT(DINIT_STAGE_DIRS_PANIC,    "dirs_panic",
	               "Panic dir exists",            DINIT_STAGE_PATHS_DATA);

	/* ----- Messages module – needs paths (ANGBAND_DIR_GAMEDATA) ----- */
	r->stages[DINIT_STAGE_MESSAGES].name        = "messages";
	r->stages[DINIT_STAGE_MESSAGES].description = "Message type table initialised";
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_MESSAGES], DINIT_STAGE_DIRS_GAMEDATA);

	/* ----- constants.txt – needs messages for event reporting ----- */
	r->stages[DINIT_STAGE_CONSTANTS].name        = "constants";
	r->stages[DINIT_STAGE_CONSTANTS].description = "Game constants (constants.txt)";
	r->stages[DINIT_STAGE_CONSTANTS].datafile    = "constants.txt";
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_CONSTANTS], DINIT_STAGE_MESSAGES);
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_CONSTANTS], DINIT_STAGE_DIRS_GAMEDATA);

	/*
	 * Individual parser stages – all depend on CONSTANTS because they
	 * may reference z_info fields (max array sizes etc.) and on
	 * DIRS_GAMEDATA because the files live there.
	 */
	#define DINIT_PARSER_INIT(stage, id, desc, fname) \
		r->stages[stage].name        = id; \
		r->stages[stage].description = desc; \
		r->stages[stage].datafile    = fname; \
		DINIT_ADD_DEP(&r->stages[stage], DINIT_STAGE_CONSTANTS); \
		DINIT_ADD_DEP(&r->stages[stage], DINIT_STAGE_DIRS_GAMEDATA)

	DINIT_PARSER_INIT(DINIT_STAGE_WORLD,          "world",
	                  "Dungeon / town world data",   "world.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_PROJECTIONS,    "projections",
	                  "Projection types",             "projection.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_UI_ENTRY_RENDER, "ui_entry_render",
	                  "UI entry renderers",          NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_UI_ENTRIES,      "ui_entries",
	                  "UI entries",                   NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_PLAYER_PROPS,    "player_props",
	                  "Player properties",            NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_FEATURES,        "features",
	                  "Terrain features",             "terrain.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_OBJECT_BASES,    "object_bases",
	                  "Object base kinds",            "object_base.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_SLAYS,           "slays",
	                  "Slay table",                   "slay.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_BRANDS,          "brands",
	                  "Brand table",                  "brand.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_MON_PAIN,        "mon_pain",
	                  "Monster pain messages",        "pain.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_MON_BASES,       "mon_bases",
	                  "Monster base races",           "monster_base.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_SUMMONS,         "summons",
	                  "Summon table",                 NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_CURSES,          "curses",
	                  "Curse table",                  NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_SHAPES,          "shapes",
	                  "Player shape table",           "shape.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_OBJECTS,         "objects",
	                  "Object kinds",                 "object.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_ACTIVATIONS,     "activations",
	                  "Object activations",           "activation.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_EGO_ITEMS,       "ego_items",
	                  "Ego item kinds",               "ego_item.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_HISTORY,         "history",
	                  "Player history charts",        "history.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_BODIES,          "bodies",
	                  "Monster bodies",               "body.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_PLAYER_RACES,    "player_races",
	                  "Player races",                 "race.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_REALMS,          "realms",
	                  "Magic realms",                 "realm.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_PLAYER_CLASSES,  "player_classes",
	                  "Player classes (incl. spells)","class.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_ARTIFACTS,       "artifacts",
	                  "Artifact definitions",         "artifact.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_OBJ_PROPERTIES,  "obj_properties",
	                  "Object power properties",      "obj-properties.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_TIMED_EFFECTS,   "timed_effects",
	                  "Player timed effects",         NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_BLOW_METHODS,    "blow_methods",
	                  "Monster blow methods",         NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_BLOW_EFFECTS,    "blow_effects",
	                  "Monster blow effects",         NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_MON_SPELLS,      "mon_spells",
	                  "Monster spells",               "mon-spell.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_MONSTERS,        "monsters",
	                  "Monster races",                "monster.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_MON_PITS,        "mon_pits",
	                  "Monster pits",                 "pit.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_MON_LORE,        "mon_lore",
	                  "Monster lore defaults",        "lore.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_TRAPS,           "traps",
	                  "Traps",                        "trap.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_CHEST_TRAPS,     "chest_traps",
	                  "Chest traps",                  NULL);
	DINIT_PARSER_INIT(DINIT_STAGE_QUESTS,          "quests",
	                  "Quests",                       "quest.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_FLAVOURS,        "flavours",
	                  "Object flavours",              "flavor.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_HINTS,           "hints",
	                  "Hints",                        "hints.txt");
	DINIT_PARSER_INIT(DINIT_STAGE_RANDOM_NAMES,    "random_names",
	                  "Random name components",       "names.txt");

	/* UI visuals module needs constants (for sizes) and arrays loaded */
	r->stages[DINIT_STAGE_VISUALS].name        = "visuals";
	r->stages[DINIT_STAGE_VISUALS].description = "UI visuals / rendering data";
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_VISUALS], DINIT_STAGE_CONSTANTS);
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_VISUALS], DINIT_STAGE_OBJECT_BASES);

	/* Post-array subsystems – each depends on the whole array set */
	#define DINIT_POST_INIT(stage, id, desc) \
		r->stages[stage].name        = id; \
		r->stages[stage].description = desc; \
		DINIT_ADD_DEP(&r->stages[stage], DINIT_STAGE_RANDOM_NAMES)

	DINIT_POST_INIT(DINIT_STAGE_PLAYER,       "player",       "Player subsystem");
	DINIT_POST_INIT(DINIT_STAGE_GENERATE,     "generate",     "Dungeon generation subsystem");
	DINIT_POST_INIT(DINIT_STAGE_RUNES,        "runes",        "Rune subsystem");
	DINIT_POST_INIT(DINIT_STAGE_OBJ_MAKE,     "obj_make",     "Object creation subsystem");
	DINIT_POST_INIT(DINIT_STAGE_IGNORE,       "ignore",       "Object ignore subsystem");
	DINIT_POST_INIT(DINIT_STAGE_MON_MAKE,     "mon_make",     "Monster creation subsystem");
	DINIT_POST_INIT(DINIT_STAGE_STORE,        "store",        "Store subsystem");
	DINIT_POST_INIT(DINIT_STAGE_OPTIONS,      "options",      "Game options subsystem");
	DINIT_POST_INIT(DINIT_STAGE_LISTS,        "lists",        "Monster / object lists");

	r->stages[DINIT_STAGE_UI_PLAYER].name        = "ui_player";
	r->stages[DINIT_STAGE_UI_PLAYER].description = "UI player display";
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_UI_PLAYER], DINIT_STAGE_PLAYER);

	r->stages[DINIT_STAGE_UI_EQUIP_CMP].name        = "ui_equip_cmp";
	r->stages[DINIT_STAGE_UI_EQUIP_CMP].description = "UI equipment comparison";
	DINIT_ADD_DEP(&r->stages[DINIT_STAGE_UI_EQUIP_CMP], DINIT_STAGE_PLAYER);

	r->stages[DINIT_STAGE_RNG].name        = "rng";
	r->stages[DINIT_STAGE_RNG].description = "Random number generator seeded";

	r->stages[DINIT_STAGE_COMPLETE].name        = "complete";
	r->stages[DINIT_STAGE_COMPLETE].description = "All initialization stages complete";
}

#undef DINIT_ADD_DEP
#undef DINIT_DIR_INIT
#undef DINIT_PARSER_INIT
#undef DINIT_POST_INIT

/* ===================================================================== */
/* Lifecycle                                                             */
/* ===================================================================== */

struct dinit_result *dinit_create_result(void)
{
	struct dinit_result *r = mem_zalloc(sizeof(*r));
	r->success               = false;
	r->mode                  = DINIT_MODE_FULL;
	r->failed_stage          = DINIT_STAGE_NOT_STARTED;
	r->error_summary[0]      = '\0';
	dinit_setup_defaults(r);
	return r;
}

void dinit_destroy_result(struct dinit_result *r)
{
	if (r) {
		if (g_dinit == r) g_dinit = NULL;
		mem_free(r);
	}
}

void dinit_reset_result(struct dinit_result *r)
{
	if (!r) return;
	r->success          = false;
	r->failed_stage     = DINIT_STAGE_NOT_STARTED;
	r->error_summary[0] = '\0';
	dinit_setup_defaults(r);
}

/* ===================================================================== */
/* Introspection helpers                                                 */
/* ===================================================================== */

const char *dinit_stage_name(dinit_stage_t s)
{
	switch (s) {
#define CASE(x) case DINIT_STAGE_##x: return #x
		CASE(NOT_STARTED);
		CASE(QUARKS);
		CASE(PATHS_CONFIG); CASE(PATHS_LIB); CASE(PATHS_DATA);
		CASE(DIRS_GAMEDATA); CASE(DIRS_USER); CASE(DIRS_SAVE);
		CASE(DIRS_SCORES);   CASE(DIRS_ARCHIVE); CASE(DIRS_PANIC);
		CASE(MESSAGES);
		CASE(CONSTANTS); CASE(WORLD); CASE(PROJECTIONS);
		CASE(UI_ENTRY_RENDER); CASE(UI_ENTRIES); CASE(PLAYER_PROPS);
		CASE(FEATURES); CASE(OBJECT_BASES); CASE(SLAYS); CASE(BRANDS);
		CASE(MON_PAIN); CASE(MON_BASES); CASE(SUMMONS); CASE(CURSES);
		CASE(SHAPES); CASE(OBJECTS); CASE(ACTIVATIONS); CASE(EGO_ITEMS);
		CASE(HISTORY); CASE(BODIES); CASE(PLAYER_RACES); CASE(REALMS);
		CASE(PLAYER_CLASSES); CASE(ARTIFACTS); CASE(OBJ_PROPERTIES);
		CASE(TIMED_EFFECTS); CASE(BLOW_METHODS); CASE(BLOW_EFFECTS);
		CASE(MON_SPELLS); CASE(MONSTERS); CASE(MON_PITS); CASE(MON_LORE);
		CASE(TRAPS); CASE(CHEST_TRAPS); CASE(QUESTS); CASE(FLAVOURS);
		CASE(HINTS); CASE(RANDOM_NAMES);
		CASE(VISUALS);
		CASE(PLAYER); CASE(GENERATE); CASE(RUNES); CASE(OBJ_MAKE);
		CASE(IGNORE); CASE(MON_MAKE); CASE(STORE); CASE(OPTIONS);
		CASE(UI_PLAYER); CASE(UI_EQUIP_CMP); CASE(LISTS); CASE(RNG);
		CASE(COMPLETE);
#undef CASE
		default: return "unknown";
	}
}

const char *dinit_status_name(dinit_status_t s)
{
	switch (s) {
		case DINIT_STATUS_PENDING:     return "pending";
		case DINIT_STATUS_IN_PROGRESS: return "in_progress";
		case DINIT_STATUS_COMPLETE:    return "complete";
		case DINIT_STATUS_FAILED:      return "failed";
		default:                        return "unknown";
	}
}

bool dinit_stage_done(struct dinit_result *r, dinit_stage_t s)
{
	if (!r || s <= DINIT_STAGE_NOT_STARTED || s >= DINIT_STAGE_MAX)
		return false;
	return r->stages[s].status == DINIT_STATUS_COMPLETE;
}

bool dinit_deps_met(struct dinit_result *r, dinit_stage_t s)
{
	int i;
	struct dinit_stage_info *info;

	if (!r || s <= DINIT_STAGE_NOT_STARTED || s >= DINIT_STAGE_MAX)
		return false;

	info = &r->stages[s];
	for (i = 0; i < info->dependency_count; i++) {
		if (r->stages[info->dependencies[i]].status != DINIT_STATUS_COMPLETE)
			return false;
	}
	return true;
}

/* ===================================================================== */
/* State / error recording                                               */
/* ===================================================================== */

void dinit_set_status(struct dinit_result *r,
                       dinit_stage_t s, dinit_status_t status)
{
	if (!r || s <= DINIT_STAGE_NOT_STARTED || s >= DINIT_STAGE_MAX)
		return;
	r->stages[s].status = status;
}

void dinit_record_error(struct dinit_result *r, dinit_stage_t s,
                         errr code, const char *fmt, ...)
{
	va_list args;
	struct dinit_stage_info *info;

	if (!r || s <= DINIT_STAGE_NOT_STARTED || s >= DINIT_STAGE_MAX)
		return;

	info = &r->stages[s];
	info->status     = DINIT_STATUS_FAILED;
	info->error_code = code;

	if (fmt) {
		va_start(args, fmt);
		vsnprintf(info->error_message, sizeof(info->error_message), fmt, args);
		va_end(args);
	}

	r->success      = false;
	r->failed_stage = s;

	if (info->datafile) {
		snprintf(r->error_summary, sizeof(r->error_summary),
			"Stage '%s' (%s, datafile='%s') failed: %s",
			dinit_stage_name(s), info->description ? info->description : "",
			info->datafile, info->error_message);
	} else {
		snprintf(r->error_summary, sizeof(r->error_summary),
			"Stage '%s' (%s) failed: %s",
			dinit_stage_name(s), info->description ? info->description : "",
			info->error_message);
	}
}

/* ===================================================================== */
/* Path helpers                                                          */
/* ===================================================================== */

void dinit_mark_paths_ready(struct dinit_result *r)
{
	if (!r) return;
	r->stages[DINIT_STAGE_PATHS_CONFIG].status = DINIT_STATUS_COMPLETE;
	r->stages[DINIT_STAGE_PATHS_LIB].status    = DINIT_STATUS_COMPLETE;
	r->stages[DINIT_STAGE_PATHS_DATA].status   = DINIT_STATUS_COMPLETE;
}

/* ===================================================================== */
/* Parser-name -> stage lookup (matches init.c:pl[])                     */
/* ===================================================================== */

struct parser_name_map {
	const char    *name;       /* matches pl[i].name  */
	dinit_stage_t  stage;
};

static const struct parser_name_map g_parser_map[] = {
	{ "world",                  DINIT_STAGE_WORLD },
	{ "projections",            DINIT_STAGE_PROJECTIONS },
	{ "ui renderers",           DINIT_STAGE_UI_ENTRY_RENDER },
	{ "ui entries",             DINIT_STAGE_UI_ENTRIES },
	{ "player properties",      DINIT_STAGE_PLAYER_PROPS },
	{ "features",               DINIT_STAGE_FEATURES },
	{ "object bases",           DINIT_STAGE_OBJECT_BASES },
	{ "slays",                  DINIT_STAGE_SLAYS },
	{ "brands",                 DINIT_STAGE_BRANDS },
	{ "monster pain messages",  DINIT_STAGE_MON_PAIN },
	{ "monster bases",          DINIT_STAGE_MON_BASES },
	{ "summons",                DINIT_STAGE_SUMMONS },
	{ "curses",                 DINIT_STAGE_CURSES },
	{ "player shapes",          DINIT_STAGE_SHAPES },
	{ "objects",                DINIT_STAGE_OBJECTS },
	{ "activations",            DINIT_STAGE_ACTIVATIONS },
	{ "ego-items",              DINIT_STAGE_EGO_ITEMS },
	{ "history charts",         DINIT_STAGE_HISTORY },
	{ "bodies",                 DINIT_STAGE_BODIES },
	{ "player races",           DINIT_STAGE_PLAYER_RACES },
	{ "magic realms",           DINIT_STAGE_REALMS },
	{ "player classes",         DINIT_STAGE_PLAYER_CLASSES },
	{ "artifacts",              DINIT_STAGE_ARTIFACTS },
	{ "object properties",      DINIT_STAGE_OBJ_PROPERTIES },
	{ "timed effects",          DINIT_STAGE_TIMED_EFFECTS },
	{ "blow methods",           DINIT_STAGE_BLOW_METHODS },
	{ "blow effects",           DINIT_STAGE_BLOW_EFFECTS },
	{ "monster spells",         DINIT_STAGE_MON_SPELLS },
	{ "monsters",               DINIT_STAGE_MONSTERS },
	{ "monster pits",           DINIT_STAGE_MON_PITS },
	{ "monster lore",           DINIT_STAGE_MON_LORE },
	{ "traps",                  DINIT_STAGE_TRAPS },
	{ "chest_traps",            DINIT_STAGE_CHEST_TRAPS },
	{ "quests",                 DINIT_STAGE_QUESTS },
	{ "flavours",               DINIT_STAGE_FLAVOURS },
	{ "hints",                  DINIT_STAGE_HINTS },
	{ "random names",           DINIT_STAGE_RANDOM_NAMES },
	{ NULL, DINIT_STAGE_NOT_STARTED }
};

static dinit_stage_t dinit_parser_to_stage(const char *parser_name)
{
	int i;
	if (!parser_name) return DINIT_STAGE_NOT_STARTED;
	for (i = 0; g_parser_map[i].name; i++) {
		if (streq(g_parser_map[i].name, parser_name))
			return g_parser_map[i].stage;
	}
	return DINIT_STAGE_NOT_STARTED;
}

/**
 * Run a single datafile parser, updating the stage record on the
 * current dinit_global() result (if any).
 *
 * Returns the errr from run_parser().  On failure, also marks the
 * corresponding stage failed with a descriptive error.
 *
 * This is called from the patched init_arrays() in init.c.
 */
errr dinit_run_parser(const char *parser_name, struct file_parser *parser)
{
	errr           rc;
	dinit_stage_t  s = dinit_parser_to_stage(parser_name);
	struct dinit_result *r = g_dinit;

	if (r && s > DINIT_STAGE_NOT_STARTED && s < DINIT_STAGE_MAX) {
		r->stages[s].status = DINIT_STATUS_IN_PROGRESS;
	}

	rc = run_parser(parser);

	if (r && s > DINIT_STAGE_NOT_STARTED && s < DINIT_STAGE_MAX) {
		if (rc == 0) {
			r->stages[s].status = DINIT_STATUS_COMPLETE;
		} else {
			const char *err_desc =
				(rc > 0 && rc < PARSE_ERROR_MAX) ? parser_error_str[rc]
				                                 : "unspecified error";
			dinit_record_error(r, s, rc,
				"Parser '%s' returned error code %d (%s)",
				parser_name, (int)rc, err_desc);
		}
	}

	return rc;
}

/* ===================================================================== */
/* Stage execution                                                       */
/* ===================================================================== */

static bool dinit_exec_module(struct dinit_result *r, dinit_stage_t s,
                               struct init_module *mod)
{
	if (!mod || !mod->init) return true;
	mod->init();
	return true;
}

static bool dinit_run_one(struct dinit_result *r, dinit_stage_t s)
{
	struct dinit_stage_info *info;

	if (!r || s <= DINIT_STAGE_NOT_STARTED || s >= DINIT_STAGE_MAX)
		return false;

	info = &r->stages[s];
	if (info->status == DINIT_STATUS_COMPLETE) return true;
	if (info->status == DINIT_STATUS_FAILED)   return false;

	if (!dinit_deps_met(r, s)) {
		dinit_record_error(r, s, PARSE_ERROR_GENERIC,
			"Missing required dependencies for stage '%s'", info->name);
		return false;
	}

	info->status = DINIT_STATUS_IN_PROGRESS;

	switch (s) {
	/* --- Quark string table – runs first, no dependencies --- */
	case DINIT_STAGE_QUARKS:
		if (!dinit_exec_module(r, s, &z_quark_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;

	/* --- path stages are externally driven (mark_paths_ready) --- */
	case DINIT_STAGE_PATHS_CONFIG:
	case DINIT_STAGE_PATHS_LIB:
	case DINIT_STAGE_PATHS_DATA:
		info->status = DINIT_STATUS_COMPLETE;
		break;

	/* --- directory capability checks --- */
	case DINIT_STAGE_DIRS_GAMEDATA:
		if (!ANGBAND_DIR_GAMEDATA || !dir_exists(ANGBAND_DIR_GAMEDATA)) {
			dinit_record_error(r, s, PARSE_ERROR_GENERIC,
				"Gamedata directory not found or not readable: '%s'",
				ANGBAND_DIR_GAMEDATA ? ANGBAND_DIR_GAMEDATA : "(null)");
			return false;
		}
		info->status = DINIT_STATUS_COMPLETE;
		break;

	case DINIT_STAGE_DIRS_USER:
	case DINIT_STAGE_DIRS_SAVE:
	case DINIT_STAGE_DIRS_SCORES:
	case DINIT_STAGE_DIRS_ARCHIVE:
	case DINIT_STAGE_DIRS_PANIC:
		/* create_needed_dirs() takes care of all of them together */
		info->status = DINIT_STATUS_COMPLETE;
		break;

	/* --- core subsystems --- */
	case DINIT_STAGE_MESSAGES:
		if (!dinit_exec_module(r, s, &messages_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;

	case DINIT_STAGE_CONSTANTS:
		init_game_constants();
		info->status = DINIT_STATUS_COMPLETE;
		break;

	/*
	 * The array-data stages are all driven from init_arrays() which
	 * uses dinit_run_parser() to mark each one complete/failed.  We
	 * treat DINIT_STAGE_WORLD as a sentinel:  when the caller reaches
	 * it, init_arrays() has already been invoked and every parser
	 * stage has been updated individually.
	 */
	case DINIT_STAGE_WORLD:
	case DINIT_STAGE_PROJECTIONS:
	case DINIT_STAGE_UI_ENTRY_RENDER:
	case DINIT_STAGE_UI_ENTRIES:
	case DINIT_STAGE_PLAYER_PROPS:
	case DINIT_STAGE_FEATURES:
	case DINIT_STAGE_OBJECT_BASES:
	case DINIT_STAGE_SLAYS:
	case DINIT_STAGE_BRANDS:
	case DINIT_STAGE_MON_PAIN:
	case DINIT_STAGE_MON_BASES:
	case DINIT_STAGE_SUMMONS:
	case DINIT_STAGE_CURSES:
	case DINIT_STAGE_SHAPES:
	case DINIT_STAGE_OBJECTS:
	case DINIT_STAGE_ACTIVATIONS:
	case DINIT_STAGE_EGO_ITEMS:
	case DINIT_STAGE_HISTORY:
	case DINIT_STAGE_BODIES:
	case DINIT_STAGE_PLAYER_RACES:
	case DINIT_STAGE_REALMS:
	case DINIT_STAGE_PLAYER_CLASSES:
	case DINIT_STAGE_ARTIFACTS:
	case DINIT_STAGE_OBJ_PROPERTIES:
	case DINIT_STAGE_TIMED_EFFECTS:
	case DINIT_STAGE_BLOW_METHODS:
	case DINIT_STAGE_BLOW_EFFECTS:
	case DINIT_STAGE_MON_SPELLS:
	case DINIT_STAGE_MONSTERS:
	case DINIT_STAGE_MON_PITS:
	case DINIT_STAGE_MON_LORE:
	case DINIT_STAGE_TRAPS:
	case DINIT_STAGE_CHEST_TRAPS:
	case DINIT_STAGE_QUESTS:
	case DINIT_STAGE_FLAVOURS:
	case DINIT_STAGE_HINTS:
	case DINIT_STAGE_RANDOM_NAMES:
		/* nothing to do here – see init_arrays() + dinit_run_parser() */
		break;

	/* UI visuals module */
	case DINIT_STAGE_VISUALS:
		if (!dinit_exec_module(r, s, &ui_visuals_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;

	/* Post-array subsystems */
	case DINIT_STAGE_PLAYER:
		if (!dinit_exec_module(r, s, &player_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_GENERATE:
		if (!dinit_exec_module(r, s, &generate_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_RUNES:
		if (!dinit_exec_module(r, s, &rune_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_OBJ_MAKE:
		if (!dinit_exec_module(r, s, &obj_make_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_IGNORE:
		if (!dinit_exec_module(r, s, &ignore_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_MON_MAKE:
		if (!dinit_exec_module(r, s, &mon_make_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_STORE:
		if (!dinit_exec_module(r, s, &store_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_OPTIONS:
		if (!dinit_exec_module(r, s, &options_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_UI_PLAYER:
		if (!dinit_exec_module(r, s, &ui_player_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_UI_EQUIP_CMP:
		if (!dinit_exec_module(r, s, &ui_equip_cmp_module)) return false;
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_LISTS:
		monster_list_init();
		object_list_init();
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_RNG:
		Rand_init();
		info->status = DINIT_STATUS_COMPLETE;
		break;
	case DINIT_STAGE_COMPLETE:
		info->status = DINIT_STATUS_COMPLETE;
		break;

	default:
		dinit_record_error(r, s, PARSE_ERROR_GENERIC,
			"Unknown stage id=%d", (int)s);
		return false;
	}

	return true;
}

/* ===================================================================== */
/* Mode – decide which stages to execute                                 */
/* ===================================================================== */

static bool dinit_should_run(dinit_mode_t mode, dinit_stage_t s)
{
	/* Path / dir / messages / constants always run. */
	if (s <= DINIT_STAGE_MESSAGES)             return true;
	if (s == DINIT_STAGE_CONSTANTS)            return true;

	/* Every parser stage (WORLD..RANDOM_NAMES) always runs in any mode. */
	if (s >= DINIT_STAGE_WORLD && s <= DINIT_STAGE_RANDOM_NAMES) return true;

	/* Visuals: not needed for bare unit tests. */
	if (s == DINIT_STAGE_VISUALS) {
		return (mode == DINIT_MODE_FULL || mode == DINIT_MODE_SPOIL);
	}

	/* Post-array subsystems: TEST mode skips almost all. */
	if (mode == DINIT_MODE_TEST) {
		return false;
	}

	/* SPOIL mode needs enough to dump data – most subsystems work. */
	if (mode == DINIT_MODE_SPOIL) {
		if (s == DINIT_STAGE_UI_PLAYER || s == DINIT_STAGE_UI_EQUIP_CMP)
			return false;
		return true;
	}

	/* FULL mode runs everything. */
	return true;
}

/* ===================================================================== */
/* The single shared entry point                                         */
/* ===================================================================== */

bool dinit_run(struct dinit_result *r, dinit_mode_t mode)
{
	dinit_stage_t s;

	if (!r) return false;

	r->mode    = mode;
	r->success = true;

	/* Install as the global result so dinit_run_parser() can see it. */
	if (!g_dinit) g_dinit = r;

	event_signal(EVENT_ENTER_INIT);

	/*
	 * Pass 1 – walk every stage in dependency order.  The array parsers
	 * (WORLD..RANDOM_NAMES) are a no-op individually; init_arrays() is
	 * triggered just before we reach WORLD.
	 */
	for (s = (dinit_stage_t)((int)DINIT_STAGE_NOT_STARTED + 1);
	     s < DINIT_STAGE_COMPLETE;
	     s = (dinit_stage_t)((int)s + 1)) {

		if (!dinit_should_run(mode, s)) continue;

		/* Trigger init_arrays() once, right before the world stage. */
		if (s == DINIT_STAGE_WORLD) {
			init_arrays();
			/* If any parser failed, bail out. */
			if (r->failed_stage != DINIT_STAGE_NOT_STARTED) {
				r->success = false;
				return false;
			}
		}

		if (!dinit_run_one(r, s)) {
			r->success = false;
			return false;
		}
	}

	dinit_set_status(r, DINIT_STAGE_COMPLETE, DINIT_STATUS_COMPLETE);
	r->success = true;
	return true;
}

/* Convenience wrappers */
bool dinit_run_full (struct dinit_result *r) { return dinit_run(r, DINIT_MODE_FULL); }
bool dinit_run_test (struct dinit_result *r) { return dinit_run(r, DINIT_MODE_TEST); }
bool dinit_run_spoil(struct dinit_result *r) { return dinit_run(r, DINIT_MODE_SPOIL); }

/* ===================================================================== */
/* Reporting                                                             */
/* ===================================================================== */

static void dinit_log(const char *line)
{
	if (!line) return;
	/* Prefer plog() when available, fall back to plain printf(). */
#if defined(INCLUDED_Z_FILE_H) || 1
	{
		extern void plog(const char *);
		plog(line);
	}
#else
	printf("%s\n", line);
#endif
}

void dinit_print_report(struct dinit_result *r)
{
	int i;
	char buf[512];

	if (!r) return;

	dinit_log("=== Data Initialization Status Report ===");
	for (i = 0; i < DINIT_STAGE_MAX; i++) {
		struct dinit_stage_info *info = &r->stages[i];

		if (info->status == DINIT_STATUS_PENDING &&
		    info->name == NULL)
			continue;

		if (info->datafile) {
			snprintf(buf, sizeof(buf),
				"  [%-11s] %-18s %-24s file='%s' %s",
				dinit_status_name(info->status),
				dinit_stage_name(info->stage),
				info->name ? info->name : "-",
				info->datafile,
				info->description ? info->description : "");
		} else {
			snprintf(buf, sizeof(buf),
				"  [%-11s] %-18s %-24s %s",
				dinit_status_name(info->status),
				dinit_stage_name(info->stage),
				info->name ? info->name : "-",
				info->description ? info->description : "");
		}
		dinit_log(buf);

		if (info->status == DINIT_STATUS_FAILED && info->error_message[0]) {
			snprintf(buf, sizeof(buf),
				"    ERROR: %s (code=%d)",
				info->error_message, (int)info->error_code);
			dinit_log(buf);
			if (info->dependency_count > 0) {
				int j;
				char depbuf[256] = "";
				char *p = depbuf;
				for (j = 0; j < info->dependency_count; j++) {
					int n = snprintf(p, sizeof(depbuf) - (size_t)(p - depbuf),
						"%s%s", (j > 0) ? ", " : "",
						dinit_stage_name(info->dependencies[j]));
					if (n > 0) p += n;
				}
				snprintf(buf, sizeof(buf),
					"    depends on: %s", depbuf);
				dinit_log(buf);
			}
		}
	}
	if (!r->success && r->error_summary[0]) {
		snprintf(buf, sizeof(buf), "SUMMARY: %s", r->error_summary);
		dinit_log(buf);
	}
	dinit_log("=========================================");
}
