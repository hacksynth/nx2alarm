// Linux platform: local play over the LAN (DM_LAN=1, hle_ldn.c / hle_socket.c).
// The Android port (dm/android/lan_android.c) only holds a WifiManager.MulticastLock: many Wi-Fi drivers drop broadcast and multicast frames to save power
// unless an app holds it, and without it the device never sees the other players' ldn_mitm scans (UDP 11452). Linux has no such lock at the socket's
// reach: the host's sockets are plain UDP and multicast/broadcast pass as the system's network manager allows. So the Linux backend is a no-op with one
// line explaining why, kept as a function pair so the front ends on both ports look alike.
#include "dm.h"
#include <stdlib.h>

void dm_lan_alarm_start(void) {
    const char *e = getenv("DM_LAN");
    if (!e || *e != '1') return;
    static int logged;
    if (!logged) { logged = 1; LOG("[lan] Linux: no Wi-Fi multicast lock is needed; the host's UDP broadcasts and multicast go out as sent"); }
}
void dm_lan_alarm_stop(void) { }
