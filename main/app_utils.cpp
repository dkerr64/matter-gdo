/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

// C/C++ language includes
#include <cstring>

// ESP-IDF include files...
#include <esp_matter.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Connected Home Alliance CHIP include files...
#include <app/clusters/time-synchronization-server/CodegenIntegration.h>

// Our project include files...
#include "app_driver_gdo.h"
#include "app_log.h"
#include "app_utils.h"
#include "app_matter_comms.h"

static constexpr const char *TAG = "app_utils";
static bool sntp_init_done = false;

// If known, store the device's IPv4 address here
ip4_addr_t deviceIPv4Address = {};
char deviceIPv4String[INET_ADDRSTRLEN] = "000.000.000.000";

time_t clockSet = 0;

char *timeString(time_t reqTime, bool syslog, char *buffer)
{
  static char tBuffer[32];
  if (buffer == nullptr)
    buffer = tBuffer;
  time_t tTime = 0;
  tm tmTime;
  strcpy(buffer, "[no time set]");
  tTime = ((reqTime == 0) && clockSet) ? time(nullptr) : reqTime;
  if (tTime != 0)
  {
    gmtime_r(&tTime, &tmTime);
    if (syslog)
    {
      // syslog compatible
      strftime(buffer, sizeof(tBuffer), "%Y-%m-%dT%H:%M:%S.000%z", &tmTime);
      // %z returns e.g. "-400" or "+1000", we need it to be "-4:00" or "+10:00"
      // this format is REQUIRED by syslog
      int i = strlen(buffer);
      if (buffer[i - 5] == '-' || buffer[i - 5] == '+' || buffer[i - 6] == '-' || buffer[i - 6] == '+')
      {
        buffer[i + 1] = 0;
        buffer[i] = buffer[i - 1];
        buffer[i - 1] = buffer[i - 2];
        buffer[i - 2] = ':';
      }
    }
    else
    {
      // Print format example: 27-Oct-2024 11:16:18
      strftime(buffer, sizeof(tBuffer), "%d-%b-%Y %H:%M:%S", &tmTime);
    }
  }
  return buffer;
}

time_t app_matter_local_time()
{
  if (!matter_comms_init_done)
    return 0;

  using namespace chip::app::Clusters;
  esp_matter_attr_val_t val;
  esp_matter::attribute::get_val(0, TimeSynchronization::Id, TimeSynchronization::Attributes::LocalTime::Id, &val);

  if (val.is_null())
    return 0;
  uint64_t unix_us;
  if (!chip::ChipEpochToUnixEpochMicros(val.val.u64, unix_us))
    return 0;

  return static_cast<time_t>(unix_us / 1000000ULL);
}

char *localTimeString(time_t reqTime)
{
  static char tBuffer[32];
  return timeString(reqTime == 0 ? app_matter_local_time() : reqTime, false, tBuffer);
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
  settimeofday(tv, nullptr);
  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                      [](intptr_t arg)
                                      {
                                        // Notify the TimeSynchronization cluster of the time sync completion
                                        using namespace chip::app::Clusters::TimeSynchronization;
                                        GetClusterInstance()->OnTimeSyncCompletionFn(TimeSourceEnum::kNonMatterSNTP, GranularityEnum::kMillisecondsGranularity);
                                        ESP_LOGI(TAG, "System clock synchronized with SNTP at: %s (%s UTC)", localTimeString(), timeString());
                                      }));
}

void initialize_sntp()
{
  if (sntp_init_done)
    return;

  sntp_init_done = true;
  constexpr const uint32_t k_sync_interval_ms = 60U * 60U * 1000U;
  ESP_LOGI(TAG, "Initialize SNTP with server: %s and sync interval: %lu minutes", NTP_SERVER, k_sync_interval_ms / 60 / 1000);
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, NTP_SERVER);
  esp_sntp_set_time_sync_notification_cb(on_sntp_time_sync);
  esp_sntp_set_sync_interval(k_sync_interval_ms); // Set sync interval to 1 hour
  esp_sntp_init();
}

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID)
void printTaskInfo(const char *buf)
{
  int count = uxTaskGetNumberOfTasks();
  TaskStatus_t *tasks = static_cast<TaskStatus_t *>(pvPortMalloc(sizeof(TaskStatus_t) * count));
  if (tasks != NULL)
  {
    uxTaskGetSystemState(tasks, count, NULL);
    printf("\n-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
    printf("Name                    Core\tPri\tStack\tState\n");
    for (size_t i = 0; i < count; i++)
    {
      printf("%s\t%s\t%4d\t%3d\t%5d\t%s\n", const_cast<char *>(tasks[i].pcTaskName), strlen(const_cast<char *>(tasks[i].pcTaskName)) > 7 ? "" : "\t",
             (int)(tasks[i].xCoreID < 16) ? tasks[i].xCoreID : -1, (int)tasks[i].uxBasePriority, (int)tasks[i].usStackHighWaterMark, (magic_enum::enum_name(tasks[i].eCurrentState)).data());
    }
    printf("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n\n");
  }
  vPortFree(tasks);
};
#endif // CONFIG_FREERTOS_USE_TRACE_FACILITY

static void reboot_task(void *arg)
{
  vTaskDelay(pdMS_TO_TICKS(1000));

  app_driver_gdo_comms_shutdown();
  esp_restart();

  // Should never get here
  vTaskDelete(NULL);
}

void schedule_reboot() { xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL); }
