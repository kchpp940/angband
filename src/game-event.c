/**
 * \file game-event.c
 * \brief Allows the registering of handlers to be told about game events.
 *
 * Copyright (c) 2007 Antony Sidwell
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

#include <assert.h>
#include <string.h>
#include "game-event.h"
#include "object.h"
#include "z-virt.h"

struct event_handler_entry
{
	struct event_handler_entry *next;	
	game_event_handler *fn;
	void *user;
};

static struct event_handler_entry *event_handlers[N_GAME_EVENTS];

static int queue_depth = 0;

typedef struct queued_event
{
	game_event_type type;
	game_event_data data;
	bool has_data;
	struct queued_event *next;
} queued_event_t;

static queued_event_t *event_queue_head = NULL;
static queued_event_t *event_queue_tail = NULL;

static void game_event_dispatch(game_event_type type, game_event_data *data);

static bool event_is_duplicable(game_event_type type)
{
	switch (type) {
	case EVENT_MAP:
	case EVENT_STATS:
	case EVENT_HP:
	case EVENT_MANA:
	case EVENT_AC:
	case EVENT_EXPERIENCE:
	case EVENT_PLAYERLEVEL:
	case EVENT_PLAYERTITLE:
	case EVENT_GOLD:
	case EVENT_MONSTERHEALTH:
	case EVENT_DUNGEONLEVEL:
	case EVENT_PLAYERSPEED:
	case EVENT_RACE_CLASS:
	case EVENT_STUDYSTATUS:
	case EVENT_STATUS:
	case EVENT_DETECTIONSTATUS:
	case EVENT_FEELING:
	case EVENT_LIGHT:
	case EVENT_STATE:
	case EVENT_INVENTORY:
	case EVENT_EQUIPMENT:
	case EVENT_ITEMLIST:
	case EVENT_MONSTERLIST:
	case EVENT_MONSTERTARGET:
	case EVENT_OBJECTTARGET:
	case EVENT_DANGER_HP:
	case EVENT_DANGER_MANA:
	case EVENT_REFRESH:
	case EVENT_MESSAGE_HIGHLIGHT:
	case EVENT_STATUSBAR:
	case EVENT_MAP_REDRAW:
	case EVENT_SUBWINDOW:
		return true;
	default:
		return false;
	}
}

static bool event_queue_has_type(game_event_type type)
{
	queued_event_t *q = event_queue_head;
	while (q) {
		if (q->type == type && event_is_duplicable(type))
			return true;
		q = q->next;
	}
	return false;
}

static void event_queue_enqueue(game_event_type type, game_event_data *data)
{
	queued_event_t *q;

	if (event_is_duplicable(type) && event_queue_has_type(type)) {
		if (data != NULL) {
			queued_event_t *existing = event_queue_head;
			while (existing) {
				if (existing->type == type) {
					existing->data = *data;
					existing->has_data = true;
					return;
				}
				existing = existing->next;
			}
		}
		return;
	}

	q = mem_zalloc(sizeof(queued_event_t));
	q->type = type;
	if (data != NULL) {
		q->data = *data;
		q->has_data = true;
	}
	q->next = NULL;

	if (event_queue_tail) {
		event_queue_tail->next = q;
	} else {
		event_queue_head = q;
	}
	event_queue_tail = q;
}

static void event_queue_dispatch_all(void)
{
	queued_event_t *q = event_queue_head;
	queued_event_t *next;

	while (q) {
		next = q->next;
		game_event_dispatch(q->type, q->has_data ? &q->data : NULL);
		mem_free(q);
		q = next;
	}
	event_queue_head = NULL;
	event_queue_tail = NULL;
}

static void game_event_dispatch(game_event_type type, game_event_data *data)
{
	struct event_handler_entry *this = event_handlers[type];

	if (queue_depth > 0) {
		event_queue_enqueue(type, data);
		return;
	}

	while (this)
	{
		this->fn(type, data, this->user);
		this = this->next;
	}
}

void event_add_handler(game_event_type type, game_event_handler *fn, void *user)
{
	struct event_handler_entry *new;

	assert(fn != NULL);

	/* Make a new entry */
	new = mem_alloc(sizeof *new);
	new->fn = fn;
	new->user = user;

	/* Add it to the head of the appropriate list */
	new->next = event_handlers[type];
	event_handlers[type] = new;
}

void event_remove_handler(game_event_type type, game_event_handler *fn, void *user)
{
	struct event_handler_entry *prev = NULL;
	struct event_handler_entry *this = event_handlers[type];

	/* Look for the entry in the list */
	while (this)
	{
		/* Check if this is the entry we want to remove */
		if (this->fn == fn && this->user == user)
		{
			if (!prev)
			{
				event_handlers[type] = this->next;
			}
			else
			{
				prev->next = this->next;
			}

			mem_free(this);
			return;
		}

		prev = this;
		this = this->next;
	}
}

void event_remove_handler_type(game_event_type type)
{
	struct event_handler_entry *handler = event_handlers[type];

	while (handler) {
		struct event_handler_entry *next = handler->next;
		mem_free(handler);
		handler = next;
	}
	event_handlers[type] = NULL;
}

void event_remove_all_handlers(void)
{
	int type;
	struct event_handler_entry *handler, *next;

	for (type = 0; type < N_GAME_EVENTS; type++) {
		handler = event_handlers[type];
		while (handler) {
			next = handler->next;
			mem_free(handler);
			handler = next;
		}
		event_handlers[type] = NULL;
	}
}

void event_add_handler_set(game_event_type *type, size_t n_types, game_event_handler *fn, void *user)
{
	size_t i;

	for (i = 0; i < n_types; i++)
		event_add_handler(type[i], fn, user);
}

void event_remove_handler_set(game_event_type *type, size_t n_types, game_event_handler *fn, void *user)
{
	size_t i;

	for (i = 0; i < n_types; i++)
		event_remove_handler(type[i], fn, user);
}




void event_signal(game_event_type type)
{
	game_event_dispatch(type, NULL);
}

void event_signal_flag(game_event_type type, bool flag)
{
	game_event_data data;
	data.flag = flag;

	game_event_dispatch(type, &data);
}


void event_signal_point(game_event_type type, int x, int y)
{
	game_event_data data;
	data.point.x = x;
	data.point.y = y;

	game_event_dispatch(type, &data);
}


void event_signal_string(game_event_type type, const char *s)
{
	game_event_data data;
	data.string = s;

	game_event_dispatch(type, &data);
}

void event_signal_message(game_event_type type, int t, const char *s)
{
	game_event_data data;
	memset(&data, 0, sizeof data);

	data.message.type = t;
	data.message.msg = s;

	game_event_dispatch(type, &data);
}

/**
 * Signal a change or refresh in the point buy for birth stats.
 *
 * \param points points[i] is the number of points already spent to increase
 * the ith stat, i >= 0 and i < STAT_MAX.
 * \param inc_points inc_points[i] is the number of additional points it would
 * take to incrase the ith stat by one, i >= 0 and i < STAT_MAX.
 * \param remaining is the number of poitns that remain to be spent.
 */
void event_signal_birthpoints(const int *points, const int *inc_points,
		int remaining)
{
	game_event_data data;

	data.birthpoints.points = points;
	data.birthpoints.inc_points = inc_points;
	data.birthpoints.remaining = remaining;

	game_event_dispatch(EVENT_BIRTHPOINTS, &data);
}

void event_signal_blast(game_event_type type,
						int proj_type,
						int num_grids,
						int *distance_to_grid,
						bool drawing,
						bool *player_sees_grid,
						struct loc *blast_grid,
						struct loc centre)
{
	game_event_data data;
	data.explosion.proj_type = proj_type;
	data.explosion.num_grids = num_grids;
	data.explosion.distance_to_grid = distance_to_grid;
	data.explosion.drawing = drawing;
	data.explosion.player_sees_grid = player_sees_grid;
	data.explosion.blast_grid = blast_grid;
	data.explosion.centre = centre;

	game_event_dispatch(type, &data);
}

void event_signal_bolt(game_event_type type,
					   int proj_type,
					   bool drawing,
					   bool seen,
					   bool beam,
					   int oy,
					   int ox,
					   int y,
					   int x)
{
	game_event_data data;
	data.bolt.proj_type = proj_type;
	data.bolt.drawing = drawing;
	data.bolt.seen = seen;
	data.bolt.beam = beam;
	data.bolt.oy = oy;
	data.bolt.ox = ox;
	data.bolt.y = y;
	data.bolt.x = x;

	game_event_dispatch(type, &data);
}

void event_signal_missile(game_event_type type,
						  struct object *obj,
						  bool seen,
						  int y,
						  int x)
{
	game_event_data data;
	data.missile.obj = obj;
	data.missile.seen = seen;
	data.missile.y = y;
	data.missile.x = x;

	game_event_dispatch(type, &data);
}

void event_signal_size(game_event_type type, int h, int w)
{
	game_event_data data;

	data.size.h = h;
	data.size.w = w;
	game_event_dispatch(type, &data);
}

void event_signal_tunnel(game_event_type type, int nstep, int npierce, int ndug,
		int dstart, int dend, bool early)
{
	game_event_data data;

	data.tunnel.nstep = nstep;
	data.tunnel.npierce = npierce;
	data.tunnel.ndug = ndug;
	data.tunnel.dstart = dstart;
	data.tunnel.dend = dend;
	data.tunnel.early = early;
	game_event_dispatch(type, &data);
}

void event_signal_danger(game_event_type type, int level, int cur, int max)
{
	game_event_data data;
	memset(&data, 0, sizeof(data));
	data.danger.level = level;
	data.danger.cur = cur;
	data.danger.max = max;
	game_event_dispatch(type, &data);
}

void event_signal_message_highlight(int msg_type, const char *text)
{
	game_event_data data;
	memset(&data, 0, sizeof(data));
	data.message_highlight.type = msg_type;
	data.message_highlight.text = text;
	game_event_dispatch(EVENT_MESSAGE_HIGHLIGHT, &data);
}

void event_signal_statusbar(uint32_t flags)
{
	game_event_data data;
	memset(&data, 0, sizeof(data));
	data.statusbar.flags = flags;
	game_event_dispatch(EVENT_STATUSBAR, &data);
}

void event_signal_map_redraw(bool full, int x1, int y1, int x2, int y2)
{
	game_event_data data;
	memset(&data, 0, sizeof(data));
	data.map_redraw.full = full;
	data.map_redraw.x1 = x1;
	data.map_redraw.y1 = y1;
	data.map_redraw.x2 = x2;
	data.map_redraw.y2 = y2;
	game_event_dispatch(EVENT_MAP_REDRAW, &data);
}

void event_signal_subwindow(int type)
{
	game_event_data data;
	memset(&data, 0, sizeof(data));
	data.subwindow.type = type;
	game_event_dispatch(EVENT_SUBWINDOW, &data);
}

void event_queue_begin(void)
{
	queue_depth++;
}

void event_queue_flush(void)
{
	if (queue_depth > 0) {
		queue_depth--;
		if (queue_depth == 0) {
			event_queue_dispatch_all();
			event_signal(EVENT_UI_FLUSH);
		}
	}
}

bool event_queue_is_active(void)
{
	return queue_depth > 0;
}
