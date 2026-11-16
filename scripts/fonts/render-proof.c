/*
 * Render a 1024x768 proof sheet for the NextUI UI font at the five real
 * device sizes (48/42/36/30/21 px, i.e. FONT_* x scale 3 on Brick Pro).
 *
 * Build (self-contained, no project makefile changes):
 *   cc -O2 -std=c99 -Wall -Wextra -o /tmp/nextui-font-proof \
 *      scripts/fonts/render-proof.c \
 *      $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image)
 *
 * Run:
 *   /tmp/nextui-font-proof skeleton/SYSTEM/res/font3.ttf /tmp/font3.png "font3.ttf"
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>

#define CANVAS_W 1024
#define CANVAS_H 768

static void fail(const char *what) {
    fprintf(stderr, "%s: %s\n", what, SDL_GetError());
    exit(1);
}

static int draw_text(SDL_Surface *canvas, TTF_Font *font, const char *text,
                     int x, int y, SDL_Color color) {
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(font, text, color);
    if (!rendered) {
        fprintf(stderr, "render failed for %s: %s\n", text, TTF_GetError());
        return 0;
    }
    SDL_Rect destination = {x, y, rendered->w, rendered->h};
    if (SDL_BlitSurface(rendered, NULL, canvas, &destination) != 0) {
        fprintf(stderr, "blit failed for %s: %s\n", text, SDL_GetError());
        SDL_FreeSurface(rendered);
        return 0;
    }
    SDL_FreeSurface(rendered);
    return 1;
}

static TTF_Font *open_font(const char *path, int size) {
    TTF_Font *font = TTF_OpenFont(path, size);
    if (!font) {
        fprintf(stderr, "TTF_OpenFont(%s, %d): %s\n", path, size, TTF_GetError());
        exit(1);
    }
    /* NextUI's CFG_DEFAULT_FONT_STYLE is 0x01 (TTF_STYLE_BOLD). */
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    return font;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <font.ttf> <out.png> [label]\n", argv[0]);
        return 2;
    }
    const char *font_path = argv[1];
    const char *out_path = argv[2];
    const char *label = argc > 3 ? argv[3] : font_path;

    if (SDL_Init(0) != 0) fail("SDL_Init");
    if (TTF_Init() != 0) fail("TTF_Init");
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) fail("IMG_Init");

    SDL_Surface *canvas = SDL_CreateRGBSurfaceWithFormat(0, CANVAS_W, CANVAS_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!canvas) fail("SDL_CreateRGBSurfaceWithFormat");
    SDL_FillRect(canvas, NULL, SDL_MapRGB(canvas->format, 16, 20, 24));

    TTF_Font *font_48 = open_font(font_path, 48);
    TTF_Font *font_42 = open_font(font_path, 42);
    TTF_Font *font_36 = open_font(font_path, 36);
    TTF_Font *font_30 = open_font(font_path, 30);
    TTF_Font *font_21 = open_font(font_path, 21);
    TTF_Font *font_24 = open_font(font_path, 24);
    TTF_Font *font_16 = open_font(font_path, 16);

    const SDL_Color white = {242, 246, 250, 255};
    const SDL_Color accent = {111, 211, 255, 255};
    const SDL_Color muted = {150, 160, 170, 255};
    const SDL_Color warning = {255, 138, 128, 255};

    int ok = 1;
    char line[256];

    snprintf(line, sizeof(line), "NextUI font proof: %s", label);
    ok &= draw_text(canvas, font_24, line, 24, 18, accent);
    ok &= draw_text(canvas, font_16,
                    "1024x768; TTF_STYLE_BOLD (CFG_DEFAULT_FONT_STYLE=1); real sizes 48/42/36/30/21 px",
                    24, 46, muted);

    SDL_Rect rule1 = {24, 68, CANVAS_W - 48, 1};
    SDL_FillRect(canvas, &rule1, SDL_MapRGB(canvas->format, 45, 55, 65));

    ok &= draw_text(canvas, font_16, "48px", 24, 96, accent);
    ok &= draw_text(canvas, font_48, "设置 语言 应用 收藏 UI，。", 120, 76, white);

    ok &= draw_text(canvas, font_16, "42px", 24, 166, accent);
    ok &= draw_text(canvas, font_42, "最近游玩，界面自定义。简体中文 UI", 120, 146, white);

    ok &= draw_text(canvas, font_16, "36px", 24, 226, accent);
    ok &= draw_text(canvas, font_36, "NextUI 中文界面 English UI，36px。", 120, 210, white);

    ok &= draw_text(canvas, font_16, "30px", 24, 282, accent);
    ok &= draw_text(canvas, font_30, "确认，读取。打开！关闭？默认载入恢复", 120, 266, white);

    ok &= draw_text(canvas, font_16, "21px", 24, 330, accent);
    ok &= draw_text(canvas, font_21, "全角标点：，。！？：；（）【】“”《》、", 120, 314, white);

    ok &= draw_text(canvas, font_16, "21px", 24, 366, accent);
    ok &= draw_text(canvas, font_21, "GB2312 L1: 啊阿埃挨哎唉哀皑癌蔼矮艾碍爱隘", 120, 350, white);

    ok &= draw_text(canvas, font_16, "21px", 24, 402, accent);
    ok &= draw_text(canvas, font_21, "A3: 最近游玩 收藏 工具 游戏 无预览 暂无最近记录", 120, 386, white);

    ok &= draw_text(canvas, font_16, "21px", 24, 438, accent);
    ok &= draw_text(canvas, font_21, "A3: 简体中文 界面显示语言。返回桌面后生效。", 120, 422, white);

    ok &= draw_text(canvas, font_16, "21px", 24, 486, warning);
    ok &= draw_text(canvas, font_21,
                    "Not covered by this subset (GB2312 L2/outside): 鑫 淼 龘 垚 犇 骉",
                    120, 470, warning);

    ok &= draw_text(canvas, font_16, "21px", 24, 522, accent);
    ok &= draw_text(canvas, font_21,
                    "English: Settings, Applications, Favorites, Recents, Language",
                    120, 506, white);

    SDL_Rect rule2 = {24, 690, CANVAS_W - 48, 1};
    SDL_FillRect(canvas, &rule2, SDL_MapRGB(canvas->format, 45, 55, 65));
    ok &= draw_text(canvas, font_16,
                    "Proof for the supplied font; fixed-UI coverage only, not all Chinese filenames. "
                    "Source/license: NotoSansSC-SOURCE.md.",
                    24, 704, muted);

    if (IMG_SavePNG(canvas, out_path) != 0) {
        fprintf(stderr, "IMG_SavePNG(%s): %s\n", out_path, IMG_GetError());
        ok = 0;
    }

    TTF_CloseFont(font_48);
    TTF_CloseFont(font_42);
    TTF_CloseFont(font_36);
    TTF_CloseFont(font_30);
    TTF_CloseFont(font_21);
    TTF_CloseFont(font_24);
    TTF_CloseFont(font_16);
    SDL_FreeSurface(canvas);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();

    if (!ok) return 1;
    printf("wrote %s\n", out_path);
    return 0;
}
