// Real-font regression checks for UTF-8 text layout.
//
// This binary links the real workspace/all/common/api.c and SDL2_ttf with
// skeleton/SYSTEM/res/font1.ttf (24 px), matching the baseline harness that
// reproduced the original byte-truncation and no-wrap defects.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"

static int failures = 0;

#define CHECK(condition, ...)                         \
	do                                                \
	{                                                 \
		if (!(condition))                             \
		{                                             \
			printf("FAIL: ");                         \
			printf(__VA_ARGS__);                      \
			printf("\n");                             \
			failures += 1;                            \
		}                                             \
	} while (0)

static int test_utf8_valid(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	while (*p)
	{
		int len;
		if (*p < 0x80)
			len = 1;
		else if ((*p & 0xE0) == 0xC0)
			len = 2;
		else if ((*p & 0xF0) == 0xE0)
			len = 3;
		else if ((*p & 0xF8) == 0xF0)
			len = 4;
		else
			return 0;
		for (int i = 1; i < len; i++)
		{
			if ((p[i] & 0xC0) != 0x80)
				return 0;
		}
		p += len;
	}
	return 1;
}

static int test_line_count(const char *text)
{
	int count = 1;
	for (; *text; text++)
	{
		if (*text == '\n')
			count += 1;
	}
	return count;
}

static int test_measure(TTF_Font *font, const char *text)
{
	int width = 0;
	if (TTF_SizeUTF8(font, text, &width, NULL) != 0)
		return -1;
	return width;
}

static int test_max_line_width(TTF_Font *font, const char *text)
{
	int max_width = 0;
	const char *line = text;
	while (1)
	{
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		char *buffer = malloc(len + 1);
		if (!buffer)
			return -1;
		memcpy(buffer, line, len);
		buffer[len] = '\0';
		int width = test_measure(font, buffer);
		free(buffer);
		if (width > max_width)
			max_width = width;
		if (!end)
			break;
		line = end + 1;
	}
	return max_width;
}

static int test_same_text_ignoring_newlines(const char *a, const char *b)
{
	while (*a || *b)
	{
		while (*a == '\n')
			a += 1;
		while (*b == '\n')
			b += 1;
		if (*a != *b)
			return 0;
		if (*a)
		{
			a += 1;
			b += 1;
		}
	}
	return 1;
}

static int test_same_text_ignoring_breaks(const char *a, const char *b)
{
	while (*a || *b)
	{
		while (*a == '\n' || *a == ' ')
			a += 1;
		while (*b == '\n' || *b == ' ')
			b += 1;
		if (*a != *b)
			return 0;
		if (*a)
		{
			a += 1;
			b += 1;
		}
	}
	return 1;
}

static void print_bytes(const char *label, const char *text)
{
	printf("%s:", label);
	for (const unsigned char *p = (const unsigned char *)text; *p; p++)
		printf(" %02X", *p);
	printf("\n");
}

static void test_truncate(TTF_Font *font)
{
	const char *text = "设置中文";
	const int widths[] = {0, 5, 10, 20, 40, 45, 50, 65, 70, 75};

	for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
	{
		char output[256];
		int width = GFX_truncateText(font, text, output, widths[i], 0);
		int measured = test_measure(font, output);

		printf("truncate width=%3d -> '%s' (%d px)\n", widths[i], output, width);
		CHECK(test_utf8_valid(output), "truncate at width %d produced invalid UTF-8", widths[i]);
		CHECK(width == measured, "truncate at width %d returned %d, measured %d", widths[i], width, measured);

		if (strcmp(output, text) != 0)
		{
			size_t len = strlen(output);
			CHECK(len >= 3 && strcmp(output + len - 3, "...") == 0,
				  "truncate at width %d did not end in an ellipsis", widths[i]);
			if (len >= 3)
				CHECK(strncmp(output, text, len - 3) == 0,
					  "truncate at width %d did not keep a prefix", widths[i]);
			CHECK(width <= widths[i] || strcmp(output, "...") == 0,
				  "truncate at width %d overflowed: %d px", widths[i], width);
		}

		if (!test_utf8_valid(output))
			print_bytes("invalid output", output);
	}

	char empty[16];
	memset(empty, 0xA5, sizeof(empty));
	int empty_width = GFX_truncateText(font, "", empty, 0, 0);
	CHECK(strcmp(empty, "") == 0 && empty_width == 0, "empty string truncation changed");
	CHECK((unsigned char)empty[sizeof(empty) - 1] == 0xA5, "empty string truncation wrote past the buffer");
}

static void test_wrap(TTF_Font *font)
{
	const char *text = "设置里的语言与字体选择需要重启";
	const int widths[] = {60, 80, 100, 140, 180};
	size_t len = strlen(text);

	for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
	{
		char *wrapped = malloc(len * 2 + 1);
		if (!wrapped)
			return;
		strcpy(wrapped, text);

		int max_width = GFX_wrapText(font, wrapped, widths[i], 0);
		int lines = test_line_count(wrapped);
		int measured_max = test_max_line_width(font, wrapped);

		printf("wrap width=%3d -> lines=%d max=%d px: '%s'\n", widths[i], lines, measured_max, wrapped);
		CHECK(test_utf8_valid(wrapped), "wrap at width %d produced invalid UTF-8", widths[i]);
		CHECK(lines > 1, "wrap at width %d did not wrap CJK text", widths[i]);
		CHECK(test_same_text_ignoring_newlines(wrapped, text), "wrap at width %d lost characters", widths[i]);
		CHECK(measured_max <= widths[i], "wrap at width %d overflowed to %d px", widths[i], measured_max);
		CHECK(max_width == measured_max, "wrap at width %d returned %d, measured %d", widths[i], max_width, measured_max);

		if (!test_utf8_valid(wrapped))
			print_bytes("invalid output", wrapped);
		free(wrapped);
	}
}

static void test_wrap_limits(TTF_Font *font)
{
	const char *text = "设置里的语言与字体选择需要重启";
	size_t len = strlen(text);

	char *one = malloc(len * 2 + 1);
	char *two = malloc(len * 2 + 1);
	if (!one || !two)
	{
		free(one);
		free(two);
		return;
	}
	strcpy(one, text);
	strcpy(two, text);
	GFX_wrapText(font, one, 100, 1);
	GFX_wrapText(font, two, 100, 2);

	printf("wrap max_lines=1 -> '%s'\n", one);
	printf("wrap max_lines=2 -> '%s'\n", two);
	CHECK(test_line_count(one) == 1, "max_lines=1 produced %d lines", test_line_count(one));
	CHECK(test_line_count(two) == 2, "max_lines=2 produced %d lines", test_line_count(two));
	CHECK(test_utf8_valid(one), "max_lines=1 produced invalid UTF-8");
	CHECK(test_utf8_valid(two), "max_lines=2 produced invalid UTF-8");
	CHECK(test_max_line_width(font, one) <= 100, "max_lines=1 overflowed");
	CHECK(test_max_line_width(font, two) <= 100, "max_lines=2 overflowed");

	size_t one_len = strlen(one);
	size_t two_len = strlen(two);
	CHECK(one_len >= 3 && strcmp(one + one_len - 3, "...") == 0, "max_lines=1 should end in an ellipsis");
	CHECK(two_len >= 3 && strcmp(two + two_len - 3, "...") == 0, "max_lines=2 should end in an ellipsis");

	free(one);
	free(two);
}

static void test_mixed(TTF_Font *font)
{
	const char *text = "中文 Mixed 文本 Test";
	size_t len = strlen(text);
	char *wrapped = malloc(len * 2 + 1);
	if (!wrapped)
		return;
	strcpy(wrapped, text);

	GFX_wrapText(font, wrapped, 120, 0);
	printf("wrap mixed width=120 -> '%s'\n", wrapped);
	CHECK(test_utf8_valid(wrapped), "mixed wrap produced invalid UTF-8");
	CHECK(test_line_count(wrapped) > 1, "mixed wrap did not wrap");
	CHECK(test_same_text_ignoring_breaks(wrapped, text), "mixed wrap lost characters");
	CHECK(test_max_line_width(font, wrapped) <= 120, "mixed wrap overflowed");
	free(wrapped);
}

int main(int argc, char **argv)
{
	const char *font_path = argc > 1 ? argv[1] : "skeleton/SYSTEM/res/font1.ttf";

	if (TTF_Init() != 0)
	{
		fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
		return 1;
	}

	TTF_Font *font = TTF_OpenFont(font_path, 24);
	if (!font)
	{
		fprintf(stderr, "could not open %s: %s\n", font_path, TTF_GetError());
		TTF_Quit();
		return 1;
	}

	printf("real-font text layout tests (%s, 24 px)\n", font_path);
	test_truncate(font);
	test_wrap(font);
	test_wrap_limits(font);
	test_mixed(font);

	TTF_CloseFont(font);
	TTF_Quit();

	if (failures)
	{
		printf("%d test(s) failed\n", failures);
		return 1;
	}
	printf("all real-font text layout tests passed\n");
	return 0;
}
