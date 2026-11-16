// Host-side regression tests for UTF-8 text layout.
//
// This binary links the real workspace/all/common/api.c and replaces the
// SDL_ttf metrics and rendering calls with deterministic stubs, so it needs
// no font files, no display and no SDL video initialization.
//
// Build and run from the repository root after `make setup`:
//   make -C workspace/all/common/tests run
//
// The stubs assign 10 pixels to ASCII and 20 pixels to CJK characters, which
// makes every wrap/truncate boundary explicit in the assertions.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"

#define ASCII_WIDTH 10
#define CJK_WIDTH 20
#define TEST_LINE_HEIGHT 10

static int failures = 0;
static int blended_calls = 0;
static int wrapped_calls = 0;

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

static int test_is_cjk(uint32_t cp)
{
	return (cp >= 0x1100 && cp <= 0x11FF) ||
		   (cp >= 0x2E80 && cp <= 0x303F) ||
		   (cp >= 0x3040 && cp <= 0x33FF) ||
		   (cp >= 0x3400 && cp <= 0x4DBF) ||
		   (cp >= 0x4E00 && cp <= 0x9FFF) ||
		   (cp >= 0xA960 && cp <= 0xA97F) ||
		   (cp >= 0xAC00 && cp <= 0xD7FF) ||
		   (cp >= 0xF900 && cp <= 0xFAFF) ||
		   (cp >= 0xFE30 && cp <= 0xFE4F) ||
		   (cp >= 0xFF00 && cp <= 0xFFEF) ||
		   (cp >= 0x20000 && cp <= 0x2FA1F);
}

static int test_utf8_decode(const char *str, uint32_t *codepoint)
{
	const unsigned char *s = (const unsigned char *)str;
	unsigned char c = s[0];
	int len;
	uint32_t cp;

	if (c < 0x80)
	{
		*codepoint = c;
		return 1;
	}
	if ((c & 0xE0) == 0xC0)
	{
		cp = c & 0x1F;
		len = 2;
	}
	else if ((c & 0xF0) == 0xE0)
	{
		cp = c & 0x0F;
		len = 3;
	}
	else if ((c & 0xF8) == 0xF0)
	{
		cp = c & 0x07;
		len = 4;
	}
	else
	{
		*codepoint = c;
		return 1;
	}

	for (int i = 1; i < len; i++)
	{
		if ((s[i] & 0xC0) != 0x80)
		{
			*codepoint = c;
			return 1;
		}
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	*codepoint = cp;
	return len;
}

static int test_measure(const char *text)
{
	int width = 0;
	while (*text)
	{
		uint32_t cp;
		text += test_utf8_decode(text, &cp);
		if (cp != '\n')
			width += test_is_cjk(cp) ? CJK_WIDTH : ASCII_WIDTH;
	}
	return width;
}

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

static int test_max_line_width(const char *text)
{
	int max_width = 0;
	const char *line = text;
	while (1)
	{
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		char buffer[4096];
		if (len >= sizeof(buffer))
			len = sizeof(buffer) - 1;
		memcpy(buffer, line, len);
		buffer[len] = '\0';
		int width = test_measure(buffer);
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

///////////////////////////////
// Deterministic SDL_ttf stubs

TTF_Font *TTF_OpenFont(const char *file, int ptsize)
{
	(void)file;
	(void)ptsize;
	return (TTF_Font *)1;
}

void TTF_CloseFont(TTF_Font *font)
{
	(void)font;
}

int TTF_Init(void)
{
	return 0;
}

int TTF_WasInit(void)
{
	return 1;
}

void TTF_SetFontStyle(TTF_Font *font, int style)
{
	(void)font;
	(void)style;
}

int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h)
{
	(void)font;
	if (w)
		*w = test_measure(text);
	if (h)
		*h = TEST_LINE_HEIGHT;
	return 0;
}

int TTF_FontHeight(const TTF_Font *font)
{
	(void)font;
	return TEST_LINE_HEIGHT;
}

static SDL_Surface *test_text_surface(const char *text, int max_width)
{
	int width = test_measure(text);
	if (max_width > 0 && width > max_width)
		width = max_width;
	if (width < 1)
		width = 1;

	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, TEST_LINE_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return NULL;
	SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
	return surface;
}

SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg)
{
	(void)font;
	(void)fg;
	blended_calls += 1;
	return test_text_surface(text, 0);
}

SDL_Surface *TTF_RenderUTF8_Blended_Wrapped(TTF_Font *font, const char *text, SDL_Color fg, Uint32 wrapLength)
{
	(void)font;
	(void)fg;
	wrapped_calls += 1;
	return test_text_surface(text, (int)wrapLength);
}

///////////////////////////////
// Legacy reference implementations
//
// These are copies of the pre-fix space-delimited algorithms. They let the
// tests assert that English output and return values did not change.

static int legacy_truncate_text(TTF_Font *font, const char *in_name, char *out_name, int max_width, int padding)
{
	int text_width;
	strcpy(out_name, in_name);
	TTF_SizeUTF8(font, out_name, &text_width, NULL);
	text_width += padding;

	while (text_width > max_width)
	{
		int len = strlen(out_name);
		if (len <= 3)
			break; // avoid the original underflow while comparing ASCII
		strcpy(&out_name[len - 4], "...\0");
		TTF_SizeUTF8(font, out_name, &text_width, NULL);
		text_width += padding;
	}

	return text_width;
}

static int legacy_wrap_text(TTF_Font *font, char *str, int max_width, int max_lines)
{
	int line_width;
	int max_line_width = 0;
	char *line = str;
	char buffer[MAX_PATH];

	TTF_SizeUTF8(font, line, &line_width, NULL);
	if (line_width <= max_width)
	{
		line_width = legacy_truncate_text(font, line, buffer, max_width, 0);
		strcpy(line, buffer);
		return line_width;
	}

	char *prev = NULL;
	char *tmp = line;
	int lines = 1;
	while (!max_lines || lines < max_lines)
	{
		tmp = strchr(tmp, ' ');
		if (!tmp)
		{
			if (prev)
			{
				TTF_SizeUTF8(font, line, &line_width, NULL);
				if (line_width >= max_width)
				{
					if (line_width > max_line_width)
						max_line_width = line_width;
					prev[0] = '\n';
					line = prev + 1;
				}
			}
			break;
		}
		tmp[0] = '\0';

		TTF_SizeUTF8(font, line, &line_width, NULL);

		if (line_width >= max_width)
		{
			if (line_width > max_line_width)
				max_line_width = line_width;
			tmp[0] = ' ';
			tmp += 1;
			prev[0] = '\n';
			prev += 1;
			line = prev;
			lines += 1;
		}
		else
		{
			tmp[0] = ' ';
			prev = tmp;
			tmp += 1;
		}
	}

	line_width = legacy_truncate_text(font, line, buffer, max_width, 0);
	strcpy(line, buffer);

	if (line_width > max_line_width)
		max_line_width = line_width;
	return max_line_width;
}

static void legacy_blit_message(TTF_Font *font, char *msg, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	if (!dst_rect)
		dst_rect = &(SDL_Rect){0, 0, dst->w, dst->h};

	SDL_Surface *text;
#define TEXT_BOX_MAX_ROWS 16
	char *rows[TEXT_BOX_MAX_ROWS];
	int row_count = 0;

	char *tmp;
	rows[row_count++] = msg;
	while ((tmp = strchr(rows[row_count - 1], '\n')) != NULL)
	{
		if (row_count + 1 >= TEXT_BOX_MAX_ROWS)
			return;
		rows[row_count++] = tmp + 1;
	}

	int line_height = TTF_FontHeight(font);
	int rendered_height = line_height * row_count;
	int y = dst_rect->y;
	y += (dst_rect->h - rendered_height) / 2;

	char line[256];
	for (int i = 0; i < row_count; i++)
	{
		int len;
		if (i + 1 < row_count)
		{
			len = rows[i + 1] - rows[i] - 1;
			if (len)
				strncpy(line, rows[i], len);
			line[len] = '\0';
		}
		else
		{
			len = strlen(rows[i]);
			strcpy(line, rows[i]);
		}

		if (len)
		{
			text = TTF_RenderUTF8_Blended_Wrapped(font, line, uintToColour(CFG_getColor(COLOR_LIST_TEXT)), dst_rect->w);
			int x = dst_rect->x;
			x += (dst_rect->w - text->w) / 2;
			SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){x, y});
			SDL_FreeSurface(text);
		}
		y += line_height;
	}
}

///////////////////////////////
// Tests

static void test_truncate_english_unchanged(void)
{
	const char *texts[] = {
		"Super Nintendo Entertainment System",
		"The quick brown fox jumps over the lazy dog",
		"Save state and return to the main menu",
		"Recently Played",
		"Empty folder",
	};
	const int widths[] = {100, 120, 160, 200, 300, 400};

	for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
	{
		for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
		{
			for (int padding = 0; padding <= 16; padding += 16)
			{
				char actual[512];
				char expected[512];
				memset(actual, 0xA5, sizeof(actual));
				memset(expected, 0xA5, sizeof(expected));

				int actual_width = GFX_truncateText((TTF_Font *)1, texts[t], actual, widths[w], padding);
				int expected_width = legacy_truncate_text((TTF_Font *)1, texts[t], expected, widths[w], padding);

				CHECK(strcmp(actual, expected) == 0, "truncate changed ASCII output for '%s' at width %d", texts[t], widths[w]);
				CHECK(actual_width == expected_width, "truncate changed ASCII width for '%s' at width %d", texts[t], widths[w]);
				CHECK((unsigned char)actual[sizeof(actual) - 1] == 0xA5, "truncate wrote past its output buffer");
			}
		}
	}
}

static void test_truncate_utf8(void)
{
	const char *texts[] = {
		"这是中文测试文本",
		"中文 Mixed 文本 Test",
		"设置语言和应用",
	};
	const int widths[] = {40, 60, 80, 100, 140, 160};

	for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
	{
		for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
		{
			char output[512];
			memset(output, 0xA5, sizeof(output));
			int width = GFX_truncateText((TTF_Font *)1, texts[t], output, widths[w], 0);

			CHECK(test_utf8_valid(output), "truncate split a UTF-8 sequence for '%s' at width %d: '%s'", texts[t], widths[w], output);
			CHECK((unsigned char)output[sizeof(output) - 1] == 0xA5, "truncate wrote past its output buffer");

			int text_width = test_measure(output);
			CHECK(width == text_width, "truncate returned %d but measured %d for '%s'", width, text_width, output);

			if (strcmp(output, texts[t]) != 0)
			{
				size_t output_len = strlen(output);
				CHECK(output_len >= 3 && strcmp(output + output_len - 3, "...") == 0,
					  "truncate did not end in an ellipsis for '%s' at width %d: '%s'", texts[t], widths[w], output);
				if (output_len >= 3)
				{
					size_t prefix_len = output_len - 3;
					CHECK(strncmp(output, texts[t], prefix_len) == 0,
						  "truncate did not keep a prefix for '%s' at width %d: '%s'", texts[t], widths[w], output);
				}
				CHECK(text_width <= widths[w] || strcmp(output, "...") == 0,
					  "truncate output '%s' is wider than %d", output, widths[w]);
			}
		}
	}
}

static void test_truncate_short_and_empty(void)
{
	char output[64];

	memset(output, 0xA5, sizeof(output));
	int width = GFX_truncateText((TTF_Font *)1, "", output, 0, 0);
	CHECK(strcmp(output, "") == 0 && width == 0, "empty string truncation changed");
	CHECK((unsigned char)output[sizeof(output) - 1] == 0xA5, "empty string truncation wrote past the buffer");

	memset(output, 0xA5, sizeof(output));
	width = GFX_truncateText((TTF_Font *)1, "", output, 0, 16);
	CHECK(strcmp(output, "") == 0 && width == 16, "empty string truncation with padding changed");
	CHECK((unsigned char)output[sizeof(output) - 1] == 0xA5, "padded empty string truncation wrote past the buffer");

	memset(output, 0xA5, sizeof(output));
	width = GFX_truncateText((TTF_Font *)1, "A", output, 5, 0);
	CHECK(strcmp(output, "A") == 0 && width == ASCII_WIDTH, "single character truncation underflowed");
	CHECK((unsigned char)output[sizeof(output) - 1] == 0xA5, "single character truncation wrote past the buffer");

	memset(output, 0xA5, sizeof(output));
	width = GFX_truncateText((TTF_Font *)1, "中", output, 5, 0);
	CHECK(test_utf8_valid(output), "single CJK character truncation split UTF-8");
	CHECK((unsigned char)output[sizeof(output) - 1] == 0xA5, "single CJK character truncation wrote past the buffer");
}

static void test_wrap_english_unchanged(void)
{
	const char *texts[] = {
		"Super Nintendo Entertainment System",
		"The quick brown fox jumps over the lazy dog",
		"Save state and return to the main menu",
		"Recently Played",
		"Empty folder",
	};
	const int widths[] = {100, 120, 160, 200, 300, 400};
	const int max_lines[] = {0, 2, 16};

	for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
	{
		for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
		{
			for (size_t l = 0; l < sizeof(max_lines) / sizeof(max_lines[0]); l++)
			{
				size_t len = strlen(texts[t]);
				char *actual = malloc(len * 2 + 1);
				char *expected = malloc(len * 2 + 1);
				if (!actual || !expected)
				{
					free(actual);
					free(expected);
					CHECK(0, "out of memory");
					return;
				}
				strcpy(actual, texts[t]);
				strcpy(expected, texts[t]);

				int actual_width = GFX_wrapText((TTF_Font *)1, actual, widths[w], max_lines[l]);
				int expected_width = legacy_wrap_text((TTF_Font *)1, expected, widths[w], max_lines[l]);

				CHECK(strcmp(actual, expected) == 0, "wrap changed ASCII output for '%s' at width %d, max_lines %d: '%s' vs '%s'",
					  texts[t], widths[w], max_lines[l], actual, expected);
				CHECK(actual_width == expected_width, "wrap changed ASCII width for '%s' at width %d, max_lines %d",
					  texts[t], widths[w], max_lines[l]);
				free(actual);
				free(expected);
			}
		}
	}
}

static void test_wrap_chinese(void)
{
	const char *text = "这是中文测试文本用于换行";
	size_t len = strlen(text);

	// 4 characters per line, no line limit.
	char *wrapped = malloc(len * 2 + 1);
	strcpy(wrapped, text);
	int max_width = GFX_wrapText((TTF_Font *)1, wrapped, 80, 0);
	CHECK(test_utf8_valid(wrapped), "Chinese wrap split UTF-8: '%s'", wrapped);
	CHECK(test_line_count(wrapped) == 3, "Chinese wrap should produce 3 lines, got %d: '%s'", test_line_count(wrapped), wrapped);
	CHECK(test_same_text_ignoring_newlines(wrapped, text), "Chinese wrap lost or duplicated characters: '%s'", wrapped);
	CHECK(test_max_line_width(wrapped) == 80, "Chinese wrap max width %d != 80", test_max_line_width(wrapped));
	CHECK(max_width == 80, "Chinese wrap returned %d, expected 80", max_width);

	// max_lines = 2: the second line must be truncated with an ellipsis.
	char *limited = malloc(len * 2 + 1);
	strcpy(limited, text);
	int limited_width = GFX_wrapText((TTF_Font *)1, limited, 100, 2);
	CHECK(test_utf8_valid(limited), "limited Chinese wrap split UTF-8: '%s'", limited);
	CHECK(test_line_count(limited) == 2, "limited Chinese wrap should produce 2 lines, got %d: '%s'", test_line_count(limited), limited);
	size_t limited_len = strlen(limited);
	CHECK(limited_len >= 3 && strcmp(limited + limited_len - 3, "...") == 0,
		  "limited Chinese wrap should end in an ellipsis: '%s'", limited);
	CHECK(test_max_line_width(limited) <= 100, "limited Chinese wrap overflowed: '%s'", limited);
	CHECK(limited_width <= 100, "limited Chinese wrap returned width %d", limited_width);

	// max_lines = 1: one truncated line.
	char *single = malloc(len * 2 + 1);
	strcpy(single, text);
	GFX_wrapText((TTF_Font *)1, single, 80, 1);
	CHECK(test_line_count(single) == 1, "single-line Chinese wrap should produce 1 line: '%s'", single);
	size_t single_len = strlen(single);
	CHECK(single_len >= 3 && strcmp(single + single_len - 3, "...") == 0,
		  "single-line Chinese wrap should end in an ellipsis: '%s'", single);
	CHECK(test_max_line_width(single) <= 80, "single-line Chinese wrap overflowed: '%s'", single);
	CHECK(test_utf8_valid(single), "single-line Chinese wrap split UTF-8: '%s'", single);

	free(wrapped);
	free(limited);
	free(single);
}

static void test_wrap_mixed(void)
{
	const char *text = "中文 English 混排测试";
	size_t len = strlen(text);
	char *wrapped = malloc(len * 2 + 1 + 16);
	memset(wrapped, 0xA5, len * 2 + 1 + 16);
	strcpy(wrapped, text);

	int max_width = GFX_wrapText((TTF_Font *)1, wrapped, 100, 0);
	CHECK(test_utf8_valid(wrapped), "mixed wrap split UTF-8: '%s'", wrapped);
	CHECK(strcmp(wrapped, "中文\nEnglish 混\n排测试") == 0, "mixed wrap layout changed: '%s'", wrapped);
	CHECK(test_same_text_ignoring_breaks(wrapped, text), "mixed wrap lost or duplicated non-space characters: '%s'", wrapped);
	CHECK(test_max_line_width(wrapped) == 100, "mixed wrap max width %d != 100", test_max_line_width(wrapped));
	CHECK(max_width == 100, "mixed wrap returned %d, expected 100", max_width);
	for (size_t i = 0; i < len * 2 + 1 + 16; i++)
	{
		if (i >= len * 2 + 1)
			CHECK(((unsigned char *)wrapped)[i] == 0xA5, "mixed wrap wrote past 2*strlen+1 at byte %zu", i);
	}
	free(wrapped);

	// Explicit newlines are preserved and do not consume an extra line.
	const char *with_newline = "中文第一行\nSecond";
	char *newline_wrapped = malloc(strlen(with_newline) * 2 + 1);
	strcpy(newline_wrapped, with_newline);
	GFX_wrapText((TTF_Font *)1, newline_wrapped, 100, 0);
	CHECK(strcmp(newline_wrapped, with_newline) == 0, "explicit newline changed: '%s'", newline_wrapped);
	free(newline_wrapped);
}

static void test_wrap_capacity_and_invalid_utf8(void)
{
	const char *text = "这是一个很长的中文句子用于验证换行缓冲区边界";
	size_t len = strlen(text);
	size_t cap = len * 2 + 1;
	char *buffer = malloc(cap + 16);
	memset(buffer, 0xA5, cap + 16);
	strcpy(buffer, text);

	GFX_wrapText((TTF_Font *)1, buffer, 40, 0);
	CHECK(test_utf8_valid(buffer), "capacity test split UTF-8: '%s'", buffer);
	CHECK(test_same_text_ignoring_newlines(buffer, text), "capacity test lost characters: '%s'", buffer);
	CHECK(strlen(buffer) < cap, "CJK wrap grew to %zu bytes, past 2*strlen+1 (%zu)", strlen(buffer), cap);
	for (size_t i = 0; i < 16; i++)
	{
		CHECK(((unsigned char *)buffer)[cap + i] == 0xA5, "CJK wrap wrote past 2*strlen+1 at +%zu", i);
	}
	free(buffer);

	// Truncated UTF-8 sequences must not cause reads past the terminator.
	const char *invalid = "中\xE4\xB8";
	size_t invalid_len = strlen(invalid);
	char *invalid_buffer = malloc(invalid_len * 2 + 1);
	strcpy(invalid_buffer, invalid);
	GFX_wrapText((TTF_Font *)1, invalid_buffer, 30, 0);
	CHECK(strcmp(invalid_buffer, "中\n\xE4\xB8") == 0, "truncated UTF-8 layout changed: '%s'", invalid_buffer);
	free(invalid_buffer);

	char short_output[16];
	memset(short_output, 0xA5, sizeof(short_output));
	GFX_truncateText((TTF_Font *)1, "中\xE4", short_output, 5, 0);
	CHECK((unsigned char)short_output[sizeof(short_output) - 1] == 0xA5, "truncating a truncated sequence wrote past the buffer");
}

static int count_text_rows(SDL_Surface *surface, int top, int bottom)
{
	int rows = 0;
	for (int y = top; y <= bottom; y++)
	{
		Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
		for (int x = 0; x < surface->w; x++)
		{
			if (row[x] != 0)
			{
				rows += 1;
				break;
			}
		}
	}
	return rows;
}

static int count_text_bands(SDL_Surface *surface, int top, int bottom, int *max_x)
{
	int bands = 0;
	int in_band = 0;
	*max_x = -1;
	for (int y = top; y <= bottom; y++)
	{
		int row_has_text = 0;
		Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
		for (int x = 0; x < surface->w; x++)
		{
			if (row[x] != 0)
			{
				row_has_text = 1;
				if (x > *max_x)
					*max_x = x;
			}
		}
		if (row_has_text && !in_band)
		{
			bands += 1;
			in_band = 1;
		}
		else if (!row_has_text)
		{
			in_band = 0;
		}
	}
	return bands;
}

static SDL_Surface *new_test_surface(int width, int height)
{
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (surface)
		SDL_FillRect(surface, NULL, 0);
	return surface;
}

static void test_blit_message_english(void)
{
	SDL_Rect rect = {10, 10, 200, 80};
	SDL_Surface *actual = new_test_surface(240, 120);
	SDL_Surface *expected = new_test_surface(240, 120);
	CHECK(actual && expected, "could not create surfaces");

	if (actual && expected)
	{
		blended_calls = 0;
		wrapped_calls = 0;
		GFX_blitMessage((TTF_Font *)1, "No Preview", actual, &rect);
		int new_blended = blended_calls;
		int new_wrapped = wrapped_calls;

		blended_calls = 0;
		wrapped_calls = 0;
		legacy_blit_message((TTF_Font *)1, "No Preview", expected, &rect);
		int old_blended = blended_calls;
		int old_wrapped = wrapped_calls;

		CHECK(new_wrapped == 1 && new_blended == 0, "English message did not use the wrapped renderer");
		CHECK(new_wrapped == old_wrapped && new_blended == old_blended, "English message changed renderer calls");
		CHECK(memcmp(actual->pixels, expected->pixels, (size_t)actual->pitch * actual->h) == 0,
			  "English message pixels changed");
	}

	SDL_FreeSurface(actual);
	SDL_FreeSurface(expected);
}

static void test_blit_message_long_english(void)
{
	char message[301];
	memset(message, 'A', sizeof(message) - 1);
	message[sizeof(message) - 1] = '\0';

	SDL_Rect rect = {0, 0, 4000, 40};
	SDL_Surface *surface = new_test_surface(4000, 40);
	CHECK(surface, "could not create long English surface");
	if (surface)
	{
		blended_calls = 0;
		wrapped_calls = 0;
		GFX_blitMessage((TTF_Font *)1, message, surface, &rect);
		CHECK(wrapped_calls == 1 && blended_calls == 0, "long English message did not use the wrapped renderer");
		int max_x = -1;
		int bands = count_text_bands(surface, 0, 39, &max_x);
		CHECK(bands == 1, "long English message should produce one band, got %d", bands);
		CHECK(max_x < rect.w, "long English message drew outside its rect");
	}
	SDL_FreeSurface(surface);
}

static void test_blit_message_cjk(void)
{
	SDL_Rect rect = {10, 10, 100, 100};
	SDL_Surface *surface = new_test_surface(120, 120);
	CHECK(surface, "could not create CJK surface");
	if (surface)
	{
		blended_calls = 0;
		wrapped_calls = 0;
		GFX_blitMessage((TTF_Font *)1, "这是中文测试消息用于自动换行", surface, &rect);

		CHECK(blended_calls == 3 && wrapped_calls == 0,
			  "CJK message should be pre-wrapped into 3 lines (blended=%d, wrapped=%d)", blended_calls, wrapped_calls);

		int text_rows = count_text_rows(surface, rect.y, rect.y + rect.h - 1);
		CHECK(text_rows == 3 * TEST_LINE_HEIGHT,
			  "CJK message should occupy 3 text lines (%d rows), got %d", 3 * TEST_LINE_HEIGHT, text_rows);

		int max_x = -1;
		int bands = count_text_bands(surface, rect.y, rect.y + rect.h - 1, &max_x);
		CHECK(bands >= 1, "CJK message should produce visible text, got %d bands", bands);
		CHECK(max_x < rect.x + rect.w, "CJK message drew outside its rect");

		for (int y = 0; y < surface->h; y++)
		{
			Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
			for (int x = 0; x < surface->w; x++)
			{
				if (row[x] != 0 && (x < rect.x || x >= rect.x + rect.w || y < rect.y || y >= rect.y + rect.h))
				{
					CHECK(0, "CJK message painted outside dst_rect at (%d,%d)", x, y);
					y = surface->h;
					break;
				}
			}
		}
	}
	SDL_FreeSurface(surface);
}

static void test_blit_message_mixed_and_long(void)
{
	SDL_Rect rect = {0, 0, 100, 60};
	SDL_Surface *surface = new_test_surface(120, 80);
	CHECK(surface, "could not create mixed surface");
	if (surface)
	{
		blended_calls = 0;
		wrapped_calls = 0;
		GFX_blitMessage((TTF_Font *)1, "中文 English 混排", surface, &rect);
		CHECK(blended_calls >= 2 && wrapped_calls == 0, "mixed message did not use CJK pre-wrapping");
	}
	SDL_FreeSurface(surface);

	char long_message[301];
	for (int i = 0; i < 100; i++)
		memcpy(long_message + i * 3, "中", 3);
	long_message[300] = '\0';

	SDL_Rect long_rect = {0, 0, 200, 120};
	SDL_Surface *long_surface = new_test_surface(200, 120);
	CHECK(long_surface, "could not create long CJK surface");
	if (long_surface)
	{
		blended_calls = 0;
		wrapped_calls = 0;
		GFX_blitMessage((TTF_Font *)1, long_message, long_surface, &long_rect);
		CHECK(blended_calls == 10 && wrapped_calls == 0,
			  "long CJK message should produce 10 lines (blended=%d, wrapped=%d)", blended_calls, wrapped_calls);
	}
	SDL_FreeSurface(long_surface);
}

int main(void)
{
	printf("text layout regression tests\n");

	test_truncate_english_unchanged();
	test_truncate_utf8();
	test_truncate_short_and_empty();
	test_wrap_english_unchanged();
	test_wrap_chinese();
	test_wrap_mixed();
	test_wrap_capacity_and_invalid_utf8();
	test_blit_message_english();
	test_blit_message_long_english();
	test_blit_message_cjk();
	test_blit_message_mixed_and_long();

	if (failures)
	{
		printf("%d test(s) failed\n", failures);
		return 1;
	}
	printf("all text layout tests passed\n");
	return 0;
}
