/**
 * \file spell-description.c
 * \brief Unified spell description builder (data only, no text formatting)
 *
 * Single layer: DATA BUILDING.
 *   spell_info_build() walks the effect chain and fills fully-typed
 *   struct fields.  No display strings are produced at this layer.
 *
 * Text formatting (menu rows, browse detail, confirmation prompts, etc.)
 * and rendering (text_out, textblock, file I/O) are entirely the caller's
 * responsibility.  Only a dice-expression utility is provided here since
 * that is a pure data→data transformation independent of display language.
 */

#include "angband.h"
#include "effects.h"
#include "effects-info.h"
#include "init.h"
#include "player-spell.h"
#include "player-timed.h"
#include "project.h"
#include "spell-description.h"
#include "z-form.h"
#include "z-util.h"


/* ========================================================================
 * Internal data mappers (effect → typed field; no text output)
 * ======================================================================== */

static const char *map_timed_idx_to_name(int idx)
{
	if (idx < 0 || idx >= TMD_MAX) return "";
	return timed_effects[idx].name ? timed_effects[idx].name : "";
}


static enum spell_effect_kind map_effect_to_kind(const struct effect *effect)
{
	if (effect_damages(effect)) return SPELL_EFFECT_DAMAGE;
	switch (effect->index) {
	case EF_HEAL_HP:                    return SPELL_EFFECT_HEAL;
	case EF_CURE:
	case EF_TIMED_SET:
	case EF_TIMED_INC:
	case EF_TIMED_INC_NO_RES:
	case EF_TIMED_DEC:                 return SPELL_EFFECT_TIMED;
	case EF_SUMMON:                     return SPELL_EFFECT_SUMMON;
	case EF_TELEPORT:
	case EF_TELEPORT_TO:
	case EF_TELEPORT_LEVEL:             return SPELL_EFFECT_TELEPORT;
	default:
		if (effect->index >= EF_DETECT_TRAPS
			&& effect->index <= EF_DETECT_SOUL) {
			return SPELL_EFFECT_DETECT;
		}
		return SPELL_EFFECT_OTHER;
	}
}


static void map_effect_fill_explicit_params(struct spell_effect_info *ei,
	const struct effect *effect, const random_value *rv)
{
	ei->beam_length = 0;
	ei->projectile_count = 0;
	ei->heal_pct_floor = 0;
	ei->teleport_random = false;

	switch (effect->index) {
	case EF_SHORT_BEAM: {
		int len = effect->radius;
		if (effect->other) {
			len += player->lev / effect->other;
			len = MIN(len, (int)z_info->max_range);
		}
		ei->beam_length = len;
		break;
	}
	case EF_SWARM:
		ei->projectile_count = rv->m_bonus;
		break;
	case EF_HEAL_HP:
		ei->heal_pct_floor = rv->m_bonus;
		break;
	case EF_TELEPORT:
	case EF_TELEPORT_TO:
	case EF_TELEPORT_LEVEL:
		ei->teleport_random = (rv->m_bonus != 0);
		break;
	default:
		break;
	}
}


/* ========================================================================
 * Public — Build structured spell_info from effect chain
 * ======================================================================== */

struct spell_info *spell_info_build(const struct class_spell *spell,
	int spell_index)
{
	struct spell_info *info;
	struct spell_effect_info **link;
	struct effect *effect;
	dice_t *shared_dice = NULL;
	bool have_shared = false;
	random_value shared_rv = { 0, 0, 0, 0 };

	if (!spell) return NULL;

	info = mem_zalloc(sizeof(*info));
	info->limits.mana = spell->smana;
	info->limits.slevel = spell->slevel;
	info->limits.fail_percent = spell_chance(spell_index);
	info->needs_aim = effect_aim(spell->effect);
	info->name = spell->name;
	info->text = spell->text;
	info->effects = NULL;
	link = &info->effects;

	for (effect = spell->effect; effect; effect = effect_next(effect)) {
		struct spell_effect_info *ei;
		random_value rv = { 0, 0, 0, 0 };
		const char *type = effect_info(effect);
		const char *proj;

		if (effect->index == EF_CLEAR_VALUE) {
			have_shared = false;
			shared_dice = NULL;
			continue;
		}
		if (effect->index == EF_SET_VALUE && effect->dice) {
			have_shared = true;
			shared_dice = effect->dice;
			dice_roll(shared_dice, &shared_rv);
			continue;
		}

		ei = mem_zalloc(sizeof(*ei));
		ei->next = NULL;

		if (effect->dice != NULL) {
			dice_roll(effect->dice, &rv);
		} else if (have_shared) {
			rv = shared_rv;
		}
		ei->dice_rv = rv;

		ei->kind = map_effect_to_kind(effect);
		ei->info_label = type ? type : "";
		ei->projection_name = "";
		ei->timed_name = "";
		ei->avg_damage = 0;
		ei->range = 0;
		ei->radius = 0;
		ei->is_damage = false;
		ei->needs_aim = false;

		proj = effect_projection(effect);
		if (proj && strlen(proj) > 0) {
			ei->projection_name = proj;
		}

		switch (ei->kind) {
		case SPELL_EFFECT_DAMAGE:
			ei->is_damage = true;
			ei->needs_aim = true;
			ei->avg_damage = effect_avg_damage(effect,
				have_shared ? shared_dice : NULL);
			ei->range = effect_range(effect);
			ei->radius = effect_radius(effect);
			break;
		case SPELL_EFFECT_TIMED:
			ei->timed_name = map_timed_idx_to_name(effect->subtype);
			break;
		default:
			break;
		}

		map_effect_fill_explicit_params(ei, effect, &rv);

		*link = ei;
		link = &ei->next;
	}

	return info;
}


void spell_info_free(struct spell_info *info)
{
	struct spell_effect_info *ei, *next;

	if (!info) return;
	for (ei = info->effects; ei; ei = next) {
		next = ei->next;
		mem_free(ei);
	}
	mem_free(info);
}


/* ========================================================================
 * Public utility — format a random_value as a dice expression
 *
 * Pure data transformation: random_value → "10+3d6" style string.
 * Not tied to any display language or UI layout.
 * ======================================================================== */

size_t spell_rv_format_dice(const random_value *rv, char *buf, size_t len)
{
	size_t off = 0;
	if (!rv || !buf || len == 0) return 0;
	buf[0] = '\0';

	if (rv->base > 0) {
		off += strnfmt(buf + off, len - off, "%d", rv->base);
		if (rv->dice > 0 && rv->sides > 0) {
			off += strnfmt(buf + off, len - off, "+");
		}
	}
	if (rv->dice == 1 && rv->sides > 0) {
		off += strnfmt(buf + off, len - off, "d%d", rv->sides);
	} else if (rv->dice > 1 && rv->sides > 0) {
		off += strnfmt(buf + off, len - off, "%dd%d", rv->dice, rv->sides);
	}
	return off;
}
