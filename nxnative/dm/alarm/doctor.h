// nx2alarm device and prerequisite audit: declarations.
// The audit is shared by `nx2alarm doctor` and the shell window; the report
// types live in alarm.h because the shell renders the same rows.
#ifndef DM_ALARM_DOCTOR_H
#define DM_ALARM_DOCTOR_H

#include "alarm.h"

// Fill rep with the device summary and the prerequisite checks.
// with_sdl = false skips SDL video/audio/gamepad probing (headless mode);
// with_sdl = true initializes the needed SDL subsystems only for the duration
// of the call and quits those it initialized itself.
void dm_doctor_collect(dm_report *rep, bool with_sdl);

// Print the full report to stdout (one PASS/WARN/FAIL line per check).
void dm_doctor_print(const dm_report *rep);

// 0 when no check failed, 1 when at least one check failed (warnings do not fail).
int dm_doctor_exit_code(const dm_report *rep);

// `nx2alarm doctor`: collect, print and return the exit code.
int dm_alarm_doctor_command(void);

#endif  // DM_ALARM_DOCTOR_H
