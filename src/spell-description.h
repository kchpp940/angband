/**
 * \file spell-description.h
 * \brief Unified spell description generation from effect/projection data
 *
 * Provides a single source of truth for spell information displayed
 * anywhere in the UI: menu rows, browse details, cast dialogs.
 */

#ifndef SPELL_DESCRIPTION_H
#define SPELL_DESCRIPTION_H

#include "z-dice.h"
#include "z-textblock.h"

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
	enum spell_effect_kind kind;
	const char *projection_name;
	const char *timed_name;
	int avg_damage;
	int range;
	int radius;
	random_value dice_rv;
	char dice_str[32];
	char extra[64];
	const char *info_label;
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

struct spell_info *spell_info_build(const struct class_spell *spell,
	int spell_index);
void spell_info_free(struct spell_info *info);

size_t spell_info_format_short(const struct spell_info *info,
	char *buf, size_t len);

void spell_info_append_detail(const struct spell_info *info,
	textblock *tb, bool include_damage_summary,
	bool include_side_effects, bool include_limits);

void spell_info_text_out_detail(const struct spell_info *info,
	bool include_damage_summary, bool include_side_effects,
	bool include_limits);

#endif
