/*
 * Host-only test for the launcher's Simplified Chinese labels on the first
 * batch of built-in Tools paks.
 *
 * It includes the real nextui.c with its SD root redirected to a temporary
 * directory, seeds the eight shipped pak directories with their real manifest
 * names, and drives the real discovery path. The language is switched through
 * the shared settings struct instead of a settings file, so no device or
 * userdata directory is touched.
 *
 * The English run is the control: the same entries must keep their on-disk
 * manifest names, and an unrelated third-party pak must stay untouched in both
 * languages.
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

static void makePak(const char* name, const char* manifest_name, const char* id)
{
	char path[512];
	int len = snprintf(path, sizeof(path), "%s/Tools/%s/%s.pak", SDCARD_PATH, PLATFORM, name);
	if (len <= 0 || (size_t)len >= sizeof(path)) return;
	makeDir(path);

	char launch[600];
	len = snprintf(launch, sizeof(launch), "%s/launch.sh", path);
	if (len > 0 && (size_t)len < sizeof(launch))
	{
		FILE* file = fopen(launch, "w");
		if (file)
		{
			fputs("#!/bin/sh\n", file);
			fclose(file);
		}
	}

	char manifest[700];
	len = snprintf(manifest, sizeof(manifest), "%s/pak.json", path);
	if (len > 0 && (size_t)len < sizeof(manifest))
	{
		FILE* file = fopen(manifest, "w");
		if (file)
		{
			fprintf(file,
			        "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"v1.0.0\","
			        "\"capabilities\":[]}\n",
			        id, manifest_name);
			fclose(file);
		}
	}
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

static int hasEnglishBuiltinNames(Array* entries)
{
	static const char* names[] = {
		"Clock", "Settings", "LedControl", "Battery",
		"Input", "Bootlogo", "Files", "Remove Loading",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!entryNamed(entries, names[i])) return 0;
	return 1;
}

int main(void)
{
	static const struct {
		const char* dir;
		const char* manifest;
		const char* id;
	} builtins[] = {
		{"Clock", "Clock", "nextui.clock"},
		{"Settings", "Settings", "nextui.settings"},
		{"LedControl", "LedControl", "nextui.ledcontrol"},
		{"Battery", "Battery", "nextui.battery"},
		{"Input", "Input", "nextui.input"},
		{"Bootlogo", "Bootlogo", "nextui.bootlogo"},
		{"Files", "Files", "nextui.files"},
		{"Remove Loading", "Remove Loading", "nextui.remove-loading"},
	};
	static const char* zh_names[] = {
		"时钟", "设置", "灯光控制", "电池",
		"按键测试", "开机画面", "文件", "移除加载画面",
	};

	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
		makePak(builtins[i].dir, builtins[i].manifest, builtins[i].id);
	makePak("Foo", "Foo", "example.foo");

	extern NextUISettings settings;
	strcpy(settings.language, "zh");

	Array* zh = getApps();
	CHECK(zh->count == 9);
	for (size_t i = 0; i < sizeof(zh_names) / sizeof(zh_names[0]); i++)
		CHECK(entryNamed(zh, zh_names[i]) != NULL);
	CHECK(entryNamed(zh, "Clock") == NULL);
	CHECK(entryNamed(zh, "Settings") == NULL);
	CHECK(entryNamed(zh, "Foo") != NULL);

	// The path and the launcher command stay on the English directory.
	Entry* clock = entryNamed(zh, "时钟");
	if (clock)
	{
		char expected[512];
		int len = snprintf(expected, sizeof(expected), "%s/Tools/%s/Clock.pak",
		                   SDCARD_PATH, PLATFORM);
		CHECK(len > 0 && (size_t)len < sizeof(expected) && !strcmp(clock->path, expected));
		CHECK(clock->type == ENTRY_PAK);
	}
	EntryArray_free(zh);

	strcpy(settings.language, "en");
	Array* en = getApps();
	CHECK(en->count == 9);
	CHECK(hasEnglishBuiltinNames(en));
	CHECK(entryNamed(en, "时钟") == NULL);
	CHECK(entryNamed(en, "Foo") != NULL);
	EntryArray_free(en);

	if (!failures)
		printf("builtin names: zh labels localized, en labels and paths unchanged\n");
	return failures ? 1 : 0;
}
