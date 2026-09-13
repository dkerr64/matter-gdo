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

// Our App include files...
#include <magic_enum.hpp>

#define ENUM_NAME(x) (magic_enum::enum_name(x).data())
#define ENUM_CAST_NAME(t, x) (magic_enum::enum_name(magic_enum::enum_cast<t>(x).value()).data())
#define ENUM_CAST_NAME_IF(b, t, x) ((b) ? magic_enum::enum_name(magic_enum::enum_cast<t>(x).value()).data() : "NO VALUE")

#define LOG_IF_ERROR(fn, msg)                                                                                                                                                                          \
  do                                                                                                                                                                                                   \
  {                                                                                                                                                                                                    \
    esp_err_t e = (fn);                                                                                                                                                                                \
    if (e != ESP_OK)                                                                                                                                                                                   \
    {                                                                                                                                                                                                  \
      ESP_LOGE(TAG, "%s: %d", msg, e);                                                                                                                                                                 \
    }                                                                                                                                                                                                  \
  } while (0)

/** Remap attribute values
 *
 * This can be used to remap attribute values to different ranges.
 * Example: To convert the brightness value (0-255) into brightness percentage (0-100) and vice-versa.
 */
#define REMAP_TO_RANGE(value, from, to) ((value * to) / from)

/** Remap attribute values with inverse dependency
 *
 * This can be used to remap attribute values with inverse dependency to different ranges.
 * Example: To convert the temperature mireds into temperature kelvin and vice-versa where the relation between them
 * is: Mireds = 1,000,000/Kelvin.
 */
#define REMAP_TO_RANGE_INVERSE(value, factor) (factor / (value ? value : 1))

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
