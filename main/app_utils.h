/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// C/C++ language include files...
#include <ctime>
#include <stdlib.h>

// ESP-IDF include files...
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define ABORT_APP_ON_FAILURE(x, ...)                                                                                                                                                                   \
  do                                                                                                                                                                                                   \
  {                                                                                                                                                                                                    \
    if (!(unlikely(x)))                                                                                                                                                                                \
    {                                                                                                                                                                                                  \
      __VA_ARGS__;                                                                                                                                                                                     \
      vTaskDelay(5000 / portTICK_PERIOD_MS);                                                                                                                                                           \
      abort();                                                                                                                                                                                         \
    }                                                                                                                                                                                                  \
  } while (0)

typedef int64_t _millis_t;
#define _millis() ((_millis_t)(esp_timer_get_time() / 1000LL))

extern time_t clockSet;
extern uint64_t lastRebootAt;
extern char *timeString(time_t reqTime = 0, bool syslog = false);
extern char *toHHMMSSmmm(_millis_t t = _millis());
extern bool enableNTP;
extern bool get_auto_timezone();
#define NTP_SERVER "pool.ntp.org"
extern ip4_addr_t deviceIPv4Address;
extern char deviceIPv4String[INET_ADDRSTRLEN];

#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
extern void printTaskInfo(const char *buf = nullptr);
#endif

extern void initialize_sntp();
extern void schedule_reboot();
