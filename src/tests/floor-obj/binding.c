/* floor-obj/binding */

#include "angband.h"
#include "cave.h"
#include "floor-obj.h"
#include "mon-make.h"
#include "monster.h"
#include "obj-gear.h"
#include "obj-knowledge.h"
#include "obj-make.h"
#include "obj-pile.h"
#include "obj-util.h"
#include "object.h"
#include "player-birth.h"
#include "test-utils.h"
#include "unit-test.h"
#include "unit-test-data.h"

int setup_tests(void **state) {
	set_file_paths();
	init_angband();
	*state = 0;
	return 0;
}

int teardown_tests(void *state) {
	mem_free(state);
	cleanup_angband();
	return 0;
}

/* Test that object_copy does NOT inherit floor_obj_id.
 * A split-off or duplicated item must never be mistaken for the target. */
static int test_obj_copy_clears_binding(void *state) {
	struct object *src = object_new();
	struct object *dst = object_new();

	src->floor_obj_id = 7;
	src->number = 1;
	object_copy(dst, src);

	eq(dst->floor_obj_id, 0);
	noteq(src->floor_obj_id, 0);

	object_free(src);
	object_free(dst);

	ok;
}

/* Test that killing an ordinary (same-race) monster does NOT increment
 * the objective counter. Only the monsters with matching floor_obj_id count. */
static int test_ordinary_monster_not_counted(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct monster *target_mon;
	struct monster *ordinary_mon;

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	c->floor_obj.count = 1;
	obj = &c->floor_obj.objs[0];
	memset(obj, 0, sizeof(*obj));
	obj->objective_id = 1;
	obj->type = FLOOR_OBJ_CLEAR_NEST;
	obj->state = FLOOR_OBJ_ACTIVE;
	obj->description = string_make("清理怪物巢穴");
	obj->data.nest.total_kills = 2;
	obj->data.nest.current_kills = 0;
	obj->reward_type = FLOOR_REWARD_GOLD;
	obj->reward_value = 100;

	/* Place target monster with matching floor_obj_id */
	target_mon = t_add_monster(c, loc(5, 5), "wolf");
	target_mon->floor_obj_id = 1;

	/* Place ordinary monster same race but NO floor_obj_id */
	ordinary_mon = t_add_monster(c, loc(6, 6), "wolf");
	ordinary_mon->floor_obj_id = 0;

	/* Kill the ordinary one first - should NOT count */
	floor_obj_check_monster_kill(player, ordinary_mon);
	eq(obj->data.nest.current_kills, 0);

	/* Now kill the target - should count */
	floor_obj_check_monster_kill(player, target_mon);
	eq(obj->data.nest.current_kills, 1);

	wipe_mon_list(c, player);
	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that if all target monsters vanish with no progress made,
 * floor_obj_validate marks the objective FAILED rather than leaving it
 * in a state where it might accidentally be completed later. */
static int test_vanished_entities_mark_failed(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct monster *m1, *m2;

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	c->floor_obj.count = 1;
	obj = &c->floor_obj.objs[0];
	memset(obj, 0, sizeof(*obj));
	obj->objective_id = 1;
	obj->type = FLOOR_OBJ_CLEAR_NEST;
	obj->state = FLOOR_OBJ_ACTIVE;
	obj->description = string_make("清理怪物巢穴");
	obj->data.nest.total_kills = 2;
	obj->data.nest.current_kills = 0;
	obj->reward_type = FLOOR_REWARD_EXP;
	obj->reward_value = 50;

	m1 = t_add_monster(c, loc(5, 5), "wolf");
	m1->floor_obj_id = 1;
	m2 = t_add_monster(c, loc(6, 6), "wolf");
	m2->floor_obj_id = 1;

	/* Wipe the monsters away simulating mysterious disappearance */
	wipe_mon_list(c, player);

	/* Now validate - objective should be FAILED because no target entities
	 * remain and there was no progress at all. */
	floor_obj_validate(c, player);
	eq(obj->state, FLOOR_OBJ_FAILED);
	eq(obj->data.nest.total_kills, 0);

	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that if some target monsters were killed before the rest vanished,
 * validation caps the total and marks as completed, not failed.
 * The player deserves credit for what they actually killed. */
static int test_partial_progress_survives_vanish(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct monster *m1, *m2;

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	c->floor_obj.count = 1;
	obj = &c->floor_obj.objs[0];
	memset(obj, 0, sizeof(*obj));
	obj->objective_id = 1;
	obj->type = FLOOR_OBJ_CLEAR_NEST;
	obj->state = FLOOR_OBJ_ACTIVE;
	obj->description = string_make("清理怪物巢穴");
	obj->data.nest.total_kills = 3;
	obj->data.nest.current_kills = 1;
	obj->reward_type = FLOOR_REWARD_EXP;
	obj->reward_value = 50;

	/* Only 2 monsters remain, player already killed 1 */
	m1 = t_add_monster(c, loc(5, 5), "wolf");
	m1->floor_obj_id = 1;
	m2 = t_add_monster(c, loc(6, 6), "wolf");
	m2->floor_obj_id = 1;

	/* Wipe the remaining 2 */
	wipe_mon_list(c, player);

	/* Validate: 1 kill + 0 remaining = 1 total, so should auto-complete */
	floor_obj_validate(c, player);
	eq(obj->state, FLOOR_OBJ_COMPLETED);
	eq(obj->data.nest.total_kills, 1);
	eq(obj->data.nest.current_kills, 1);

	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that a RETRIEVE_ITEM objective whose item has been carried into the
 * player's pack is still found by validation (remains ACTIVE, not FAILED).
 * This simulates a save/load cycle while the item is being carried. */
static int test_item_in_pack_survives_validation(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct object *target_obj;

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	c->floor_obj.count = 1;
	obj = &c->floor_obj.objs[0];
	memset(obj, 0, sizeof(*obj));
	obj->objective_id = 1;
	obj->type = FLOOR_OBJ_RETRIEVE_ITEM;
	obj->state = FLOOR_OBJ_ACTIVE;
	obj->description = string_make("回收特殊物品");
	obj->data.item.picked_up = false;
	obj->reward_type = FLOOR_REWARD_OBJECT;
	obj->reward_value = 0;

	/* Create the target item and place it in the player's pack */
	target_obj = object_new();
	object_prep(target_obj, lookup_kind(TV_LIGHT, 1), 0, RANDOMISE);
	target_obj->floor_obj_id = 1;
	target_obj->number = 1;
	target_obj->known = object_new();
	object_set_base_known(player, target_obj);
	object_touch(player, target_obj);

	gear_insert_end(player, target_obj);

	/* The item is in player->gear, NOT in the chunk's objects array. */

	/* Validate - should remain ACTIVE because item is in player pack */
	floor_obj_validate(c, player);
	eq(obj->state, FLOOR_OBJ_ACTIVE);

	/* Now remove the item from the pack and validate again - should FAIL */
	{
		struct object *known_obj = target_obj->known;
		pile_excise(&player->gear, target_obj);
		pile_excise(&player->gear_k, known_obj);
		object_free(known_obj);
		object_free(target_obj);
		target_obj = NULL;
	}
	floor_obj_validate(c, player);
	eq(obj->state, FLOOR_OBJ_FAILED);

	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that picking up an ordinary (non-bound) item does NOT trigger
 * the RETRIEVE_ITEM objective completion. Only the specific bound item counts. */
static int test_ordinary_item_not_counted(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct object *ordinary_obj;

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	c->floor_obj.count = 1;
	obj = &c->floor_obj.objs[0];
	memset(obj, 0, sizeof(*obj));
	obj->objective_id = 1;
	obj->type = FLOOR_OBJ_RETRIEVE_ITEM;
	obj->state = FLOOR_OBJ_ACTIVE;
	obj->description = string_make("回收特殊物品");
	obj->data.item.picked_up = false;
	obj->reward_type = FLOOR_REWARD_GOLD;
	obj->reward_value = 100;

	/* Simulate picking up an ordinary item with floor_obj_id = 0 */
	ordinary_obj = object_new();
	memset(ordinary_obj, 0, sizeof(*ordinary_obj));
	ordinary_obj->floor_obj_id = 0;
	ordinary_obj->number = 1;

	floor_obj_check_item_pickup(player, ordinary_obj);
	eq(obj->data.item.picked_up, false);
	eq(obj->state, FLOOR_OBJ_ACTIVE);

	object_free(ordinary_obj);
	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

const char *suite_name = "floor-obj/binding";
struct test tests[] = {
	{ "obj_copy_clears_binding", test_obj_copy_clears_binding },
	{ "ordinary_monster_not_counted", test_ordinary_monster_not_counted },
	{ "vanished_entities_mark_failed", test_vanished_entities_mark_failed },
	{ "partial_progress_survives_vanish", test_partial_progress_survives_vanish },
	{ "item_in_pack_survives_validation", test_item_in_pack_survives_validation },
	{ "ordinary_item_not_counted", test_ordinary_item_not_counted },
	{ NULL, NULL }
};
