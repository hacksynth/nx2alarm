// Linux platform layer shared declarations (nxnative/dm/alarm/platform).
// Only the Linux-specific entry points the front end may call; the host API these backends replace (AudioToolbox, mavc, dm_saves_*, AAssetManager) keeps
// the headers the rest of the host already includes, so no platform types leak into them.
#pragma once

// haptics_alarm.c: SDL3 rumble for player 1 (called once by the front end when the host starts; a stop is for shutdown)
void dm_haptics_alarm_start(int enabled);
void dm_haptics_alarm_set(int on);
void dm_haptics_alarm_stop(void);

// lan_alarm.c: the Linux LAN setup (a no-op besides one log line; see the file)
void dm_lan_alarm_start(void);
void dm_lan_alarm_stop(void);
