/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// System build includes
#include <sdkconfig.h>

// Our App include files...
#include <magic_enum.hpp>

#define ENUM_NAME(x) (magic_enum::enum_name(x).data())
#define ENUM_CAST_NAME(t, x) (magic_enum::enum_name(magic_enum::enum_cast<t>(x).value()).data())
#define ENUM_CAST_NAME_IF(b, t, x) ((b) ? magic_enum::enum_name(magic_enum::enum_cast<t>(x).value()).data() : "NO VALUE")

#define LOG_ON_FALSE(a, format, ...)                                                                                                                                                                   \
  do                                                                                                                                                                                                   \
  {                                                                                                                                                                                                    \
    if (unlikely(!(a)))                                                                                                                                                                                \
    {                                                                                                                                                                                                  \
      ESP_LOGE(TAG, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);                                                                                                                         \
    }                                                                                                                                                                                                  \
  } while (0)

#if defined(CONFIG_GDO_ENABLE_SYSLOG) && defined(CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION)
#include <esp_log.h>
extern void find_mdns_service(const char *service_name, const char *proto);
extern void start_syslog_discovery();
extern void app_log_init();

extern bool app_log_init_done;
#else
#define app_log_init_done false
#endif // CONFIG_GDO_ENABLE_SYSLOG
