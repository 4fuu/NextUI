#include "fn_action.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if (!(cond))                                                      \
		{                                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                   \
		}                                                                 \
	} while (0)

typedef struct
{
	int launch_calls;
	int home_calls;
	int overlay_calls;
	int result;
	char last_path[256];
} FakeHandlers;

static int fake_launch(void* user, const char* path)
{
	FakeHandlers* fake = (FakeHandlers*)user;
	fake->launch_calls++;
	snprintf(fake->last_path, sizeof(fake->last_path), "%s", path ? path : "(null)");
	return fake->result;
}

static int fake_home(void* user)
{
	FakeHandlers* fake = (FakeHandlers*)user;
	fake->home_calls++;
	return fake->result;
}

static int fake_overlay(void* user)
{
	FakeHandlers* fake = (FakeHandlers*)user;
	fake->overlay_calls++;
	return fake->result;
}

static FnActionHandlers all_handlers(FakeHandlers* fake)
{
	FnActionHandlers handlers = {
		.user = fake,
		.launch_pak = fake_launch,
		.exit_to_home = fake_home,
		.open_overlay = fake_overlay,
	};
	return handlers;
}

static void reset(FakeHandlers* fake, int result)
{
	memset(fake, 0, sizeof(*fake));
	fake->result = result;
}

static void test_parse(void)
{
	const char* arg = (const char*)0x1;
	CHECK(fnActionParse(NULL, &arg) == FN_ACTION_KIND_NONE);
	CHECK(arg == NULL);

	arg = (const char*)0x1;
	CHECK(fnActionParse("", &arg) == FN_ACTION_KIND_NONE);
	CHECK(arg == NULL);

	CHECK(fnActionParse("none", NULL) == FN_ACTION_KIND_NONE);
	CHECK(fnActionParse("home", NULL) == FN_ACTION_KIND_HOME);
	CHECK(fnActionParse("overlay", NULL) == FN_ACTION_KIND_OVERLAY);

	CHECK(fnActionParse("pak:Bootlogo.pak", &arg) == FN_ACTION_KIND_PAK);
	CHECK(arg && strcmp(arg, "Bootlogo.pak") == 0);

	CHECK(fnActionParse("pak:group/Tool.pak", &arg) == FN_ACTION_KIND_PAK);
	CHECK(arg && strcmp(arg, "group/Tool.pak") == 0);

	CHECK(fnActionParse("pak:", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:/absolute.pak", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:../escape.pak", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a/../../b", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a/./b", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a//b", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a/", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a\\b", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak:a\nb", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("pak", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("package:Tool.pak", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("HOME", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("home:1", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("home ", NULL) == FN_ACTION_KIND_UNKNOWN);
	CHECK(fnActionParse("overlay:1", NULL) == FN_ACTION_KIND_UNKNOWN);
}

static void test_defaults(void)
{
	CHECK(strcmp(fnActionDefault(-1), "") == 0);
	CHECK(strcmp(fnActionDefault(0), "") == 0);
	CHECK(strcmp(fnActionDefault(1), FN_ACTION_HOME) == 0);
	CHECK(strcmp(fnActionDefault(2), FN_ACTION_OVERLAY) == 0);
	CHECK(strcmp(fnActionDefault(3), "") == 0);

	// Legacy empty values must regain the per-slot defaults.
	CHECK(strcmp(fnActionEffective(0, NULL), "") == 0);
	CHECK(strcmp(fnActionEffective(1, NULL), FN_ACTION_HOME) == 0);
	CHECK(strcmp(fnActionEffective(2, NULL), FN_ACTION_OVERLAY) == 0);
	CHECK(strcmp(fnActionEffective(1, ""), FN_ACTION_HOME) == 0);
	CHECK(strcmp(fnActionEffective(2, ""), FN_ACTION_OVERLAY) == 0);
	CHECK(strcmp(fnActionEffective(3, ""), "") == 0);

	// Explicit values, including "none", must never fall back.
	CHECK(strcmp(fnActionEffective(1, FN_ACTION_NONE), FN_ACTION_NONE) == 0);
	CHECK(strcmp(fnActionEffective(1, "pak:Tool.pak"), "pak:Tool.pak") == 0);
	CHECK(strcmp(fnActionEffective(2, FN_ACTION_HOME), FN_ACTION_HOME) == 0);
	CHECK(strcmp(fnActionEffective(2, "made-up-kind"), "made-up-kind") == 0);
}

static void test_dispatch_desktop(void)
{
	FakeHandlers fake;
	FnActionHandlers handlers;

	reset(&fake, 1);
	handlers = all_handlers(&fake);

	CHECK(fnActionDispatch("none", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_NONE);
	CHECK(fnActionDispatch("", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_NONE);
	CHECK(fake.launch_calls == 0 && fake.home_calls == 0 && fake.overlay_calls == 0);

	CHECK(fnActionDispatch("pak:Bootlogo.pak", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_HANDLED);
	CHECK(fake.launch_calls == 1);
	CHECK(strcmp(fake.last_path, "Bootlogo.pak") == 0);
	CHECK(fake.home_calls == 0 && fake.overlay_calls == 0);

	// Recognised app-runtime actions are no-ops in the launcher, even when the
	// caller happens to provide app-runtime handlers: they must not fall
	// through to the pak path.
	reset(&fake, 1);
	handlers = all_handlers(&fake);
	CHECK(fnActionDispatch(FN_ACTION_HOME, FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fnActionDispatch(FN_ACTION_OVERLAY, FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fake.launch_calls == 0);
	CHECK(fake.home_calls == 0 && fake.overlay_calls == 0);

	CHECK(fnActionDispatch("made-up-kind", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_UNKNOWN);
	CHECK(fake.launch_calls == 0);

	// Unknown/invalid pak paths must not reach the launch handler.
	CHECK(fnActionDispatch("pak:../escape.pak", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_UNKNOWN);
	CHECK(fake.launch_calls == 0);
}

static void test_dispatch_app(void)
{
	FakeHandlers fake;
	FnActionHandlers handlers;

	reset(&fake, 1);
	handlers = all_handlers(&fake);

	CHECK(fnActionDispatch(FN_ACTION_HOME, FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_HANDLED);
	CHECK(fake.home_calls == 1);
	CHECK(fake.launch_calls == 0 && fake.overlay_calls == 0);

	CHECK(fnActionDispatch(FN_ACTION_OVERLAY, FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_HANDLED);
	CHECK(fake.overlay_calls == 1);
	CHECK(fake.launch_calls == 0 && fake.home_calls == 1);

	// A pak action is launcher-owned and must not run inside an app.
	CHECK(fnActionDispatch("pak:Bootlogo.pak", FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fake.launch_calls == 0);

	CHECK(fnActionDispatch("none", FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_NONE);
	CHECK(fnActionDispatch("made-up-kind", FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_UNKNOWN);
}

static void test_dispatch_unsupported_and_failure(void)
{
	FakeHandlers fake;

	reset(&fake, 1);
	CHECK(fnActionDispatch(FN_ACTION_HOME, FN_ACTION_SCOPE_APP, NULL) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fnActionDispatch(FN_ACTION_OVERLAY, FN_ACTION_SCOPE_APP, NULL) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fnActionDispatch("pak:Tool.pak", FN_ACTION_SCOPE_DESKTOP, NULL) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);

	{
		FnActionHandlers handlers = all_handlers(&fake);
		CHECK(fnActionDispatch("pak:Tool.pak", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
			  FN_ACTION_DISPATCH_HANDLED);
	}
	reset(&fake, 0);
	{
		FnActionHandlers handlers = all_handlers(&fake);
		CHECK(fnActionDispatch("pak:Tool.pak", FN_ACTION_SCOPE_DESKTOP, &handlers) ==
			  FN_ACTION_DISPATCH_FAILED);
		CHECK(fake.launch_calls == 1);
		CHECK(fnActionDispatch(FN_ACTION_HOME, FN_ACTION_SCOPE_APP, &handlers) ==
			  FN_ACTION_DISPATCH_FAILED);
		CHECK(fake.home_calls == 1);
	}
}

static void test_legacy_fallback_routes(void)
{
	FakeHandlers fake;
	FnActionHandlers handlers;

	// An empty FN2 must behave like the default "home" binding.
	reset(&fake, 1);
	handlers = all_handlers(&fake);
	CHECK(fnActionDispatch(fnActionEffective(1, ""), FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_HANDLED);
	CHECK(fake.home_calls == 1);
	CHECK(fake.launch_calls == 0);

	// Desktop scope still refuses it.
	reset(&fake, 1);
	handlers = all_handlers(&fake);
	CHECK(fnActionDispatch(fnActionEffective(1, ""), FN_ACTION_SCOPE_DESKTOP, &handlers) ==
		  FN_ACTION_DISPATCH_UNSUPPORTED);
	CHECK(fake.home_calls == 0 && fake.launch_calls == 0);

	// Explicit none must not resurrect the default.
	reset(&fake, 1);
	handlers = all_handlers(&fake);
	CHECK(fnActionDispatch(fnActionEffective(1, FN_ACTION_NONE), FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_NONE);
	CHECK(fake.home_calls == 0);

	// An unknown stored kind stays unknown.
	reset(&fake, 1);
	handlers = all_handlers(&fake);
	CHECK(fnActionDispatch(fnActionEffective(1, "made-up-kind"), FN_ACTION_SCOPE_APP, &handlers) ==
		  FN_ACTION_DISPATCH_UNKNOWN);
	CHECK(fake.home_calls == 0);
}

int main(void)
{
	test_parse();
	test_defaults();
	test_dispatch_desktop();
	test_dispatch_app();
	test_dispatch_unsupported_and_failure();
	test_legacy_fallback_routes();

	if (failures)
	{
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}

	printf("fn_action_test: all checks passed\n");
	return 0;
}
