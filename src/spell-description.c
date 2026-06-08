/**
 * \file spell-description.c
 * \brief Unified spell description generator from effect/projection data
 *
 * Builds structured spell information and formats both short menu-line
 * strings and detailed browse-view descriptions from a single pass over
 * the spell's effect chain.
 */

#include "angband.h"
#include "effects.h"
#include "effects-info.h"
#include "init.h"
#include "player-spell.h"
#include "player-timed.h"
#include "project.h"
#include "spell-description.h"
#include "z-color.h"
#include "z-form.h"
#include "z-textblock.h"
#include "z-util.h"


static size_t append_rv_to_dice_str(char *buffer, size_t size, random_value *rv)
{
	size_t offset = 0;

	if (rv->base > 0) {
		offset += strnfmt(buffer + offset, size - offset, "%d", rv->base);

		if (rv->dice > 0 && rv->sides > 0) {
			offset += strnfmt(buffer + offset, size - offset, "+");
		}
	}

	if (rv->dice == 1 && rv->sides > 0) {
		offset += strnfmt(buffer + offset, size - offset, "d%d", rv->sides);
	} else if (rv->dice > 1 && rv->sides > 0) {
		offset += strnfmt(buffer + offset, size - offset, "%dd%d", rv->dice,
						  rv->sides);
	}

	return offset;
}


static const char *timed_idx_to_name(int idx)
{
	if (idx < 0 || idx >= TMD_MAX) return "";
	return timed_effects[idx].name ? timed_effects[idx].name : "";
}


static void spell_effect_fill_extra(struct spell_effect_info *ei,
	const struct effect *effect)
{
	switch (effect->index) {
	case EF_SPHERE:
		if (effect->radius) {
			strnfmt(ei->extra, sizeof(ei->extra), ", rad %d", effect->radius);
		} else {
			my_strcpy(ei->extra, ", rad 2", sizeof(ei->extra));
		}
		break;
	case EF_BALL:
	case EF_STAR_BALL: {
		int rad = effect->radius;
		if (effect->other) {
			rad += player->lev / effect->other;
		}
		if (rad) {
			strnfmt(ei->extra, sizeof(ei->extra), ", rad %d", rad);
		} else {
			my_strcpy(ei->extra, "rad 2", sizeof(ei->extra));
		}
		break;
	}
	case EF_STRIKE:
		if (effect->radius) {
			strnfmt(ei->extra, sizeof(ei->extra), ", rad %d", effect->radius);
		}
		break;
	case EF_SHORT_BEAM: {
		int beam_len = effect->radius;
		if (effect->other) {
			beam_len += player->lev / effect->other;
			beam_len = MIN(beam_len, (int)z_info->max_range);
		}
		strnfmt(ei->extra, sizeof(ei->extra), ", len %d", beam_len);
		break;
	}
	case EF_SWARM: {
		random_value rv = ei->dice_rv;
		strnfmt(ei->extra, sizeof(ei->extra), "x%d", rv.m_bonus);
		break;
	}
	default:
		break;
	}
}


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
		ei->info_label = type ? type : "";
		ei->projection_name = "";
		ei->timed_name = "";
		ei->dice_rv = (random_value){ 0, 0, 0, 0 };
		ei->dice_str[0] = '\0';
		ei->extra[0] = '\0';
		ei->avg_damage = 0;
		ei->range = 0;
		ei->radius = 0;
		ei->is_damage = false;
		ei->needs_aim = false;

		if (effect->dice != NULL) {
			dice_roll(effect->dice, &rv);
		} else if (have_shared) {
			rv = shared_rv;
		}

		ei->dice_rv = rv;

		proj = effect_projection(effect);
		if (proj && strlen(proj) > 0) {
			ei->projection_name = proj;
		}

		if (effect_damages(effect)) {
			ei->kind = SPELL_EFFECT_DAMAGE;
			ei->is_damage = true;
			ei->needs_aim = true;
			ei->avg_damage = effect_avg_damage(effect,
				have_shared ? shared_dice : NULL);
			ei->range = effect_range(effect);
			ei->radius = effect_radius(effect);
			append_rv_to_dice_str(ei->dice_str, sizeof(ei->dice_str), &rv);
		} else if (effect->index == EF_HEAL_HP) {
			ei->kind = SPELL_EFFECT_HEAL;
			append_rv_to_dice_str(ei->dice_str, sizeof(ei->dice_str), &rv);
			if (rv.m_bonus) {
				strnfmt(ei->extra, sizeof(ei->extra), "/%d%%", rv.m_bonus);
			}
		} else if (effect->index == EF_CURE
				   || effect->index == EF_TIMED_SET
				   || effect->index == EF_TIMED_INC
				   || effect->index == EF_TIMED_INC_NO_RES
				   || effect->index == EF_TIMED_DEC) {
			ei->kind = SPELL_EFFECT_TIMED;
			ei->timed_name = timed_idx_to_name(effect->subtype);
			append_rv_to_dice_str(ei->dice_str, sizeof(ei->dice_str), &rv);
		} else if (effect->index == EF_SUMMON) {
			ei->kind = SPELL_EFFECT_SUMMON;
		} else if (effect->index == EF_TELEPORT
				   || effect->index == EF_TELEPORT_TO
				   || effect->index == EF_TELEPORT_LEVEL) {
			ei->kind = SPELL_EFFECT_TELEPORT;
			append_rv_to_dice_str(ei->dice_str, sizeof(ei->dice_str), &rv);
			if (rv.m_bonus) {
				my_strcpy(ei->extra, "random", sizeof(ei->extra));
			}
		} else if (effect->index >= EF_DETECT_TRAPS
				   && effect->index <= EF_DETECT_SOUL) {
			ei->kind = SPELL_EFFECT_DETECT;
		} else {
			ei->kind = SPELL_EFFECT_OTHER;
			append_rv_to_dice_str(ei->dice_str, sizeof(ei->dice_str), &rv);
		}

		spell_effect_fill_extra(ei, effect);

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


size_t spell_info_format_short(const struct spell_info *info, char *buf,
							   size_t len)
{
	size_t offset = 0;
	struct spell_effect_info *ei;
	struct spell_effect_info *pre = NULL;
	char pre_special[40] = "";
	random_value pre_rv = { 0, 0, 0, 0 };

	if (!info || !buf || len == 0) return 0;

	buf[0] = '\0';

	for (ei = info->effects; ei; ei = ei->next) {
		random_value rv = ei->dice_rv;
		bool same_as_prev = false;

		if (pre && pre->kind == ei->kind
			&& streq(pre_special, ei->extra)
			&& pre_rv.base == rv.base
			&& pre_rv.dice == rv.dice
			&& pre_rv.sides == rv.sides
			&& pre_rv.m_bonus == rv.m_bonus
			&& streq(pre->info_label, ei->info_label)
			&& streq(pre->projection_name, ei->projection_name)) {
			same_as_prev = true;
		}

		if ((strlen(ei->dice_str) > 0 || strlen(ei->extra) > 0)
			&& !same_as_prev) {
			if (offset) {
				offset += strnfmt(buf + offset, len - offset, ";");
			}

			offset += strnfmt(buf + offset, len - offset, " %s ",
							  ei->info_label);
			offset += strnfmt(buf + offset, len - offset, "%s",
							  ei->dice_str);

			if (strlen(ei->extra) > 1) {
				offset += strnfmt(buf + offset, len - offset, "%s",
								  ei->extra);
			}

			pre = ei;
			my_strcpy(pre_special, ei->extra, sizeof(pre_special));
			pre_rv = rv;
		}
	}

	return offset;
}


static void spell_append_damage_summary(const struct spell_info *info,
	textblock *tb)
{
	const struct spell_effect_info *ei;
	int num_damaging = 0;
	int i = 0;

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->is_damage) num_damaging++;
	}

	if (num_damaging == 0) return;

	textblock_append(tb, "  Inflicts an average of");
	for (ei = info->effects; ei; ei = ei->next) {
		if (!ei->is_damage) continue;
		if (num_damaging > 2 && i > 0) {
			textblock_append(tb, ",");
		}
		if (num_damaging > 1 && i == num_damaging - 1) {
			textblock_append(tb, " and");
		}
		textblock_append_c(tb, COLOUR_L_GREEN, " %d", ei->avg_damage);
		if (strlen(ei->projection_name) > 0) {
			textblock_append(tb, " %s", ei->projection_name);
		}
		if (ei->radius > 0) {
			textblock_append(tb, " (radius %d)", ei->radius);
		} else if (ei->range > 0) {
			textblock_append(tb, " (range %d)", ei->range);
		}
		i++;
	}
	textblock_append(tb, " damage.\n");
}


static void spell_append_side_effects(const struct spell_info *info,
	textblock *tb)
{
	const struct spell_effect_info *ei;
	bool has_any = false;

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->kind == SPELL_EFFECT_TIMED && strlen(ei->timed_name) > 0) {
			if (!has_any) {
				textblock_append(tb, "  Side effects:");
				has_any = true;
			}
			textblock_append(tb, " %s", ei->timed_name);
			if (strlen(ei->dice_str) > 0) {
				textblock_append(tb, " (%s)", ei->dice_str);
			}
			textblock_append(tb, ";");
		} else if (ei->kind == SPELL_EFFECT_HEAL) {
			if (!has_any) {
				textblock_append(tb, "  Restores:");
				has_any = true;
			}
			textblock_append(tb, " %s HP", ei->dice_str);
			if (strlen(ei->extra) > 0) {
				textblock_append(tb, "%s", ei->extra);
			}
			textblock_append(tb, ";");
		} else if (ei->kind == SPELL_EFFECT_SUMMON) {
			if (!has_any) {
				textblock_append(tb, "  Effect:");
				has_any = true;
			}
			textblock_append(tb, " summons;");
		} else if (ei->kind == SPELL_EFFECT_TELEPORT) {
			if (!has_any) {
				textblock_append(tb, "  Effect:");
				has_any = true;
			}
			textblock_append(tb, " teleport");
			if (strlen(ei->dice_str) > 0) {
				textblock_append(tb, " %s", ei->dice_str);
			}
			if (strlen(ei->extra) > 0) {
				textblock_append(tb, " (%s)", ei->extra);
			}
			textblock_append(tb, ";");
		} else if (ei->kind == SPELL_EFFECT_DETECT) {
			if (!has_any) {
				textblock_append(tb, "  Effect:");
				has_any = true;
			}
			textblock_append(tb, " detect %s;", ei->info_label);
		}
	}

	if (has_any) {
		textblock_append(tb, "\n");
	}
}


static void spell_append_limits(const struct spell_info *info, textblock *tb)
{
	textblock_append(tb, "  Level: %d, Mana: %d, Fail: %d%%",
		info->limits.slevel, info->limits.mana,
		info->limits.fail_percent);
	if (info->needs_aim) {
		textblock_append(tb, ", Requires aim");
	}
	textblock_append(tb, ".\n");
}


void spell_info_append_detail(const struct spell_info *info,
	textblock *tb, bool include_damage_summary,
	bool include_side_effects, bool include_limits)
{
	if (!info || !tb) return;

	if (info->text) {
		textblock_append(tb, "\n%s\n", info->text);
	}

	if (include_damage_summary) {
		spell_append_damage_summary(info, tb);
	}

	if (include_side_effects) {
		spell_append_side_effects(info, tb);
	}

	if (include_limits) {
		spell_append_limits(info, tb);
	}
}


static void spell_text_out_damage_summary(const struct spell_info *info)
{
	const struct spell_effect_info *ei;
	int num_damaging = 0;
	int i = 0;

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->is_damage) num_damaging++;
	}

	if (num_damaging == 0) return;

	text_out("  Inflicts an average of");
	for (ei = info->effects; ei; ei = ei->next) {
		if (!ei->is_damage) continue;
		if (num_damaging > 2 && i > 0) {
			text_out(",");
		}
		if (num_damaging > 1 && i == num_damaging - 1) {
			text_out(" and");
		}
		text_out_c(COLOUR_L_GREEN, " %d", ei->avg_damage);
		if (strlen(ei->projection_name) > 0) {
			text_out(" %s", ei->projection_name);
		}
		if (ei->radius > 0) {
			text_out(" (radius %d)", ei->radius);
		} else if (ei->range > 0) {
			text_out(" (range %d)", ei->range);
		}
		i++;
	}
	text_out(" damage.\n");
}


static void spell_text_out_side_effects(const struct spell_info *info)
{
	const struct spell_effect_info *ei;
	bool has_any = false;

	for (ei = info->effects; ei; ei = ei->next) {
		if (ei->kind == SPELL_EFFECT_TIMED && strlen(ei->timed_name) > 0) {
			if (!has_any) {
				text_out("  Side effects:");
				has_any = true;
			}
			text_out(" %s", ei->timed_name);
			if (strlen(ei->dice_str) > 0) {
				text_out(" (%s)", ei->dice_str);
			}
			text_out(";");
		} else if (ei->kind == SPELL_EFFECT_HEAL) {
			if (!has_any) {
				text_out("  Restores:");
				has_any = true;
			}
			text_out(" %s HP", ei->dice_str);
			if (strlen(ei->extra) > 0) {
				text_out("%s", ei->extra);
			}
			text_out(";");
		} else if (ei->kind == SPELL_EFFECT_SUMMON) {
			if (!has_any) {
				text_out("  Effect:");
				has_any = true;
			}
			text_out(" summons;");
		} else if (ei->kind == SPELL_EFFECT_TELEPORT) {
			if (!has_any) {
				text_out("  Effect:");
				has_any = true;
			}
			text_out(" teleport");
			if (strlen(ei->dice_str) > 0) {
				text_out(" %s", ei->dice_str);
			}
			if (strlen(ei->extra) > 0) {
				text_out(" (%s)", ei->extra);
			}
			text_out(";");
		} else if (ei->kind == SPELL_EFFECT_DETECT) {
			if (!has_any) {
				text_out("  Effect:");
				has_any = true;
			}
			text_out(" detect %s;", ei->info_label);
		}
	}

	if (has_any) {
		text_out("\n");
	}
}


static void spell_text_out_limits(const struct spell_info *info)
{
	text_out("  Level: %d, Mana: %d, Fail: %d%%",
		info->limits.slevel, info->limits.mana,
		info->limits.fail_percent);
	if (info->needs_aim) {
		text_out(", Requires aim");
	}
	text_out(".\n");
}


void spell_info_text_out_detail(const struct spell_info *info,
	bool include_damage_summary, bool include_side_effects,
	bool include_limits)
{
	if (!info) return;

	if (info->text) {
		text_out("\n%s\n", info->text);
	}

	if (include_damage_summary) {
		spell_text_out_damage_summary(info);
	}

	if (include_side_effects) {
		spell_text_out_side_effects(info);
	}

	if (include_limits) {
		spell_text_out_limits(info);
	}
}
