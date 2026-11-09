#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SNTP starten (nach net_wifi_start). */
void net_time_start(void);

/* true, sobald eine plausible Uhrzeit vorliegt. */
bool net_time_valid(void);

#ifdef __cplusplus
}
#endif
