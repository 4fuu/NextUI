/*
 * Host-only wiring test for the launcher's HOME action. It includes the real
 * nextui.c (renaming its main) so the test calls the same runFnAction() and
 * shared UIOverlay instance the launcher uses. No device input is involved.
 *
 * It also renders the real overlay through GFX and checks the selected row's
 * label and status are centered in the row whose icon box is centered too.
 * That is the observable form of the alignment contract: SDL_ttf's line box is
 * taller than the nominal FONT_* size, so centering on the nominal value pushes
 * text below the icon.
 */
#define main nextui_launcher_main
#include "../nextui.c"
#undef main

#include <stdio.h>

/* Defined in api.c; CFG owns the callback type, the test needs the reload. */
int GFX_loadSystemFont(const char* fontPath);

typedef struct
{
	int row_center2;   /* centers are doubled to avoid half-pixel rounding */
	int label_center2;
	int status_center2;
} RowAlignment;

static Uint32 overlay_test_pixel(SDL_Surface* screen, int x, int y)
{
	return *(Uint32*)((Uint8*)screen->pixels + y * screen->pitch + x * sizeof(Uint32));
}

static int overlay_test_is_row_color(SDL_Surface* screen, int x, int y)
{
	return overlay_test_pixel(screen, x, y) == THEME_COLOR1;
}

/* Finds the selected row by its full-width highlight, then splits its ink into
 * icon, label and status runs. Columns are grouped with a gap tolerance so the
 * letters of one word stay together but the icon/label/status gaps stay apart. */
static int overlay_test_measure_row(SDL_Surface* screen, RowAlignment* out)
{
	int width = screen->w;
	int height = screen->h;
	int top = -1, bottom = -1;

	if (screen->format->BitsPerPixel != 32) return 0;
	if (SDL_LockSurface(screen) != 0) return 0;

	for (int y = 0; y < height; y++)
	{
		int count = 0;
		for (int x = 0; x < width; x++)
			if (overlay_test_is_row_color(screen, x, y)) count++;

		int wide = count > width / 2;
		if (wide && top < 0) top = y;
		if (wide)
			bottom = y;
		else if (top >= 0)
			break;
	}
	if (top < 0 || bottom <= top)
	{
		SDL_UnlockSurface(screen);
		return 0;
	}

	int left = width, right = -1;
	for (int y = top; y <= bottom; y++)
	{
		for (int x = 0; x < width; x++)
		{
			if (!overlay_test_is_row_color(screen, x, y)) continue;
			if (x < left) left = x;
			if (x > right) right = x;
		}
	}

	int inset = SCALE1(2);
	int x0 = left + inset;
	int x1 = right - inset;
	int y0 = top + inset;
	int y1 = bottom - inset;
	int gap = SCALE1(4);
	int runs[8][2];
	int run_count = 0;
	int run_open = 0;
	int run_start = 0;
	int last_ink = 0;

	for (int x = x0; x <= x1; x++)
	{
		int ink = 0;
		for (int y = y0; y <= y1; y++)
		{
			if (!overlay_test_is_row_color(screen, x, y))
			{
				ink = 1;
				break;
			}
		}

		if (ink)
		{
			if (!run_open)
			{
				run_open = 1;
				run_start = x;
			}
			last_ink = x;
		}
		else if (run_open && x - last_ink > gap)
		{
			if (run_count < 8)
			{
				runs[run_count][0] = run_start;
				runs[run_count][1] = last_ink;
				run_count++;
			}
			run_open = 0;
		}
	}
	if (run_open && run_count < 8)
	{
		runs[run_count][0] = run_start;
		runs[run_count][1] = last_ink;
		run_count++;
	}

	if (run_count < 2)
	{
		SDL_UnlockSurface(screen);
		return 0;
	}

	int centers[8];
	for (int i = 0; i < run_count; i++)
	{
		int ink_top = -1, ink_bottom = -1;
		for (int x = runs[i][0]; x <= runs[i][1]; x++)
		{
			for (int y = y0; y <= y1; y++)
			{
				if (overlay_test_is_row_color(screen, x, y)) continue;
				if (ink_top < 0) ink_top = y;
				ink_bottom = y;
			}
		}
		centers[i] = ink_top < 0 ? -1 : ink_top + ink_bottom;
	}

	out->row_center2 = top + bottom;
	out->label_center2 = centers[1];
	out->status_center2 = run_count > 2 ? centers[2] : -1;

	SDL_UnlockSurface(screen);
	return 1;
}

static int overlay_test_check_alignment(SDL_Surface* screen, const char* language)
{
	RowAlignment row;
	if (!overlay_test_measure_row(screen, &row))
	{
		fprintf(stderr, "%s overlay: selected row has no icon and label ink\n", language);
		return 1;
	}

	/* The icon asset box is centered in the row; its drawn ink can be
	 * asymmetric (the Wi-Fi fan sits high in its 12x12 cell), so the row
	 * highlight is the shared center that both text runs must match. */
	int tolerance2 = SCALE1(2) * 2;
	int failed = 0;

	if (abs(row.label_center2 - row.row_center2) > tolerance2)
	{
		fprintf(stderr, "%s overlay: label center %.1f is %.1f px from row center %.1f\n", language,
				row.label_center2 / 2.0, (row.label_center2 - row.row_center2) / 2.0,
				row.row_center2 / 2.0);
		failed = 1;
	}
	if (row.status_center2 >= 0 && abs(row.status_center2 - row.row_center2) > tolerance2)
	{
		fprintf(stderr, "%s overlay: status center %.1f is %.1f px from row center %.1f\n", language,
				row.status_center2 / 2.0, (row.status_center2 - row.row_center2) / 2.0,
				row.row_center2 / 2.0);
		failed = 1;
	}

	return failed;
}

static void overlay_test_clear(SDL_Surface* screen)
{
	SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
}

static int overlay_test_device_available(void* user)
{
	(void)user;
	return 1;
}

/* Deliberately Chinese copy for the shared overlay path used by adapted apps.
 * UIOverlay_create copies the struct but keeps these static literals alive. */
static UIOverlayStrings overlay_test_zh_strings(void)
{
	UIOverlayStrings strings = *UIOverlay_defaultStrings();
	strings.title = "系统";
	strings.resume = "继续";
	strings.save = "保存进度";
	strings.wifi = "Wi-Fi";
	strings.bluetooth = "蓝牙";
	strings.exit_to_desktop = "退出到桌面";
	strings.close = "关闭";
	strings.select = "选择";
	strings.ready = "就绪";
	strings.no_state = "无存档";
	strings.no_changes = "无更改";
	strings.unavailable = "不可用";
	strings.not_connected = "未连接";
	strings.on = "开";
	strings.off = "关";
	strings.saved = "已保存";
	strings.resumed = "已继续";
	strings.failed = "失败";
	return strings;
}

int main(int argc, char** argv)
{
	int failed = 0;
	const char* font_en = argc > 1 ? argv[1] : RES_PATH "/font1.ttf";
	const char* font_zh = argc > 2 ? argv[2] : RES_PATH "/font3.ttf";

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

	/* GFX_initAttached loads the real atlas and paints into an off-screen
	 * surface, so the row check uses the same TTF metrics as the device without
	 * a window. */
	SDL_Surface* screen = GFX_initAttached(FIXED_WIDTH, FIXED_HEIGHT);
	if (!screen)
	{
		fprintf(stderr, "overlay render surface could not be created\n");
		failed = 1;
	}
	else
	{
		if (GFX_loadSystemFont(font_en) != 0 || !font.medium)
		{
			fprintf(stderr, "English font could not be loaded: %s\n", font_en);
			failed = 1;
		}
		else if (!runFnAction(2))
		{
			fprintf(stderr, "desktop HOME action was not handled for rendering\n");
			failed = 1;
		}
		else
		{
			overlay_test_clear(screen);
			UIOverlay_draw(home_overlay, screen);
			failed |= overlay_test_check_alignment(screen, "English");
			UIOverlay_close(home_overlay);
		}

		if (GFX_loadSystemFont(font_zh) != 0 || !font.medium)
		{
			fprintf(stderr, "Chinese font could not be loaded: %s\n", font_zh);
			failed = 1;
		}
		else
		{
			UIOverlayDeviceHooks device;
			memset(&device, 0, sizeof(device));
			device.available = overlay_test_device_available;
			device.wifi_get = home_overlay_wifi_get;
			device.wifi_set = home_overlay_wifi_set;

			OverlayAppHooks hooks;
			memset(&hooks, 0, sizeof(hooks));
			hooks.app_name = "NextUI";
			hooks.capabilities = UI_OVERLAY_CAP_WIFI;
			hooks.device = &device;

			UIOverlayStrings strings = overlay_test_zh_strings();
			UIOverlay* zh_overlay = UIOverlay_create(&hooks, &strings);
			if (!zh_overlay)
			{
				fprintf(stderr, "Chinese overlay could not be created\n");
				failed = 1;
			}
			else
			{
				UIOverlay_open(zh_overlay);
				overlay_test_clear(screen);
				UIOverlay_draw(zh_overlay, screen);
				failed |= overlay_test_check_alignment(screen, "Chinese");
				UIOverlay_destroy(zh_overlay);
			}
		}

		GFX_quitAttached();
	}

	UIOverlay_destroy(home_overlay);
	home_overlay = NULL;
	if (!failed) printf("desktop HOME overlay wiring and row alignment passed\n");
	return failed ? 1 : 0;
}
