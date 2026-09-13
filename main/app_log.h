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

#ifdef CONFIG_GDO_ENABLE_SYSLOG
#include <esp_log.h>
extern void find_mdns_service(const char *service_name, const char *proto);
extern void start_syslog_discovery();
extern void app_log_init();

extern bool app_log_init_done;
#else
#define app_log_init()                                                                                                                                                                                 \
  do                                                                                                                                                                                                   \
  {                                                                                                                                                                                                    \
  } while (0)
#define start_syslog_discovery()                                                                                                                                                                       \
  do                                                                                                                                                                                                   \
  {                                                                                                                                                                                                    \
  } while (0)
#define app_log_init_done false
#endif // CONFIG_GDO_ENABLE_SYSLOG
