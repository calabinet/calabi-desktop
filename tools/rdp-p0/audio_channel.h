#pragma once
#include <stdio.h>
#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/settings.h>

static BOOL configure_audio_playback(rdpSettings *settings, BOOL enabled) {
    return freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, enabled) &&
           freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE) &&
           freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, FALSE);
}

static BOOL load_audio_playback(freerdp *rdp) {
    rdpSettings *settings = rdp->context->settings;
    if (!freerdp_settings_get_bool(settings, FreeRDP_AudioPlayback)) return TRUE;

    // FreeRDP pairs rdpsnd with rdpdr for server compatibility. The settings
    // have an empty device collection; no local drives or other devices are shared.
    freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
    const DWORD flags = FREERDP_ADDIN_CHANNEL_STATIC | FREERDP_ADDIN_CHANNEL_ENTRYEX;
    PVIRTUALCHANNELENTRY devices = freerdp_load_channel_addin_entry("rdpdr", NULL, NULL, flags);
    PVIRTUALCHANNELENTRY sound = freerdp_load_channel_addin_entry("rdpsnd", NULL, NULL, flags);
    if (devices && sound &&
        freerdp_channels_client_load_ex(rdp->context->channels, settings,
            (PVIRTUALCHANNELENTRYEX)devices, settings) == 0 &&
        freerdp_channels_client_load_ex(rdp->context->channels, settings,
            (PVIRTUALCHANNELENTRYEX)sound, NULL) == 0) return TRUE;

    fputs("Unable to load RDP audio playback channel\n", stderr);
    return freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, FALSE);
}
