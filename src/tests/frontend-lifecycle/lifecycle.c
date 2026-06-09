/* frontend-lifecycle/lifecycle.c */

#include "unit-test.h"
#include "frontend-lifecycle.h"
#include "z-util.h"

#define MAX_LOG_ENTRIES 32
static const char *s_cleanup_log[MAX_LOG_ENTRIES];
static int s_cleanup_log_count = 0;
static void (*s_saved_quit_aux_before)(const char *);
static int s_fail_at_stage = -1;

static void log_cleanup(const char *name)
{
	if (s_cleanup_log_count < MAX_LOG_ENTRIES) {
		s_cleanup_log[s_cleanup_log_count++] = name;
	}
}

static void clear_log(void)
{
	s_cleanup_log_count = 0;
	s_fail_at_stage = -1;
}

static errr mock_parse_args(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_PARSE_ARGS) {
		frontend_set_stage_error("mock parse_args failed");
		return -1;
	}
	return 0;
}

static errr mock_check_capability(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_CAPABILITY) {
		frontend_set_stage_error("mock check_capability failed");
		return -1;
	}
	return 0;
}

static errr mock_load_resources(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_RESOURCES) {
		frontend_set_stage_error("mock load_resources failed: cannot open font file");
		return -1;
	}
	return 0;
}

static errr mock_register_terms(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_TERMS) {
		frontend_set_stage_error("mock register_terms failed: window creation error");
		return -1;
	}
	return 0;
}

static errr mock_subscribe_events(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_EVENTS) return -1;
	return 0;
}

static errr mock_finalize_ready(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (s_fail_at_stage == FE_STAGE_READY) return -1;
	return 0;
}

static void mock_cleanup_parse_args(void)  { log_cleanup("cleanup_parse_args"); }
static void mock_cleanup_capability(void)  { log_cleanup("cleanup_capability"); }
static void mock_cleanup_resources(void)   { log_cleanup("cleanup_resources"); }
static void mock_cleanup_terms(void)       { log_cleanup("cleanup_terms"); }
static void mock_cleanup_events(void)      { log_cleanup("cleanup_events"); }
static void mock_cleanup_ready(void)       { log_cleanup("cleanup_ready"); }
static void mock_shutdown(void)            { log_cleanup("shutdown"); }

static const struct frontend_adapter mock_adapter = {
	.name = "mock",
	.help = "Mock frontend adapter for testing",
	.hup_disconnects = false,
	.tstp_default = false,

	.init_parse_args          = mock_parse_args,
	.init_check_capability    = mock_check_capability,
	.init_load_resources      = mock_load_resources,
	.init_register_terms      = mock_register_terms,
	.init_subscribe_events    = mock_subscribe_events,
	.init_finalize_ready      = mock_finalize_ready,

	.cleanup_parse_args       = mock_cleanup_parse_args,
	.cleanup_capability       = mock_cleanup_capability,
	.cleanup_resources        = mock_cleanup_resources,
	.cleanup_terms            = mock_cleanup_terms,
	.cleanup_events           = mock_cleanup_events,
	.cleanup_ready            = mock_cleanup_ready,

	.shutdown                 = mock_shutdown,
};

static void dummy_quit(const char *s) { (void)s; }

static int setup_tests(void **data)
{
	(void)data;
	s_saved_quit_aux_before = quit_aux;
	quit_aux = dummy_quit;
	return 0;
}

static int teardown_tests(void *data)
{
	(void)data;
	quit_aux = s_saved_quit_aux_before;
	return 0;
}

static int test_success(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	rc = frontend_run_lifecycle(&mock_adapter, 0, NULL);
	res = frontend_get_last_result();

	eq(rc, 0);
	require(res->success);
	eq(res->failed_stage, FE_STAGE_MAX);
	require(quit_aux != dummy_quit);
	require(quit_aux != s_saved_quit_aux_before);

	frontend_get_active_adapter();
	ok;
}

static int test_resource_load_failure_rollback(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	s_fail_at_stage = FE_STAGE_RESOURCES;
	rc = frontend_run_lifecycle(&mock_adapter, 0, NULL);
	res = frontend_get_last_result();

	noteq(rc, 0);
	require(!res->success);
	eq(res->failed_stage, FE_STAGE_RESOURCES);
	require(strstr(res->error_message, "load_resources failed") != NULL);

	require(s_cleanup_log_count == 2);
	require(streq(s_cleanup_log[0], "cleanup_capability"));
	require(streq(s_cleanup_log[1], "cleanup_parse_args"));

	require(quit_aux == dummy_quit);

	ok;
}

static int test_term_register_failure_rollback(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	s_fail_at_stage = FE_STAGE_TERMS;
	rc = frontend_run_lifecycle(&mock_adapter, 0, NULL);
	res = frontend_get_last_result();

	noteq(rc, 0);
	require(!res->success);
	eq(res->failed_stage, FE_STAGE_TERMS);
	require(strstr(res->error_message, "register_terms failed") != NULL);

	require(s_cleanup_log_count == 3);
	require(streq(s_cleanup_log[0], "cleanup_resources"));
	require(streq(s_cleanup_log[1], "cleanup_capability"));
	require(streq(s_cleanup_log[2], "cleanup_parse_args"));

	require(quit_aux == dummy_quit);

	ok;
}

static int test_capability_failure_rollback(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	s_fail_at_stage = FE_STAGE_CAPABILITY;
	rc = frontend_run_lifecycle(&mock_adapter, 0, NULL);
	res = frontend_get_last_result();

	noteq(rc, 0);
	require(!res->success);
	eq(res->failed_stage, FE_STAGE_CAPABILITY);

	require(s_cleanup_log_count == 1);
	require(streq(s_cleanup_log[0], "cleanup_parse_args"));

	require(quit_aux == dummy_quit);

	ok;
}

static int test_parse_args_failure_no_rollback(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	s_fail_at_stage = FE_STAGE_PARSE_ARGS;
	rc = frontend_run_lifecycle(&mock_adapter, 0, NULL);
	res = frontend_get_last_result();

	noteq(rc, 0);
	require(!res->success);
	eq(res->failed_stage, FE_STAGE_PARSE_ARGS);

	eq(s_cleanup_log_count, 0);

	require(quit_aux == dummy_quit);

	ok;
}

static int test_stage_names(void *state)
{
	(void)state;
	require(streq(frontend_stage_name(FE_STAGE_PARSE_ARGS), "parse_args"));
	require(streq(frontend_stage_name(FE_STAGE_CAPABILITY), "check_capability"));
	require(streq(frontend_stage_name(FE_STAGE_RESOURCES), "load_resources"));
	require(streq(frontend_stage_name(FE_STAGE_TERMS), "register_terms"));
	require(streq(frontend_stage_name(FE_STAGE_EVENTS), "subscribe_events"));
	require(streq(frontend_stage_name(FE_STAGE_READY), "finalize_ready"));
	ok;
}

static int test_null_adapter(void *state)
{
	errr rc;
	const struct frontend_lifecycle_result *res;
	(void)state;

	clear_log();
	rc = frontend_run_lifecycle(NULL, 0, NULL);
	res = frontend_get_last_result();

	noteq(rc, 0);
	require(!res->success);
	require(strstr(res->error_message, "NULL adapter") != NULL);
	ok;
}

const char *suite_name = "frontend-lifecycle/lifecycle";
struct test tests[] = {
	{ "success", test_success },
	{ "stage_names", test_stage_names },
	{ "null_adapter", test_null_adapter },
	{ "parse_args_failure_no_rollback", test_parse_args_failure_no_rollback },
	{ "capability_failure_rollback", test_capability_failure_rollback },
	{ "resource_load_failure_rollback", test_resource_load_failure_rollback },
	{ "term_register_failure_rollback", test_term_register_failure_rollback },
	{ NULL, NULL },
};
