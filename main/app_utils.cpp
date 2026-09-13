/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

// C/C++ language includes
#include <cstring>

// ESP-IDF include files...
#include <esp_netif_sntp.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <platform/PlatformManager.h>

// Our project include files...
#include "app_log.h"
#include "app_utils.h"

static constexpr char *TAG = "app_utils";

// If known, store the device's IPv4 address here
ip4_addr_t deviceIPv4Address = {};
char deviceIPv4String[INET_ADDRSTRLEN] = "000.000.000.000";

time_t clockSet = 0;
bool enableNTP = false;

char *timeString(time_t reqTime, bool syslog)
{
  static char tBuffer[32];
  time_t tTime = 0;
  tm tmTime;
  strcpy(tBuffer, "[no time set]");
  tTime = ((reqTime == 0) && clockSet) ? time(NULL) : reqTime;
  if (tTime != 0)
  {
    localtime_r(&tTime, &tmTime);
    if (syslog)
    {
      // syslog compatible
      strftime(tBuffer, sizeof(tBuffer), "%Y-%m-%dT%H:%M:%S.000%z", &tmTime);
      // %z returns e.g. "-400" or "+1000", we need it to be "-4:00" or "+10:00"
      // this format is REQUIRED by syslog
      int i = strlen(tBuffer);
      if (tBuffer[i - 5] == '-' || tBuffer[i - 5] == '+' || tBuffer[i - 6] == '-' || tBuffer[i - 6] == '+')
      {
        tBuffer[i + 1] = 0;
        tBuffer[i] = tBuffer[i - 1];
        tBuffer[i - 1] = tBuffer[i - 2];
        tBuffer[i - 2] = ':';
      }
    }
    else
    {
      // Print format example: 27-Oct-2024 11:16:18 EDT
      strftime(tBuffer, sizeof(tBuffer), "%d-%b-%Y %H:%M:%S %Z", &tmTime);
    }
  }
  return tBuffer;
}

char *toHHMMSSmmm(_millis_t t)
{
  static char timestr[32];
  uint32_t secs = t / 1000;
  uint32_t ms = t % 1000;
  uint32_t mins = secs / 60;
  secs = secs % 60;
  uint32_t hrs = mins / 60;
  mins = mins % 60;
  snprintf(timestr, sizeof(timestr), "%02lu:%02lu:%02lu.%03lu", hrs, mins, secs, ms);
  return timestr;
}

static void on_sntp_time_sync(struct timeval *tv)
{
  // Keep this callback function short and simple. Do not use our logging functions
  clockSet = tv->tv_sec;
  chip::DeviceLayer::ChipDeviceEvent event = {};
  event.Type = chip::DeviceLayer::DeviceEventType::kTimeSyncChange;
  event.TimeSyncChange.IsTimeSynchronized = true;
  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().PostEvent(&event));
}

void initialize_sntp()
{
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
  config.sync_cb = on_sntp_time_sync;
  config.wait_for_sync = false;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_sntp_init(&config));
}

#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
void printTaskInfo(const char *buf)
{
  int count = uxTaskGetNumberOfTasks();
  TaskStatus_t *tasks = static_cast<TaskStatus_t *>(pvPortMalloc(sizeof(TaskStatus_t) * count));
  if (tasks != NULL)
  {
    uxTaskGetSystemState(tasks, count, NULL);
    Serial.print("\n-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
    Serial.print("Name                    Core\tPri\tStack\tState\n");
    for (size_t i = 0; i < count; i++)
    {
      Serial.printf("%s\t%s\t%4d\t%3d\t%5d\t%s\n", const_cast<char *>(tasks[i].pcTaskName), strlen(const_cast<char *>(tasks[i].pcTaskName)) > 7 ? "" : "\t",
                    (int)(tasks[i].xCoreID < 16) ? tasks[i].xCoreID : -1, (int)tasks[i].uxBasePriority, (int)tasks[i].usStackHighWaterMark, (magic_enum::enum_name(tasks[i].eCurrentState)).data());
    }
    Serial.print("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n\n");
  }
  vPortFree(tasks);
};
#endif // CONFIG_FREERTOS_USE_TRACE_FACILITY

static void reboot_task(void *arg)
{
  vTaskDelay(pdMS_TO_TICKS(1000));

  esp_restart();

  // Should never get here
  vTaskDelete(NULL);
}

void schedule_reboot() { xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL); }
