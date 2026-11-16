/*
 * Host-only regression test for the launcher's app entries after the Pak Store
 * special-casing was removed. It includes the real nextui.c (renaming its main)
 * and redirects the desktop platform's SD root to an isolated directory, so:
 *
 *   - a normal Tools/<platform>/Foo.pak is still discovered as a launchable app
 *   - an existing user-installed "Pak Store.pak" is still discovered as an
 *     ordinary pak: the build no longer preinstalls a store, but a card that
 *     already has one keeps it, listed and launched like any other app
 *   - the quick-menu toggle list keeps Settings and the system toggles and no
 *     longer carries a special Pak Store entry
 *
 * The quick-menu icon branch for "Pak Store" is asserted at the source level by
 * tests/pak_store_preinstall_test.py, which also proves the build no longer
 * downloads or preinstalls the store.
 *
 * No device, no real SD card, no network: the fake root lives in a temporary
 * directory the runner creates and removes.
 */
#include <sys/stat.h>
#include <sys/types.h>

#include <stdio.h>
#include <string.h>

#include "platform.h"
#undef SDCARD_PATH
#define SDCARD_PATH SD_TEST_ROOT

#define main nextui_launcher_main
#include "../nextui.c"
#undef main

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

static void makeDir(const char* path)
{
	char partial[512];
	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(partial)) return;
	strcpy(partial, path);
	for (char* slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/'))
	{
		*slash = '\0';
		mkdir(partial, 0777);
		*slash = '/';
	}
	mkdir(partial, 0777);
}

// Creates <root>/Tools/<platform>/<name>.pak/launch.sh, the shape the generic
// Tools discovery requires (a directory with a launch script).
static void makePak(const char* name)
{
	char path[512];
	int len = snprintf(path, sizeof(path), "%s/Tools/%s/%s.pak", SDCARD_PATH, PLATFORM, name);
	if (len <= 0 || (size_t)len >= sizeof(path)) return;
	makeDir(path);

	char launch[600];
	len = snprintf(launch, sizeof(launch), "%s/launch.sh", path);
	if (len <= 0 || (size_t)len >= sizeof(launch)) return;
	FILE* file = fopen(launch, "w");
	if (file)
	{
		fputs("#!/bin/sh\n", file);
		fclose(file);
	}
}

static int hasEntryNamed(Array* entries, const char* name)
{
	for (int i = 0; i < entries->count; i++)
	{
		Entry* entry = entries->items[i];
		if (!strcmp(entry->name, name)) return 1;
	}
	return 0;
}

static Entry* entryNamed(Array* entries, const char* name)
{
	for (int i = 0; i < entries->count; i++)
	{
		Entry* entry = entries->items[i];
		if (!strcmp(entry->name, name)) return entry;
	}
	return NULL;
}

int main(void)
{
	makePak("Settings");
	makePak("Foo");
	makePak("Pak Store");

	// Generic discovery: every pak under Tools/<platform> is listed, including
	// an existing user-installed store, which is just an ordinary pak now.
	Array* apps = getApps();
	CHECK(apps->count == 3);
	Entry* foo = entryNamed(apps, "Foo");
	CHECK(foo != NULL);
	if (foo)
	{
		CHECK(foo->type == ENTRY_PAK);
		char expected[512];
		int len = snprintf(expected, sizeof(expected), "%s/Tools/%s/Foo.pak", SDCARD_PATH, PLATFORM);
		CHECK(len > 0 && (size_t)len < sizeof(expected) && !strcmp(foo->path, expected));
	}
	Entry* store = entryNamed(apps, "Pak Store");
	CHECK(store != NULL);
	if (store) CHECK(store->type == ENTRY_PAK);
	CHECK(entryNamed(apps, "Settings") != NULL);
	EntryArray_free(apps);

	// Quick toggles: Settings stays, the system toggles stay, and the special
	// Pak Store entry is gone even though the pak exists on the fake root.
	Array* quick = getQuickToggles();
	CHECK(hasEntryNamed(quick, "Settings"));
	CHECK(!hasEntryNamed(quick, "Pak Store"));
	CHECK(hasEntryNamed(quick, "Wifi"));
	CHECK(hasEntryNamed(quick, "Reboot"));
	CHECK(hasEntryNamed(quick, "Poweroff"));
	EntryArray_free(quick);

	if (!failures)
		printf("pak store entry: generic Tools discovery intact, no special quick entry\n");
	return failures ? 1 : 0;
}
