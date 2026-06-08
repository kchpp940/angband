/**
 * \file player-spell.c
 * \brief Spell and prayer casting/praying
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
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

#include "angband.h"
#include "cave.h"
#include "cmd-core.h"
#include "effects.h"
#include "effects-info.h"
#include "init.h"
#include "monster.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "object.h"
#include "player-calcs.h"
#include "player-spell.h"
#include "player-timed.h"
#include "player-util.h"
#include "project.h"
#include "target.h"
#include "z-textblock.h"

/**
 * Stat Table (INT/WIS) -- Minimum failure rate (percentage)
 */
static const int adj_mag_fail[STAT_RANGE] =
{
	99	/* 3 */,
	99	/* 4 */,
	99	/* 5 */,
	99	/* 6 */,
	99	/* 7 */,
	50	/* 8 */,
	30	/* 9 */,
	20	/* 10 */,
	15	/* 11 */,
	12	/* 12 */,
	11	/* 13 */,
	10	/* 14 */,
	9	/* 15 */,
	8	/* 16 */,
	7	/* 17 */,
	6	/* 18/00-18/09 */,
	6	/* 18/10-18/19 */,
	5	/* 18/20-18/29 */,
	5	/* 18/30-18/39 */,
	5	/* 18/40-18/49 */,
	4	/* 18/50-18/59 */,
	4	/* 18/60-18/69 */,
	4	/* 18/70-18/79 */,
	4	/* 18/80-18/89 */,
	3	/* 18/90-18/99 */,
	3	/* 18/100-18/109 */,
	2	/* 18/110-18/119 */,
	2	/* 18/120-18/129 */,
	2	/* 18/130-18/139 */,
	2	/* 18/140-18/149 */,
	1	/* 18/150-18/159 */,
	1	/* 18/160-18/169 */,
	1	/* 18/170-18/179 */,
	1	/* 18/180-18/189 */,
	1	/* 18/190-18/199 */,
	0	/* 18/200-18/209 */,
	0	/* 18/210-18/219 */,
	0	/* 18/220+ */
};

/**
 * Stat Table (INT/WIS) -- failure rate adjustment
 */
static const int adj_mag_stat[STAT_RANGE] =
{
	-5	/* 3 */,
	-4	/* 4 */,
	-3	/* 5 */,
	-3	/* 6 */,
	-2	/* 7 */,
	-1	/* 8 */,
	 0	/* 9 */,
	 0	/* 10 */,
	 0	/* 11 */,
	 0	/* 12 */,
	 0	/* 13 */,
	 1	/* 14 */,
	 2	/* 15 */,
	 3	/* 16 */,
	 4	/* 17 */,
	 5	/* 18/00-18/09 */,
	 6	/* 18/10-18/19 */,
	 7	/* 18/20-18/29 */,
	 8	/* 18/30-18/39 */,
	 9	/* 18/40-18/49 */,
	10	/* 18/50-18/59 */,
	11	/* 18/60-18/69 */,
	12	/* 18/70-18/79 */,
	15	/* 18/80-18/89 */,
	18	/* 18/90-18/99 */,
	21	/* 18/100-18/109 */,
	24	/* 18/110-18/119 */,
	27	/* 18/120-18/129 */,
	30	/* 18/130-18/139 */,
	33	/* 18/140-18/149 */,
	36	/* 18/150-18/159 */,
	39	/* 18/160-18/169 */,
	42	/* 18/170-18/179 */,
	45	/* 18/180-18/189 */,
	48	/* 18/190-18/199 */,
	51	/* 18/200-18/209 */,
	54	/* 18/210-18/219 */,
	57	/* 18/220+ */
};

/**
 * Initialise player spells
 */
void player_spells_init(struct player *p)
{
	int i, num_spells = p->class->magic.total_spells;

	/* None */
	if (!num_spells) return;

	/* Allocate */
	p->spell_flags = mem_zalloc(num_spells * sizeof(uint8_t));
	p->spell_order = mem_zalloc(num_spells * sizeof(uint8_t));

	/* None of the spells have been learned yet */
	for (i = 0; i < num_spells; i++)
		p->spell_order[i] = 99;
}

/**
 * Free player spells
 */
void player_spells_free(struct player *p)
{
	mem_free(p->spell_flags);
	mem_free(p->spell_order);
}

/**
 * Make a list of the spell realms the player's class has books from
 */
struct magic_realm *class_magic_realms(const struct player_class *c, int *count)
{
	int i;
	struct magic_realm *r = mem_zalloc(sizeof(struct magic_realm));

	*count = 0;

	if (!c->magic.total_spells) {
		mem_free(r);
		return NULL;
	}

	for (i = 0; i < c->magic.num_books; i++) {
		struct magic_realm *r_test = r;
		struct class_book *book = &c->magic.books[i];
		bool found = false;

		/* Test for first realm */
		if (r->name == NULL) {
			memcpy(r, book->realm, sizeof(struct magic_realm));
			r->next = NULL;
			(*count)++;
			continue;
		}

		/* Test for already recorded */
		while (r_test) {
			if (streq(r_test->name, book->realm->name)) {
				found = true;
			}
			r_test = r_test->next;
		}
		if (found) continue;

		/* Add it */
		r_test = mem_zalloc(sizeof(struct magic_realm));
		memcpy(r_test, book->realm, sizeof(struct magic_realm));
		r_test->next = r;
		r = r_test;
		(*count)++;
	}

	return r;
}


/**
 * Get the spellbook structure from any object which is a book
 */
const struct class_book *object_kind_to_book(const struct object_kind *kind)
{
	struct player_class *class = classes;
	while (class) {
		int i;

		for (i = 0; i < class->magic.num_books; i++)
		if ((kind->tval == class->magic.books[i].tval) &&
			(kind->sval == class->magic.books[i].sval)) {
			return &class->magic.books[i];
		}
		class = class->next;
	}

	return NULL;
}

/**
 * Get the spellbook structure from an object which is a book the player can
 * cast from
 */
const struct class_book *player_object_to_book(const struct player *p,
		const struct object *obj)
{
	int i;

	for (i = 0; i < p->class->magic.num_books; i++)
		if ((obj->tval == p->class->magic.books[i].tval) &&
			(obj->sval == p->class->magic.books[i].sval))
			return &p->class->magic.books[i];

	return NULL;
}

const struct class_spell *spell_by_index(const struct player *p, int index)
{
	int book = 0, count = 0;
	const struct class_magic *magic = &p->class->magic;

	/* Check index validity */
	if (index < 0 || index >= magic->total_spells)
		return NULL;

	/* Find the book, count the spells in previous books */
	while (count + magic->books[book].num_spells - 1 < index)
		count += magic->books[book++].num_spells;

	/* Find the spell */
	return &magic->books[book].spells[index - count];
}

/**
 * Collect spells from a book into the spells[] array, allocating
 * appropriate memory.
 */
int spell_collect_from_book(const struct player *p, const struct object *obj,
		int **spells)
{
	const struct class_book *book = player_object_to_book(p, obj);
	int i, n_spells = 0;

	if (!book) {
		return n_spells;
	}

	/* Count the spells */
	for (i = 0; i < book->num_spells; i++)
		n_spells++;

	/* Allocate the array */
	*spells = mem_zalloc(n_spells * sizeof(*spells));

	/* Write the spells */
	for (i = 0; i < book->num_spells; i++)
		(*spells)[i] = book->spells[i].sidx;

	return n_spells;
}


/**
 * Return the number of castable spells in the spellbook 'obj'.
 */
int spell_book_count_spells(const struct player *p, const struct object *obj,
		bool (*tester)(const struct player *p, int spell))
{
	const struct class_book *book = player_object_to_book(p, obj);
	int i, n_spells = 0;

	if (!book) {
		return n_spells;
	}

	for (i = 0; i < book->num_spells; i++)
		if (tester(p, book->spells[i].sidx))
			n_spells++;

	return n_spells;
}


/**
 * True if at least one spell in spells[] is OK according to spell_test.
 */
bool spell_okay_list(const struct player *p,
		bool (*spell_test)(const struct player *p, int spell),
		const int spells[], int n_spells)
{
	int i;
	bool okay = false;

	for (i = 0; i < n_spells; i++)
		if (spell_test(p, spells[i]))
			okay = true;

	return okay;
}

/**
 * True if the spell is castable.
 */
bool spell_okay_to_cast(const struct player *p, int spell)
{
	return (p->spell_flags[spell] & PY_SPELL_LEARNED);
}

/**
 * True if the spell can be studied.
 */
bool spell_okay_to_study(const struct player *p, int spell_index)
{
	const struct class_spell *spell = spell_by_index(p, spell_index);
	return spell && spell->slevel <= p->lev
		&& !(p->spell_flags[spell_index] & PY_SPELL_LEARNED);
}

/**
 * True if the spell is browsable.
 */
bool spell_okay_to_browse(const struct player *p, int spell_index)
{
	const struct class_spell *spell = spell_by_index(p, spell_index);
	return spell && spell->slevel < 99;
}

/**
 * Spell failure adjustment by casting stat level
 */
static int fail_adjust(struct player *p, const struct class_spell *spell)
{
	int stat = spell->realm->stat;
	return adj_mag_stat[p->state.stat_ind[stat]];
}

/**
 * Spell minimum failure by casting stat level
 */
static int min_fail(struct player *p, const struct class_spell *spell)
{
	int stat = spell->realm->stat;
	return adj_mag_fail[p->state.stat_ind[stat]];
}

/**
 * Returns chance of failure for a spell
 */
int16_t spell_chance(int spell_index)
{
	int chance = 100, minfail;

	const struct class_spell *spell;

	/* Paranoia -- must be literate */
	if (!player->class->magic.total_spells) return chance;

	/* Get the spell */
	spell = spell_by_index(player, spell_index);
	if (!spell) return chance;

	/* Extract the base spell failure rate */
	chance = spell->sfail;

	/* Reduce failure rate by "effective" level adjustment */
	chance -= 3 * (player->lev - spell->slevel);

	/* Reduce failure rate by casting stat level adjustment */
	chance -= fail_adjust(player, spell);

	/* Not enough mana to cast */
	if (spell->smana > player->csp)
		chance += 5 * (spell->smana - player->csp);

	/* Get the minimum failure rate for the casting stat level */
	minfail = min_fail(player, spell);

	/* Non zero-fail characters never get better than 5 percent */
	if (!player_has(player, PF_ZERO_FAIL) && minfail < 5) {
		minfail = 5;
	}

	/* Necromancers are punished by being on lit squares */
	if (player_has(player, PF_UNLIGHT) && square_islit(cave, player->grid)) {
		chance += 25;
	}

	/* Fear makes spells harder (before minfail) */
	/* Note that spells that remove fear have a much lower fail rate than
	 * surrounding spells, to make sure this doesn't cause mega fail */
	if (player_of_has(player, OF_AFRAID)) chance += 20;

	/* Minimal and maximal failure rate */
	if (chance < minfail) chance = minfail;
	if (chance > 50) chance = 50;

	/* Stunning makes spells harder (after minfail) */
	if (player->timed[TMD_STUN] > 50) {
		chance += 25;
	} else if (player->timed[TMD_STUN]) {
		chance += 15;
	}

	/* Amnesia makes spells very difficult */
	if (player->timed[TMD_AMNESIA]) {
		chance = 50 + chance / 2;
	}

	/* Always a 5 percent chance of working */
	if (chance > 95) {
		chance = 95;
	}

	/* Return the chance */
	return (chance);
}


/**
 * Learn the specified spell.
 */
void spell_learn(int spell_index)
{
	int i;
	const struct class_spell *spell = spell_by_index(player, spell_index);

	/* Learn the spell */
	player->spell_flags[spell_index] |= PY_SPELL_LEARNED;

	/* Find the next open entry in "spell_order[]" */
	for (i = 0; i < player->class->magic.total_spells; i++)
		if (player->spell_order[i] == 99) break;

	/* Add the spell to the known list */
	player->spell_order[i] = spell_index;

	/* Mention the result */
	msgt(MSG_STUDY, "You have learned the %s of %s.", spell->realm->spell_noun,
		 spell->name);

	/* One less spell available */
	player->upkeep->new_spells--;

	/* Message if needed */
	if (player->upkeep->new_spells)
		msg("You can learn %d more %s%s.", player->upkeep->new_spells,
			spell->realm->spell_noun, PLURAL(player->upkeep->new_spells));

	/* Redraw Study Status */
	player->upkeep->redraw |= (PR_STUDY | PR_OBJECT);
}

static int beam_chance(void)
{
	int plev = player->lev;
	return (player_has(player, PF_BEAM) ? plev : (plev / 2));
}

/**
 * Cast the specified spell
 */
bool spell_cast(int spell_index, int dir, struct command *cmd)
{
	int chance;
	bool ident = false;
	int beam  = beam_chance();

	/* Get the spell */
	const struct class_spell *spell = spell_by_index(player, spell_index);

	/* Spell failure chance */
	chance = spell_chance(spell_index);

	/* Fail or succeed */
	if (randint0(100) < chance) {
		event_signal(EVENT_INPUT_FLUSH);
		msg("You failed to concentrate hard enough!");
	} else {
		/* Cast the spell */
		if (!effect_do(spell->effect, source_player(), NULL, &ident, true, dir,
					   beam, 0, cmd)) {
			return false;
		}

		/* Reward COMBAT_REGEN with small HP recovery */
		if (player_has(player, PF_COMBAT_REGEN)) {
			convert_mana_to_hp(player, spell->smana << 16);
		}

		/* A spell was cast */
		sound(MSG_SPELL);

		if (!(player->spell_flags[spell_index] & PY_SPELL_WORKED)) {
			int e = spell->sexp;

			/* The spell worked */
			player->spell_flags[spell_index] |= PY_SPELL_WORKED;

			/* Gain experience */
			player_exp_gain(player, e * spell->slevel);

			/* Redraw object recall */
			player->upkeep->redraw |= (PR_OBJECT);
		}
	}

	/* Sufficient mana? */
	if (spell->smana <= player->csp) {
		/* Use some mana */
		player->csp -= spell->smana;
	} else {
		int oops = spell->smana - player->csp;

		/* No mana left */
		player->csp = 0;
		player->csp_frac = 0;

		/* Over-exert the player */
		player_over_exert(player, PY_EXERT_FAINT, 100, 5 * oops + 1);
		player_over_exert(player, PY_EXERT_CON, 50, 0);
	}

	/* Redraw mana */
	player->upkeep->redraw |= (PR_MANA);

	return true;
}


bool spell_needs_aim(int spell_index)
{
	const struct class_spell *spell = spell_by_index(player, spell_index);
	assert(spell);
	return effect_aim(spell->effect);
}

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

struct spell_info *spell_info_build(int spell_index)
{
	const struct class_spell *spell = spell_by_index(player, spell_index);
	struct spell_info *info;
	struct spell_effect_info **link;
	struct effect *effect;
	dice_t *shared_dice = NULL;
	bool have_shared = false;
	random_value shared_rv = { 0, 0, 0, 0 };

	if (!spell) return NULL;

	info = mem_zalloc(sizeof(*info));
	info->mana = spell->smana;
	info->slevel = spell->slevel;
	info->fail = spell_chance(spell_index);
	info->needs_aim = effect_aim(spell->effect);
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
		ei->dice_rv = (random_value){ 0, 0, 0, 0 };
		ei->dice_str[0] = '\0';
		ei->extra[0] = '\0';
		ei->avg_damage = 0;
		ei->range = 0;
		ei->radius = 0;
		ei->is_damage = false;

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

		switch (effect->index) {
		case EF_SPHERE:
			if (effect->radius) {
				strnfmt(ei->extra, sizeof(ei->extra), ", rad %d",
					effect->radius);
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
				strnfmt(ei->extra, sizeof(ei->extra), ", rad %d",
					effect->radius);
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
		case EF_SWARM:
			strnfmt(ei->extra, sizeof(ei->extra), "x%d", rv.m_bonus);
			break;
		default:
			break;
		}

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

void get_spell_info(int spell_index, char *p, size_t len)
{
	struct spell_info *info = spell_info_build(spell_index);

	if (p && len > 0) {
		p[0] = '\0';
		if (info) {
			spell_info_format_short(info, p, len);
		}
	}

	spell_info_free(info);
}
