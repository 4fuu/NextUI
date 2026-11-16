#ifndef APP_CAPABILITIES_H
#define APP_CAPABILITIES_H

#include <stddef.h>

#include "app_state.h"
#include "pak_manifest.h"
#include "ui_overlay.h"

/*
 * B4 capability adapter.
 *
 * This module turns the single pak.json manifest, an application's runtime
 * callbacks, and B3's validated state into the Save/Resume/Overlay entry
 * decisions consumed by the desktop and by the shared B5 overlay. It does not
 * parse save payloads, implement a new state format, or draw anything.
 *
 * Two contexts are deliberately different:
 *
 *   DESKTOP - the application process is not running. The launcher may offer
 *             Resume only when "saves" is declared and B3 reports a complete,
 *             valid state. It never offers Save because there is no running
 *             application callback to call. It never offers Overlay because
 *             the desktop cannot draw into another process. A resume launch
 *             is B3's env-only APP_LAUNCH_RESUME command.
 *
 *   APP     - an adapted application is running and hosts the shared overlay.
 *             Save requires the exact "saves" declaration and a usable write
 *             callback; existing state is not required, so the first save is
 *             possible. Resume requires the declaration, a usable read
 *             callback, and a B3 state that fully validates. Overlay is
 *             available only when the application declared "overlay"; an
 *             unadapted application gets no hooks and no overlay rows.
 *
 * Capability availability follows the B5 rule: a declared capability makes
 * its row visible, while a missing callback or invalid state leaves the row
 * visible but disabled. Undeclared capabilities are not shown at all.
 */

#define APP_CAPABILITIES_ACTION_SAVE (1u << 0)
#define APP_CAPABILITIES_ACTION_RESUME (1u << 1)
#define APP_CAPABILITIES_ACTION_OVERLAY (1u << 2)

typedef enum
{
    APP_CAPABILITIES_DESKTOP = 0, /* launcher process, application not running */
    APP_CAPABILITIES_APP = 1,     /* adapted application process */
} AppCapabilitiesContext;

/*
 * Application-owned state callbacks for the shared overlay. write_state()
 * writes payload files into the directory returned by
 * app_capabilities_adapter_payload_dir(); it must not call app_state_commit()
 * itself because the adapter publishes the state after the callback returns.
 * read_state() restores the application from that same directory.
 *
 * All callbacks return 1 for success/available and 0 otherwise. can_save is
 * optional and is an additional gate; it is never a substitute for the
 * declaration or for the write callback.
 */
typedef struct
{
    void* user;
    int (*write_state)(void* user);
    int (*read_state)(void* user);
    int (*can_save)(void* user);
} AppCapabilityCallbacks;

typedef struct
{
    PakManifest manifest;                    /* snapshot supplied by the caller */
    unsigned declared;                       /* PAK_CAPABILITY_* mask */
    int saves_declared;                      /* exact "saves" declaration */
    int overlay_declared;                    /* exact "overlay" declaration */
    AppStateStatus state_status;             /* B3 inspection result */
    char state_reason[APP_STATE_REASON_MAX]; /* stable diagnostic, never UI copy */
    int state_valid;                         /* state_status == APP_STATE_VALID */
    char payload_dir[APP_STATE_PATH_MAX];    /* B3 payload directory, when known */
} AppCapabilityInfo;

typedef struct
{
    char root[APP_STATE_PATH_MAX];
    char pak_dir[APP_STATE_PATH_MAX];
    char payload_dir[APP_STATE_PATH_MAX];
    PakManifest manifest; /* adapter-owned snapshot for B3 calls */
    AppCapabilityInfo info;
    AppCapabilityCallbacks callbacks;
    OverlayAppHooks hooks;
} AppCapabilityAdapter;

/*
 * Inspect one manifest snapshot and, when "saves" is declared, the B3 state.
 * The manifest is never re-read here, so capability and state decisions use the
 * same snapshot. `info` must not be NULL.
 */
void app_capabilities_inspect(const char* root, const char* pak_dir,
                              const PakManifest* manifest, AppCapabilityInfo* info);

/*
 * Return the APP_CAPABILITIES_ACTION_* entries available in one context.
 * DESKTOP never returns SAVE or OVERLAY. APP returns OVERLAY for an adapted
 * application even when it has no Save/Resume rows, and gates SAVE/RESUME on
 * the callbacks plus B3 validity. This function does not call can_save; row
 * enablement does that so a dynamic gate is evaluated at action time.
 */
unsigned app_capabilities_actions(const AppCapabilityInfo* info,
                                  AppCapabilitiesContext context,
                                  const AppCapabilityCallbacks* callbacks);

/* The application explicitly declared "overlay": it may host the B5 module. */
int app_capabilities_overlay_supported(const AppCapabilityInfo* info);

/*
 * B5 capability mask for an adapted application. Zero when "overlay" is not
 * declared, so an unadapted application cannot accidentally receive Save or
 * Resume rows. "saves" maps to SAVE|RESUME; B4 does not add EXIT or device
 * actions, which remain the application's/B6's/C1's responsibility.
 */
unsigned app_capabilities_overlay_capabilities(const AppCapabilityInfo* info);

/*
 * Row visibility and enablement, mirroring the B5 panel rules. A row is
 * visible only for a declared capability on an adapted application. It is
 * enabled only when the required callback exists and its gate passes:
 *   SAVE   - write_state exists and optional can_save is not 0; no state needed
 *   RESUME - read_state exists and B3 reports APP_STATE_VALID
 */
int app_capabilities_overlay_row_visible(const AppCapabilityInfo* info, unsigned action);
int app_capabilities_overlay_row_enabled(const AppCapabilityInfo* info, unsigned action,
                                         const AppCapabilityCallbacks* callbacks);

/*
 * Initialize an adapter for one running application. The manifest snapshot
 * must stay valid for the duration of the call only; it is copied. The
 * adapter creates/prepares the B3 payload directory when "saves" is declared
 * and then performs the authoritative B3 inspection. Returns 0 for invalid
 * arguments; a legacy or invalid manifest still initializes successfully so
 * the caller can keep its normal launch path.
 */
int app_capabilities_adapter_init(AppCapabilityAdapter* adapter,
                                  const char* root, const char* pak_dir,
                                  const PakManifest* manifest,
                                  const AppCapabilityCallbacks* callbacks);

/* Re-run B3 inspection and refresh the adapter's state information. */
void app_capabilities_adapter_refresh(AppCapabilityAdapter* adapter);

/*
 * Overlay hooks for this adapter, or NULL when the application did not
 * declare "overlay". The returned struct is owned by the adapter and remains
 * valid until the adapter is destroyed. The caller may fill exit_to_desktop,
 * focus_changed, input_reset and device before passing it to UIOverlay_create().
 */
const OverlayAppHooks* app_capabilities_adapter_hooks(AppCapabilityAdapter* adapter);

/* B3 payload directory the application should write/read, or NULL. */
const char* app_capabilities_adapter_payload_dir(const AppCapabilityAdapter* adapter);

#endif
