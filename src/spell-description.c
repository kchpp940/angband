/**
 * \file spell-description.c
 * \brief Unified spell description builder and formatters
 *
 * Two layers, strictly separated:
 *
 *   1. DATA BUILDING — spell_info_build() walks the effect chain and
 *      fills fully-typed struct fields.  No display strings are produced.
 *
 *   2. TEXT FORMATTING — spell_info_format_*() and spell_rv_format_dice()
 *      read the struct fields and produce char-buffer text fragments.
 *      They have no side effects and do not touch globals beyond reading
 *      the provided info.
 *
 * Rendering (text_out, textblock, file I/O) is entirely the caller's job.
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
 * Layer 1 — Internal data mappers (effect → typed field; no text output)
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
 * Layer 1 (public) — Build structured spell_info from effect chain
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
 * Layer 2 — Public text formatters (pure: read struct → char buffer)
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


/* Helper: produce the "extra" display fragment for an effect (rad/len/xN/%) */
static size_t fmt_extra_for_display(const struct spell_effect_info *ei,
	char *buf, size_t len)
{
	size_t off = 0;
	if (!buf || len == 0) return 0;
	buf[0] = '\0';

	if (ei->radius > 0) {
		off += strnfmt(buf + off, len - off, ", rad %d", ei->radius);
	}
	if (ei->beam_length > 0) {
		off += strnfmt(buf + off, len - off, ", len %d", ei->beam_length);
	}
	if (ei->projectile_count > 0) {
		off += strnfmt(buf + off, len - off, "x%d", ei->projectile_count);
	}
	if (ei->heal_pct_floor > 0) {
		off += strnfmt(buf + off, len - off, "/%d%%", ei->heal_pct_floor);
	}
	if (ei->teleport_random) {
		off += strnfmt(buf + off, len - off, "random");
	}
	return off;
}


size_t spell_info_format_short(const struct spell_info *info, char *buf,
	size_t len)
{
	size_t offset = 0;
	struct spell_effect_info *ei;
	struct spell_effect_info *pre = NULL;
	char pre_extra[64] = "";
	random_value pre_rv = { 0, 0, 0, 0 };

	if (!info || !buf || len == 0) return 0;
	buf[0] = '\0';

	for (ei = info->effects; ei; ei = ei->next) {
		random_value rv = ei->dice_rv;
		char dice_buf[32];
		char extra_buf[64];
		bool same_as_prev = false;

		spell_rv_format_dice(&rv, dice_buf, sizeof(dice_buf));
		fmt_extra_for_display(ei, extra_buf, sizeof(extra_buf));

		if (pre && pre->kind == ei->kind
			&& streq(pre_extra, extra_buf)
			&& pre_rv.base == rv.base
			&& pre_rv.dice == rv.dice
			&& pre_rv.sides == rv.sides
			&& pre_rv.m_bonus == rv.m_bonus
			&& streq(pre->info_label, ei->info_label)
			&& streq(pre->projection_name, ei->projection_name)) {
			same_as_prev = true;
		}

		if ((strlen(dice_buf) > 0 || strlen(extra_buf) > 1)
			&& !same_as_prev) {
			if (offset) {
				offset += strnfmt(buf + offset, len - offset, ";");
			}
			offset += strnfmt(buf + offset, len - offset, " %s ",
							  ei->info_label);
			offset += strnfmt(buf + offset, len - offset, "%s",
							  dice_buf);
			if (strlen(extra_buf) > 1) {
				offset += strnfmt(buf + offset, len - offset, "%s",
								  extra_buf);
			}
			pre = ei;
			my_strcpy(pre_extra, extra_buf, sizeof(pre_extra));
			pre_rv = rv;
		}
	}

	return offset;
}


size_t spell_info_format_damage(const struct spell_info *info, char *buf,
	size_t len)
{
	const struct spell_effect_info *ei;
	int num_damaging = 0;
	int i = 0;
	size_t offset = 0;

	if (!info || !buf || len == 0) return 0;
	buf[0] = '\0';

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->is_damage) num_damaging++;
	}
	if (num_damaging == 0) return 0;

	offset += strnfmt(buf + offset, len - offset, "  Inflicts an average of");

	for (ei = info->effects; ei; ei = ei->next) {
		if (!ei->is_damage) continue;
		if (num_damaging > 2 && i > 0) {
			offset += strnfmt(buf + offset, len - offset, ",");
		}
		if (num_damaging > 1 && i == num_damaging - 1) {
			offset += strnfmt(buf + offset, len - offset, " and");
		}
		offset += strnfmt(buf + offset, len - offset, " %d", ei->avg_damage);
		if (strlen(ei->projection_name) > 0) {
			offset += strnfmt(buf + offset, len - offset, " %s",
				ei->projection_name);
		}
		if (ei->radius > 0) {
			offset += strnfmt(buf + offset, len - offset, " (radius %d)",
				ei->radius);
		} else if (ei->range > 0) {
			offset += strnfmt(buf + offset, len - offset, " (range %d)",
				ei->range);
		} else if (ei->beam_length > 0) {
			offset += strnfmt(buf + offset, len - offset, " (length %d)",
				ei->beam_length);
		}
		i++;
	}
	offset += strnfmt(buf + offset, len - offset, " damage.\n");

	return offset;
}


size_t spell_info_format_side_effects(const struct spell_info *info,
	char *buf, size_t len)
{
	const struct spell_effect_info *ei;
	bool has_any = false;
	size_t offset = 0;
	char dice_buf[32];

	if (!info || !buf || len == 0) return 0;
	buf[0] = '\0';

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->kind == SPELL_EFFECT_TIMED && strlen(ei->timed_name) > 0) {
			if (!has_any) {
				offset += strnfmt(buf + offset, len - offset,
					"  Side effects:");
				has_any = true;
			}
			offset += strnfmt(buf + offset, len - offset, " %s",
				ei->timed_name);
			spell_rv_format_dice(&ei->dice_rv, dice_buf, sizeof(dice_buf));
			if (strlen(dice_buf) > 0) {
				offset += strnfmt(buf + offset, len - offset, " (%s)",
					dice_buf);
			}
			offset += strnfmt(buf + offset, len - offset, ";");
		} else if (ei->kind == SPELL_EFFECT_HEAL) {
			if (!has_any) {
				offset += strnfmt(buf + offset, len - offset,
					"  Restores:");
				has_any = true;
			}
			spell_rv_format_dice(&ei->dice_rv, dice_buf, sizeof(dice_buf));
			offset += strnfmt(buf + offset, len - offset, " %s HP",
				dice_buf);
			if (ei->heal_pct_floor > 0) {
				offset += strnfmt(buf + offset, len - offset, "/%d%%",
					ei->heal_pct_floor);
			}
			offset += strnfmt(buf + offset, len - offset, ";");
		} else if (ei->kind == SPELL_EFFECT_SUMMON) {
			if (!has_any) {
				offset += strnfmt(buf + offset, len - offset,
					"  Effect:");
				has_any = true;
			}
			offset += strnfmt(buf + offset, len - offset, " summons;");
		} else if (ei->kind == SPELL_EFFECT_TELEPORT) {
			if (!has_any) {
				offset += strnfmt(buf + offset, len - offset,
					"  Effect:");
				has_any = true;
			}
			spell_rv_format_dice(&ei->dice_rv, dice_buf, sizeof(dice_buf));
			offset += strnfmt(buf + offset, len - offset, " teleport");
			if (strlen(dice_buf) > 0) {
				offset += strnfmt(buf + offset, len - offset, " %s",
					dice_buf);
			}
			if (ei->teleport_random) {
				offset += strnfmt(buf + offset, len - offset,
					" (random)");
			}
			offset += strnfmt(buf + offset, len - offset, ";");
		} else if (ei->kind == SPELL_EFFECT_DETECT) {
			if (!has_any) {
				offset += strnfmt(buf + offset, len - offset,
					"  Effect:");
				has_any = true;
			}
			offset += strnfmt(buf + offset, len - offset,
				" detect %s;", ei->info_label);
		}
	}

	if (has_any) {
		offset += strnfmt(buf + offset, len - offset, "\n");
	}
	return offset;
}


size_t spell_info_format_limits(const struct spell_info *info,
	char *buf, size_t len)
{
	size_t offset = 0;

	if (!info || !buf || len == 0) return 0;
	buf[0] = '\0';

	offset += strnfmt(buf + offset, len - offset,
		"  Level: %d, Mana: %d, Fail: %d%%",
		info->limits.slevel, info->limits.mana,
		info->limits.fail_percent);
	if (info->needs_aim) {
		offset += strnfmt(buf + offset, len - offset, ", Requires aim");
	}
	offset += strnfmt(buf + offset, len - offset, ".\n");

	return offset;
}
