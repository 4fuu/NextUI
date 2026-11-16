/*
 * Host-only wiring test for the launcher's HOME action. It includes the real
 * nextui.c (renaming its main) so the test calls the same runFnAction() and
 * shared UIOverlay instance the launcher uses. No device input is involved.
 */
#define main nextui_launcher_main
#include "../nextui.c"
#undef main

#include <stdio.h>

int main(void)
{
	int failed = 0;

	home_overlay_init();
	if (!home_overlay)
	{
		fprintf(stderr, "launcher overlay could not be created\n");
		return 1;
	}

	/* FN2's default home action has no foreground app on the desktop. */
	if (runFnAction(1))
	{
		fprintf(stderr, "desktop FN2 must remain a no-op\n");
		failed = 1;
	}

	/* Side HOME's default overlay action is owned by the launcher itself. */
	if (!runFnAction(2))
	{
		fprintf(stderr, "desktop HOME action was not handled\n");
		failed = 1;
	}
	if (!UIOverlay_isOpen(home_overlay))
	{
		fprintf(stderr, "desktop HOME must open the shared overlay\n");
		failed = 1;
	}

	if (!runFnAction(2) || UIOverlay_isOpen(home_overlay))
	{
		fprintf(stderr, "second desktop HOME must close the overlay\n");
		failed = 1;
	}

	UIOverlay_destroy(home_overlay);
	home_overlay = NULL;
	if (!failed) printf("desktop HOME overlay wiring passed\n");
	return failed ? 1 : 0;
}
