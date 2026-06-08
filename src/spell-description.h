/**
 * \file spell-description.h
 * \brief Unified spell description generation from effect/projection data
 *
 * Produces fully structured spell information.  All text formatting
 * (menu rows, browse detail, confirmation dialogs, help pages) is the
 * caller's responsibility; this header provides helper formatters that
 * read from the structured fields only.
 */

#ifndef SPELL_DESCRIPTION_H
#define SPELL_DESCRIPTION_H

#include "z-dice.h"

struct class_spell;

enum spell_effect_kind {
	SPELL_EFFECT_DAMAGE,
	SPELL_EFFECT_HEAL,
	SPELL_EFFECT_TIMED,
	SPELL_EFFECT_SUMMON,
	SPELL_EFFECT_TELEPORT,
	SPELL_EFFECT_DETECT,
	SPELL_EFFECT_OTHER
};

struct spell_effect_info {
	struct spell_effect_info *next;

	/* ---- Identification (static string references) ---- */
	enum spell_effect_kind kind;
	const char *projection_name;   /* "acid", "fire", ... or ""      */
	const char *timed_name;        /* "confusion", "poison", ... or ""*/
	const char *info_label;        /* Short label: "dam"/"heal"/...   */

	/* ---- Core numeric data ---- */
	int avg_damage;                /* Average damage (0 = not damage) */
	int range;                     /* Tiles of range (0 = N/A)        */
	int radius;                    /* Area radius in tiles (0 = N/A)  */
	random_value dice_rv;          /* Raw dice / random value         */

	/* ---- Explicit parameters (split from former 'extra' text) ---- */
	int beam_length;               /* EF_SHORT_BEAM: length in tiles  */
	int projectile_count;          /* EF_SWARM: number of projectiles */
	int heal_pct_floor;            /* EF_HEAL_HP: min % of max HP     */
	bool teleport_random;          /* EF_TELEPORT: destination random */

	/* ---- Flags ---- */
	bool is_damage;
	bool needs_aim;
};

struct spell_cast_limits {
	int mana;
	int slevel;
	int fail_percent;
};

struct spell_info {
	struct spell_effect_info *effects;
	struct spell_cast_limits limits;
	bool needs_aim;
	const char *name;
	const char *text;
};

/* ---- Construction / destruction ---- */

struct spell_info *spell_info_build(const struct class_spell *spell,
	int spell_index);
void spell_info_free(struct spell_info *info);

/* ---- Utility: format a random_value as a dice expression ---- */

size_t spell_rv_format_dice(const random_value *rv, char *buf, size_t len);

/* ---- Text fragment formatters (operate on structured fields) ---- */

size_t spell_info_format_short(const struct spell_info *info,
	char *buf, size_t len);

size_t spell_info_format_damage(const struct spell_info *info,
	char *buf, size_t len);

size_t spell_info_format_side_effects(const struct spell_info *info,
	char *buf, size_t len);

size_t spell_info_format_limits(const struct spell_info *info,
	char *buf, size_t len);

#endif
