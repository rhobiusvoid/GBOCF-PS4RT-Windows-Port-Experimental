/* Console platform features with no Linux counterpart: sharing, streaming, remote
 * play, recording, invitation/profile/friend/web dialogs. Lifecycle and
 * configuration calls succeed as no-ops, status queries report idle, and dialog
 * opens report that the dialog system is unavailable so the title takes its
 * "cannot open" path. Call shapes follow shadPS4 (references/), which stubs most of them. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80001)
#define REMOTEPLAY_DISCONNECT 0

static unsigned noops, dialog_refusals;
static ABI int32_t platform_noop(void) { ++noops; return 0; }
static ABI int32_t dialog_unavailable(void) {
    if (!dialog_refusals++) fputs("Runtime: platform dialogs unavailable; open refused\n",stdout);
    return DIALOG_NOT_INITIALIZED;
}
static ABI int32_t dialog_idle(void) { return 0; } /* ORBIS_COMMON_DIALOG_STATUS_NONE */
static ABI int32_t remoteplay_connection(int32_t user,int32_t *status) {
    (void)user; if (!status) return (int32_t)0x80B20002; /* invalid argument */
    *status=REMOTEPLAY_DISCONNECT; return 0;
}

uintptr_t runtime_platform_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        /* VideoRecording, ScreenShot, MsgDialog, AppContent, ShareUtility */
        {"Fc8qxlKINYQ#libSceVideoRecording",platform_noop}, {"G7KlmIYFIZc#libSceScreenShot",platform_noop},
        {"Gc5k1qcK4fs#libSceMsgDialog",platform_noop}, {"3rHWaV-1KC4#libSceAppContent",platform_noop},
        {"u4bCGkqhOB4#libSceShareUtility",platform_noop}, {"karnJhakdKg#libSceShareUtility",platform_noop},
        {"xfB04bSjrZY#libSceShareUtility",platform_noop}, {"j7DlalBzHh8#libSceShareUtility",platform_noop},
        {"DUWhxkyVPj4#libSceShareUtility",platform_noop},
        {"1TFyHtLzdvg#libSceShareUtility",dialog_unavailable}, {"tyYRZwL4rog#libSceShareUtility",dialog_unavailable},
        {"wcxz4F94s6k#libSceShareUtility",dialog_unavailable}, {"Lqp4EDIRXSo#libSceShareUtility",dialog_unavailable},
        {"-4kr-v21vjc#libSceShareUtility",dialog_unavailable}, {"8hZ2EEl2Tto#libSceShareUtility",dialog_unavailable},
        /* Remoteplay, SharePlay, GameLiveStreaming */
        {"g3PNjYKWqnQ#libSceRemoteplay",remoteplay_connection}, {"k1SwgkMSOM8#libSceRemoteplay",platform_noop},
        {"7QLrixwVHcU#libSceRemoteplay",platform_noop}, {"BOwybKVa3Do#libSceRemoteplay",platform_noop},
        {"+MCXJlWdi+s#libSceSharePlay",platform_noop}, {"isruqthpYcw#libSceSharePlay",platform_noop},
        {"co2NCj--pnc#libSceSharePlay",platform_noop}, {"UaLjloJinow#libSceSharePlay",platform_noop},
        {"lK8dLBNp9OE#libSceGameLiveStreaming",platform_noop}, {"kvYEw2lBndk#libSceGameLiveStreaming",platform_noop},
        {"K0QxEbD7q+c#libSceGameLiveStreaming",platform_noop}, {"9yK6Fk8mKOQ#libSceGameLiveStreaming",platform_noop},
        /* Invitation, profile, friend-list and web browser dialogs */
        {"XvA5KS56wcs#libSceInvitationDialog",platform_noop}, {"B6HVJtDYxEE#libSceInvitationDialog",platform_noop},
        {"WWtCL5lzi7Y#libSceInvitationDialog",platform_noop}, {"9+g9iOq+7kg#libSceInvitationDialog",dialog_idle},
        {"sAxbHhAWMXM#libSceInvitationDialog",dialog_unavailable}, {"WuuUhuKOxwQ#libSceInvitationDialog",dialog_unavailable},
        {"wkwjz0Xdo2A#libSceNpProfileDialog",platform_noop}, {"nrQRlLKzdwE#libSceNpProfileDialog",dialog_unavailable},
        {"2L-W-ZYn2Qo#libSceNpFriendListDialog",platform_noop}, {"ECEzk+K9L2k#libSceNpFriendListDialog",platform_noop},
        {"frwz3eyuA6w#libSceNpFriendListDialog",dialog_idle}, {"vN9sTP63WT8#libSceNpFriendListDialog",dialog_unavailable},
        {"hK0CCljzJmY#libSceNpFriendListDialog",dialog_unavailable},
        {"jqb7HntFQFc#libSceWebBrowserDialog",platform_noop}, {"ocHtyBwHfys#libSceWebBrowserDialog",platform_noop},
        {"PSK+Eik919Q#libSceWebBrowserDialog",platform_noop}, {"h1dR-t5ISgg#libSceWebBrowserDialog",dialog_idle},
        {"FraP7debcdg#libSceWebBrowserDialog",dialog_unavailable}, {"O7dIZQrwVFY#libSceWebBrowserDialog",dialog_unavailable},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_platform_report(void) {
    printf("Runtime: platform no-op calls=%u, dialog refusals=%u\n",noops,dialog_refusals);
}
