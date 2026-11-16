#include "app_capabilities.h"

#include <string.h>

static void copy_text(char* out, size_t out_size, const char* text)
{
    size_t length;

    if (out == NULL || out_size == 0) return;
    if (text == NULL)
    {
        out[0] = '\0';
        return;
    }
    length = strlen(text);
    if (length >= out_size) length = out_size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static int adapter_can_save(void* user);
static int adapter_save(void* user);
static int adapter_can_resume(void* user);
static int adapter_resume(void* user);

void app_capabilities_inspect(const char* root, const char* pak_dir,
                              const PakManifest* manifest, AppCapabilityInfo* info)
{
    AppStateInfo state;

    if (info == NULL) return;

    memset(info, 0, sizeof *info);
    if (manifest != NULL) info->manifest = *manifest;

    info->state_status = APP_STATE_UNSUPPORTED;
    copy_text(info->state_reason, sizeof info->state_reason, "capability_not_declared");

    if (manifest == NULL || manifest->status != PAK_MANIFEST_OK) return;

    info->declared = manifest->capabilities;
    info->saves_declared =
        pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES) != 0;
    info->overlay_declared =
        pak_manifest_has_capability(manifest, PAK_CAPABILITY_OVERLAY) != 0;

    if (!info->saves_declared) return;

    info->state_status = app_state_inspect(root, pak_dir, manifest, &state);
    copy_text(info->state_reason, sizeof info->state_reason, state.reason);
    copy_text(info->payload_dir, sizeof info->payload_dir, state.payload_dir);
    info->state_valid = info->state_status == APP_STATE_VALID;
}

unsigned app_capabilities_actions(const AppCapabilityInfo* info,
                                  AppCapabilitiesContext context,
                                  const AppCapabilityCallbacks* callbacks)
{
    unsigned actions = 0;

    if (info == NULL) return 0;

    if (context == APP_CAPABILITIES_APP)
    {
        /* No "overlay" declaration means the application is not adapted and
         * must not receive any shared-overlay entries, even if it declares
         * "saves" for its own state management. */
        if (!info->overlay_declared) return 0;

        actions |= APP_CAPABILITIES_ACTION_OVERLAY;
        if (info->saves_declared && callbacks != NULL)
        {
            if (callbacks->write_state != NULL)
                actions |= APP_CAPABILITIES_ACTION_SAVE;
            if (callbacks->read_state != NULL && info->state_valid)
                actions |= APP_CAPABILITIES_ACTION_RESUME;
        }
        return actions;
    }

    /* Desktop: no application process is running. Save has no callback to
     * call and Overlay cannot be drawn into another process. Resume is a
     * launcher action backed by B3's env-only resume command. */
    if (info->saves_declared && info->state_valid)
        actions |= APP_CAPABILITIES_ACTION_RESUME;
    return actions;
}

int app_capabilities_overlay_supported(const AppCapabilityInfo* info)
{
    return info != NULL && info->overlay_declared != 0;
}

unsigned app_capabilities_overlay_capabilities(const AppCapabilityInfo* info)
{
    unsigned capabilities = 0;

    if (!app_capabilities_overlay_supported(info)) return 0;
    if (info->saves_declared)
        capabilities |= UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME;
    return capabilities;
}

int app_capabilities_overlay_row_visible(const AppCapabilityInfo* info, unsigned action)
{
    if (!app_capabilities_overlay_supported(info)) return 0;

    switch (action)
    {
    case APP_CAPABILITIES_ACTION_SAVE:
    case APP_CAPABILITIES_ACTION_RESUME:
        return info->saves_declared != 0;
    default:
        return 0;
    }
}

int app_capabilities_overlay_row_enabled(const AppCapabilityInfo* info, unsigned action,
                                         const AppCapabilityCallbacks* callbacks)
{
    if (!app_capabilities_overlay_row_visible(info, action)) return 0;
    if (callbacks == NULL) return 0;

    switch (action)
    {
    case APP_CAPABILITIES_ACTION_SAVE:
        if (callbacks->write_state == NULL) return 0;
        if (callbacks->can_save != NULL && callbacks->can_save(callbacks->user) == 0)
            return 0;
        return 1;
    case APP_CAPABILITIES_ACTION_RESUME:
        return callbacks->read_state != NULL && info->state_valid != 0;
    default:
        return 0;
    }
}

static int adapter_can_save(void* user)
{
    AppCapabilityAdapter* adapter = (AppCapabilityAdapter*)user;

    if (adapter == NULL) return 0;
    return app_capabilities_overlay_row_enabled(&adapter->info,
                                                APP_CAPABILITIES_ACTION_SAVE,
                                                &adapter->callbacks);
}

static int adapter_save(void* user)
{
    AppCapabilityAdapter* adapter = (AppCapabilityAdapter*)user;
    AppStateCommitResult result;
    char error[APP_STATE_REASON_MAX];

    if (adapter == NULL) return 0;
    if (!app_capabilities_overlay_row_enabled(&adapter->info,
                                              APP_CAPABILITIES_ACTION_SAVE,
                                              &adapter->callbacks))
        return 0;
    if (adapter->callbacks.write_state(adapter->callbacks.user) == 0) return 0;

    error[0] = '\0';
    result = app_state_commit(adapter->root, adapter->pak_dir, &adapter->manifest,
                              error, sizeof error);
    app_capabilities_adapter_refresh(adapter);

    /* APP_STATE_COMMIT_OK is 0. Returning the enum directly would turn a
     * failure into success and success into failure; compare explicitly. */
    return result == APP_STATE_COMMIT_OK;
}

static int adapter_can_resume(void* user)
{
    AppCapabilityAdapter* adapter = (AppCapabilityAdapter*)user;

    if (adapter == NULL) return 0;
    app_capabilities_adapter_refresh(adapter);
    return app_capabilities_overlay_row_enabled(&adapter->info,
                                                APP_CAPABILITIES_ACTION_RESUME,
                                                &adapter->callbacks);
}

static int adapter_resume(void* user)
{
    AppCapabilityAdapter* adapter = (AppCapabilityAdapter*)user;

    if (adapter == NULL) return 0;
    app_capabilities_adapter_refresh(adapter);
    if (!app_capabilities_overlay_row_enabled(&adapter->info,
                                              APP_CAPABILITIES_ACTION_RESUME,
                                              &adapter->callbacks))
        return 0;
    return adapter->callbacks.read_state(adapter->callbacks.user) == 1;
}

static void build_hooks(AppCapabilityAdapter* adapter)
{
    memset(&adapter->hooks, 0, sizeof adapter->hooks);

    if (!app_capabilities_overlay_supported(&adapter->info)) return;

    adapter->hooks.user = adapter;
    adapter->hooks.capabilities = app_capabilities_overlay_capabilities(&adapter->info);
    if (adapter->info.manifest.name[0])
        adapter->hooks.app_name = adapter->info.manifest.name;

    if (app_capabilities_overlay_row_visible(&adapter->info,
                                             APP_CAPABILITIES_ACTION_SAVE)
        && adapter->callbacks.write_state != NULL)
    {
        adapter->hooks.can_save = adapter_can_save;
        adapter->hooks.save = adapter_save;
    }

    if (app_capabilities_overlay_row_visible(&adapter->info,
                                             APP_CAPABILITIES_ACTION_RESUME)
        && adapter->callbacks.read_state != NULL)
    {
        adapter->hooks.can_resume = adapter_can_resume;
        adapter->hooks.resume = adapter_resume;
    }
}

int app_capabilities_adapter_init(AppCapabilityAdapter* adapter,
                                  const char* root, const char* pak_dir,
                                  const PakManifest* manifest,
                                  const AppCapabilityCallbacks* callbacks)
{
    AppStateInfo state;

    if (adapter == NULL || root == NULL || root[0] == '\0'
        || pak_dir == NULL || pak_dir[0] == '\0' || manifest == NULL)
        return 0;
    if (strlen(root) >= sizeof adapter->root
        || strlen(pak_dir) >= sizeof adapter->pak_dir)
        return 0;

    memset(adapter, 0, sizeof *adapter);
    copy_text(adapter->root, sizeof adapter->root, root);
    copy_text(adapter->pak_dir, sizeof adapter->pak_dir, pak_dir);
    adapter->manifest = *manifest;
    if (callbacks != NULL) adapter->callbacks = *callbacks;

    if (manifest->status == PAK_MANIFEST_OK
        && pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES))
    {
        if (app_state_prepare(root, pak_dir, manifest, &state))
            copy_text(adapter->payload_dir, sizeof adapter->payload_dir,
                      state.payload_dir);
    }

    app_capabilities_adapter_refresh(adapter);
    build_hooks(adapter);
    return 1;
}

void app_capabilities_adapter_refresh(AppCapabilityAdapter* adapter)
{
    if (adapter == NULL) return;

    app_capabilities_inspect(adapter->root, adapter->pak_dir, &adapter->manifest,
                             &adapter->info);
    if (adapter->info.payload_dir[0])
        copy_text(adapter->payload_dir, sizeof adapter->payload_dir,
                  adapter->info.payload_dir);
}

const OverlayAppHooks* app_capabilities_adapter_hooks(AppCapabilityAdapter* adapter)
{
    if (adapter == NULL) return NULL;
    if (!app_capabilities_overlay_supported(&adapter->info)) return NULL;
    return &adapter->hooks;
}

const char* app_capabilities_adapter_payload_dir(const AppCapabilityAdapter* adapter)
{
    if (adapter == NULL || adapter->payload_dir[0] == '\0') return NULL;
    return adapter->payload_dir;
}
