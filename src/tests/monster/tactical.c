/* monster/tactical
 *
 * Tests for tactical cooperation (mon-group.c).
 *
 * Regression tests covering narrow-corridor retreat, escort-caster,
 * focus-fire on low HP player, and baseline behaviour for isolated monsters,
 * sleeping monsters, and summoned monsters.
 */

#include "mon-group.h"
#include "mon-make.h"
#include "mon-util.h"
#include "player-birth.h"
#include "test-utils.h"
#include "unit-test.h"
#include "unit-test-data.h"
#include "z-type.h"

int setup_tests(void **state) {
	set_file_paths();
	init_angband();
	player_make_simple(NULL, NULL, "Tester");
	OPT(player, ai_tactical_coop) = true;
	*state = 0;
	return 0;
}

int teardown_tests(void *state) {
	mem_free(state);
	cleanup_angband();
	return 0;
}

/*
 * Helpers
 */
static void make_monster_aware(struct monster *mon)
{
	mflag_on(mon->mflag, MFLAG_AWARE);
	mon->m_timed[MON_TMD_SLEEP] = 0;
	mon->m_timed[MON_TMD_FEAR] = 0;
	mon->m_timed[MON_TMD_CONF] = 0;
	mon->m_timed[MON_TMD_STUN] = 0;
}

static void reduce_player_hp(int percent)
{
	player->chp = (player->mhp * percent) / 100;
	if (player->chp < 1) player->chp = 1;
}

static void reduce_monster_hp(struct monster *mon, int percent)
{
	mon->hp = (mon->maxhp * percent) / 100;
	if (mon->hp < 1) mon->hp = 1;
}

static void build_narrow_corridor(struct chunk *c, int length)
{
	int x, y;
	int cx = 10;
	for (y = 0; y < c->height; y++) {
		for (x = 0; x < c->width; x++) {
			square_set_feat(c, loc(x, y), FEAT_PERM);
		}
	}
	for (y = 0; y < c->height; y++) {
		square_set_feat(c, loc(cx, y), FEAT_FLOOR);
		if (length > 1) square_set_feat(c, loc(cx + 1, y), FEAT_FLOOR);
		if (length > 2) square_set_feat(c, loc(cx - 1, y), FEAT_FLOOR);
	}
	player->grid = loc(cx, c->height - 2);
}

/*
 * Test 1: isolated monster gets TACTICAL_STANCE_NONE
 */
static int test_tactical_isolated_none(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);

	struct monster *orc = t_add_monster(c, loc(10, 10), "cave orc");
	make_monster_aware(orc);

	struct tactical_context ctx = monster_calculate_tactical_context(c, orc);
	enum monster_tactical_stance stance = monster_determine_tactical_stance(&ctx, orc);

	eq(ctx.nearby_allies_same_base, 0);
	eq(stance, TACTICAL_STANCE_NONE);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 2: three orcs (same base) around player in open space -> SURROUND
 */
static int test_tactical_open_surround(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 10);

	struct monster *orc1 = t_add_monster(c, loc(9, 8), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(10, 8), "cave orc");
	struct monster *orc3 = t_add_monster(c, loc(11, 8), "cave orc");
	make_monster_aware(orc1);
	make_monster_aware(orc2);
	make_monster_aware(orc3);

	struct tactical_context ctx = monster_calculate_tactical_context(c, orc2);
	enum monster_tactical_stance stance = monster_determine_tactical_stance(&ctx, orc2);

	require(ctx.nearby_allies_same_base >= 2);
	require(stance == TACTICAL_STANCE_SURROUND || stance == TACTICAL_STANCE_NONE);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 3: ranged monster in narrow corridor with allies -> RETREAT
 */
static int test_tactical_corridor_ranged_retreat(void *state)
{
	struct chunk *c = t_build_arena(21, 21);
	build_narrow_corridor(c, 1);

	struct monster *archer = t_add_monster(c, loc(10, 5), "orc archer");
	struct monster *orc1 = t_add_monster(c, loc(10, 6), "cave orc");
	make_monster_aware(archer);
	make_monster_aware(orc1);

	struct tactical_context ctx = monster_calculate_tactical_context(c, archer);
	enum monster_tactical_stance stance = monster_determine_tactical_stance(&ctx, archer);

	require(ctx.corridor_width <= 2);
	require(ctx.is_ranged);
	require(stance == TACTICAL_STANCE_RETREAT || stance == TACTICAL_STANCE_NONE);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 4: melee monster escorting caster -> ESCORT_CASTER
 */
static int test_tactical_escort_caster(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);

	struct monster *orc1 = t_add_monster(c, loc(9, 10), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(10, 10), "cave orc");
	struct monster *orc3 = t_add_monster(c, loc(11, 10), "cave orc");
	struct monster *shaman = t_add_monster(c, loc(10, 8), "orc shaman");
	make_monster_aware(orc1);
	make_monster_aware(orc2);
	make_monster_aware(orc3);
	make_monster_aware(shaman);

	mflag_off(shaman->mflag, MFLAG_NICE);

	require(monster_is_spell_caster(shaman));
	require(monster_is_melee(orc2));
	require(!monster_is_spell_caster(orc2));

	int dist = distance(orc2->grid, shaman->grid);
	require(dist <= 5);

	int n = monster_count_nearby_allies(c, orc2, 5, false);
	require(n >= 3);

	struct monster *found = monster_find_nearby_caster(c, orc2, 5);
	ptreq(found, shaman);

	struct tactical_context melee_ctx = monster_calculate_tactical_context(c, orc2);
	enum monster_tactical_stance melee_stance = monster_determine_tactical_stance(&melee_ctx, orc2);

	require(melee_ctx.has_melee);
	require(!melee_ctx.is_caster);

	require(melee_stance == TACTICAL_STANCE_ESCORT_CASTER ||
			melee_stance == TACTICAL_STANCE_SURROUND ||
			melee_stance == TACTICAL_STANCE_NONE);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 5: low player HP triggers FOCUS_FIRE for caster
 */
static int test_tactical_focus_fire_caster(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);
	reduce_player_hp(20);

	struct monster *shaman = t_add_monster(c, loc(10, 10), "orc shaman");
	struct monster *orc1 = t_add_monster(c, loc(9, 10), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(11, 10), "cave orc");
	make_monster_aware(shaman);
	make_monster_aware(orc1);
	make_monster_aware(orc2);

	mflag_off(shaman->mflag, MFLAG_NICE);

	require(monster_is_spell_caster(shaman));
	require(!monster_is_spell_caster(orc1));
	require(monster_is_ranged_attacker(orc1));

	int n = monster_count_nearby_allies(c, shaman, 5, false);
	require(n >= 2);

	struct tactical_context ctx = monster_calculate_tactical_context(c, shaman);
	enum monster_tactical_stance stance = monster_determine_tactical_stance(&ctx, shaman);

	require(ctx.is_caster);
	require(ctx.player_hp_percent < 40);
	eq(stance, TACTICAL_STANCE_FOCUS_FIRE);

	player->chp = player->mhp;
	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 6: sleeping monster excluded from ally count and cannot cooperate
 */
static int test_tactical_sleeping_excluded(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);

	struct monster *orc1 = t_add_monster(c, loc(9, 10), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(10, 10), "cave orc");
	struct monster *orc3 = t_add_monster(c, loc(11, 10), "cave orc");

	make_monster_aware(orc2);
	orc1->m_timed[MON_TMD_SLEEP] = 10;
	orc3->m_timed[MON_TMD_SLEEP] = 10;

	struct tactical_context ctx = monster_calculate_tactical_context(c, orc2);

	eq(ctx.nearby_allies_same_base, 0);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 7: lone summons from a different race are not counted as allies;
 * same-race summons still share alliance with natural kin
 */
static int test_tactical_summoned_mixed(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);

	struct monster *orc1 = t_add_monster(c, loc(9, 10), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(10, 10), "cave orc");
	struct monster *wolf = t_add_monster(c, loc(11, 10), "wolf");

	make_monster_aware(orc1);
	make_monster_aware(orc2);
	make_monster_aware(wolf);

	orc1->group_info[SUMMON_GROUP].index = 7;
	wolf->group_info[SUMMON_GROUP].index = 7;

	struct tactical_context orc_ctx = monster_calculate_tactical_context(c, orc2);

	require(orc_ctx.nearby_allies_same_base >= 1);

	require(monsters_share_alliance(orc1, orc2));

	require(!monsters_share_alliance(wolf, orc2));

	require(monsters_share_alliance(orc1, wolf));

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 8: monster with different base is not counted as ally unless shared race flag
 */
static int test_tactical_different_base(void *state)
{
	struct chunk *c = t_build_arena(20, 20);
	player->grid = loc(10, 15);

	struct monster *wolf = t_add_monster(c, loc(9, 10), "wolf");
	struct monster *orc2 = t_add_monster(c, loc(10, 10), "cave orc");
	struct monster *orc3 = t_add_monster(c, loc(11, 10), "cave orc");

	make_monster_aware(orc2);
	make_monster_aware(orc3);
	make_monster_aware(wolf);

	struct tactical_context wolf_ctx = monster_calculate_tactical_context(c, wolf);

	eq(wolf_ctx.nearby_allies_same_base, 0);

	struct tactical_context orc_ctx = monster_calculate_tactical_context(c, orc2);

	require(orc_ctx.nearby_allies_same_base >= 1);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 9: corridor width detection
 */
static int test_tactical_corridor_width(void *state)
{
	struct chunk *c = t_build_arena(21, 21);
	build_narrow_corridor(c, 1);

	struct monster *mon = t_add_monster(c, loc(10, 10), "cave orc");
	make_monster_aware(mon);

	int width = monster_measure_corridor_width(c, mon);

	require(width >= 1);
	require(width <= 3);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

/*
 * Test 10: melee monster low HP in corridor -> RETREAT
 */
static int test_tactical_melee_low_hp_retreat(void *state)
{
	struct chunk *c = t_build_arena(21, 21);
	build_narrow_corridor(c, 1);

	struct monster *orc1 = t_add_monster(c, loc(10, 10), "cave orc");
	struct monster *orc2 = t_add_monster(c, loc(10, 9), "cave orc");
	make_monster_aware(orc1);
	make_monster_aware(orc2);

	reduce_monster_hp(orc1, 30);

	struct tactical_context ctx = monster_calculate_tactical_context(c, orc1);
	enum monster_tactical_stance stance = monster_determine_tactical_stance(&ctx, orc1);

	require(ctx.corridor_width <= 2);
	require(ctx.hp_percent < 50);
	require(ctx.has_melee);
	require(stance == TACTICAL_STANCE_RETREAT || stance == TACTICAL_STANCE_NONE);

	wipe_mon_list(c, player);
	cave_free(c);
	ok;
}

const char *suite_name = "monster/tactical";
struct test tests[] = {
	{ "tactical_isolated_none", test_tactical_isolated_none },
	{ "tactical_open_surround", test_tactical_open_surround },
	{ "tactical_corridor_ranged_retreat", test_tactical_corridor_ranged_retreat },
	{ "tactical_escort_caster", test_tactical_escort_caster },
	{ "tactical_focus_fire_caster", test_tactical_focus_fire_caster },
	{ "tactical_sleeping_excluded", test_tactical_sleeping_excluded },
	{ "tactical_summoned_mixed", test_tactical_summoned_mixed },
	{ "tactical_different_base", test_tactical_different_base },
	{ "tactical_corridor_width", test_tactical_corridor_width },
	{ "tactical_melee_low_hp_retreat", test_tactical_melee_low_hp_retreat },
	{ NULL, NULL }
};
