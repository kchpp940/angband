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

/* Test that object_split does NOT transfer floor_obj_id to the split-off
 * portion, but DOES preserve it on the remaining source pile.
 * Only the original, intact entity should count as the target. */
static int test_obj_split_preserves_source_binding(void *state) {
	struct object *src = object_new();
	struct object *split;

	object_prep(src, lookup_kind(TV_LIGHT, 1), 0, RANDOMISE);
	src->floor_obj_id = 3;
	src->number = 5;

	/* Split off 2 items - source now has 3, split has 2 */
	split = object_split(src, 2);

	/* Source pile (remaining) must keep the binding */
	eq(src->floor_obj_id, 3);
	eq(src->number, 3);

	/* Split-off pile must NOT have the binding */
	eq(split->floor_obj_id, 0);
	eq(split->number, 2);

	object_free(src);
	object_free(split);

	ok;
}

/* Test that picking up only a partial stack of the target item (via
 * object_split internally) does NOT complete the objective.
 * The split-off copy has floor_obj_id=0, so pickup check ignores it. */
static int test_partial_pickup_not_counted(void *state) {
	struct chunk *c = t_build_arena(20, 20);
	struct floor_objective *obj;
	struct object *target_obj;
	struct object *partial;

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

	/* Create a stack of 5 target torches */
	target_obj = object_new();
	object_prep(target_obj, lookup_kind(TV_LIGHT, 1), 0, RANDOMISE);
	target_obj->floor_obj_id = 1;
	target_obj->number = 5;

	/* Simulate partial pickup: split off 2 */
	partial = object_split(target_obj, 2);
	eq(partial->floor_obj_id, 0);
	eq(target_obj->floor_obj_id, 1);

	/* Pick up the partial (floor_obj_id=0) - should NOT complete */
	floor_obj_check_item_pickup(player, partial);
	eq(obj->data.item.picked_up, false);
	eq(obj->state, FLOOR_OBJ_ACTIVE);

	/* Now pick up the remaining original stack (floor_obj_id=1) - COMPLETES */
	floor_obj_check_item_pickup(player, target_obj);
	eq(obj->data.item.picked_up, true);
	eq(obj->state, FLOOR_OBJ_COMPLETED);

	object_free(partial);
	object_free(target_obj);
	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that picking up the full original target object (not a split copy)
 * triggers completion. This is the expected happy path. */
static int test_full_pickup_counts(void *state) {
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

	/* Single target item (no splitting needed) */
	target_obj = object_new();
	memset(target_obj, 0, sizeof(*target_obj));
	target_obj->floor_obj_id = 1;
	target_obj->number = 1;

	floor_obj_check_item_pickup(player, target_obj);
	eq(obj->data.item.picked_up, true);
	eq(obj->state, FLOOR_OBJ_COMPLETED);

	object_free(target_obj);
	cave_free(c);
	cave = NULL;
	player->cave = NULL;

	ok;
}

/* Test that floor_obj_validate on a chunk with count=0 (simulating an
 * old savefile that has no "floor obj" block) is a safe no-op.
 * This is the compatibility path for pre-floor-obj savefiles. */
static int test_empty_chunk_validate_noop(void *state) {
	struct chunk *c = t_build_arena(20, 20);

	player_make_simple(NULL, NULL, "Tester");
	cave = c;
	player->cave = c;

	/* New chunk from cave_new() has floor_obj.count=0 already,
	 * simulating a savefile with no "floor obj" block. */
	eq(c->floor_obj.count, 0);

	/* Should not crash, should not assert, should not touch memory */
	floor_obj_validate(c, player);

	/* Count still 0, nothing modified */
	eq(c->floor_obj.count, 0);

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
	{ "obj_split_preserves_source_binding", test_obj_split_preserves_source_binding },
	{ "partial_pickup_not_counted", test_partial_pickup_not_counted },
	{ "full_pickup_counts", test_full_pickup_counts },
	{ "empty_chunk_validate_noop", test_empty_chunk_validate_noop },
	{ NULL, NULL }
};
