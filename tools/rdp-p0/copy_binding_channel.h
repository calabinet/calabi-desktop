#pragma once
#include <freerdp/svc.h>
#include <freerdp/channels/channels.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/crypto.h>
#include <winpr/interlocked.h>

// The host sends a fresh nonce and the authorized RDP TCP port. The helper
// signs the challenge with the key it shares with the local file agent.
enum { COPY_BINDING_MAC_SIZE = 32, COPY_BINDING_CHALLENGE_SIZE = 34 };
typedef struct {
    UINT16 port;
    const char *key;
} CopyBindingConfig;

typedef struct {
    CHANNEL_ENTRY_POINTS_EX api;
    void *init;
    DWORD open;
    UINT16 port;
    char key[65];
    BYTE challenge[COPY_BINDING_CHALLENGE_SIZE];
    UINT used;
    BOOL collecting;
    LONG pending;
} CopyBindingChannel;

static BOOL copy_binding_mac(UINT16 port, const char *key, const BYTE *challenge, BYTE *mac) {
    UINT16 requested = ((UINT16)challenge[32] << 8) | challenge[33];
    if (!port || port != requested || !key || strlen(key) != 64)
        return FALSE;
    return winpr_HMAC(WINPR_MD_SHA256, key, 64, challenge, COPY_BINDING_CHALLENGE_SIZE,
                      mac, COPY_BINDING_MAC_SIZE);
}

static VOID VCAPITYPE copy_binding_event(void *user, DWORD handle, UINT event, void *data,
                                        UINT32 length, UINT32 total, UINT32 flags) {
    CopyBindingChannel *channel = user;
    (void)handle;
    if (event == CHANNEL_EVENT_WRITE_COMPLETE || event == CHANNEL_EVENT_WRITE_CANCELLED) {
        free(data);
        InterlockedDecrement(&channel->pending);
        return;
    }
    if (event != CHANNEL_EVENT_DATA_RECEIVED || !channel->open)
        return;
    if (flags & CHANNEL_FLAG_FIRST) {
        channel->used = 0;
        channel->collecting = total == COPY_BINDING_CHALLENGE_SIZE;
    }
    if (!channel->collecting || total != COPY_BINDING_CHALLENGE_SIZE ||
        length > COPY_BINDING_CHALLENGE_SIZE - channel->used) {
        channel->collecting = FALSE;
        return;
    }
    memcpy(channel->challenge + channel->used, data, length);
    channel->used += length;
    if (!(flags & CHANNEL_FLAG_LAST))
        return;
    channel->collecting = FALSE;
    if (channel->used != COPY_BINDING_CHALLENGE_SIZE)
        return;
    if (InterlockedIncrement(&channel->pending) > 1) {
        InterlockedDecrement(&channel->pending);
        return;
    }
    BYTE *reply = malloc(COPY_BINDING_MAC_SIZE);
    if (!reply || !copy_binding_mac(channel->port, channel->key, channel->challenge, reply) ||
        channel->api.pVirtualChannelWriteEx(channel->init, channel->open, reply, COPY_BINDING_MAC_SIZE, reply) != CHANNEL_RC_OK) {
        free(reply);
        InterlockedDecrement(&channel->pending);
    }
}

static VOID VCAPITYPE copy_binding_init(void *user, void *init, UINT event, void *data, UINT length) {
    CopyBindingChannel *channel = user;
    (void)init; (void)data; (void)length;
    if (event == CHANNEL_EVENT_CONNECTED) {
        if (channel->api.pVirtualChannelOpenEx(channel->init, &channel->open, "calbind", copy_binding_event) != CHANNEL_RC_OK)
            channel->open = 0;
    } else if (event == CHANNEL_EVENT_DISCONNECTED) {
        channel->collecting = FALSE;
        if (channel->open)
            channel->api.pVirtualChannelCloseEx(channel->init, channel->open);
        channel->open = 0;
    } else if (event == CHANNEL_EVENT_TERMINATED) {
        SecureZeroMemory(channel->key, sizeof(channel->key));
        free(channel);
    }
}

static BOOL VCAPITYPE copy_binding_entry(PCHANNEL_ENTRY_POINTS_EX api, void *init) {
    CHANNEL_ENTRY_POINTS_FREERDP_EX *extended = (CHANNEL_ENTRY_POINTS_FREERDP_EX *)api;
    if (api->cbSize < sizeof(*extended) || extended->MagicNumber != FREERDP_CHANNEL_MAGIC_NUMBER)
        return FALSE;
    const CopyBindingConfig *config = extended->pExtendedData;
    if (!config || !config->port || !config->key || strlen(config->key) != 64)
        return FALSE;
    CopyBindingChannel *channel = calloc(1, sizeof(*channel));
    if (!channel)
        return FALSE;
    channel->api = *api;
    channel->init = init;
    channel->port = config->port;
    memcpy(channel->key, config->key, sizeof(channel->key));
    CHANNEL_DEF def = {0};
    memcpy(def.name, "calbind", sizeof("calbind"));
    def.options = CHANNEL_OPTION_INITIALIZED | CHANNEL_OPTION_ENCRYPT_RDP;
    if (api->pVirtualChannelInitEx(channel, NULL, init, &def, 1, VIRTUAL_CHANNEL_VERSION_WIN2000, copy_binding_init) != CHANNEL_RC_OK) {
        SecureZeroMemory(channel->key, sizeof(channel->key));
        free(channel);
        return FALSE;
    }
    return TRUE;
}

static UINT copy_binding_load(freerdp *rdp, UINT16 port, const char *key) {
    CopyBindingConfig config = {port, key};
    return freerdp_channels_client_load_ex(rdp->context->channels, rdp->context->settings,
                                           copy_binding_entry, &config);
}
