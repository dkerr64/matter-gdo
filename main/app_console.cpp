/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

 // System build includes
#include <sdkconfig.h>

#if CONFIG_ENABLE_CHIP_SHELL
// C/C++ includes

// ESP-IDF includes
#include <esp_err.h>
#include <esp_log.h>

// Our project include files...
#include "app_console.h"
#include "app_driver_gdo.h"
#include "app_matter_comms.h"
#include <gdo.h>

static constexpr char *TAG = "gdo_console";

int gdo_command_handler(int argc, char **argv)
{
  esp_err_t err = ESP_OK;
  if (argc < 2)
  {
    printf("Usage: gdo <command> [args]\n");
    printf("Commands:\n");
    printf("  sim <command> - Simulate sending a GDO event to Matter\n");
    printf("  send <command> - Send command to the Garage Door Opener\n");
    return 0;
  }

  if (strcmp(argv[1], "sim") == 0)
  {
    if (argc < 3)
    {
      printf("Usage: gdo sim <command>\n");
      printf("  stopped\n");
      printf("  opening\n");
      printf("  open\n");
      printf("  closing\n");
      printf("  closed\n");
      printf("  light-on\n");
      printf("  light-off\n");
      printf("  obstructed\n");
      printf("  obst-cleared\n");
      printf("  motion\n");
      printf("  motion-cleared\n");
      return 0;
    }

    if (!matter_comms_init_done)
    {
      ESP_LOGE(TAG, "Matter communications not initialized, cannot simulate GDO events");
      return 0;
    }

    const char *command = argv[2];
    ESP_LOGI(TAG, "Simulating GDO command: %s", command);

    if (strcmp(command, "stopped") == 0)
    {
      err = matter_notify_door_state(GDO_DOOR_STATE_STOPPED);
    }
    else if (strcmp(command, "opening") == 0)
    {
      err = matter_notify_door_state(GDO_DOOR_STATE_OPENING);
    }
    else if (strcmp(command, "open") == 0)
    {
      err = matter_notify_door_state(GDO_DOOR_STATE_OPEN);
    }
    else if (strcmp(command, "closing") == 0)
    {
      err = matter_notify_door_state(GDO_DOOR_STATE_CLOSING);
    }
    else if (strcmp(command, "closed") == 0)
    {
      err = matter_notify_door_state(GDO_DOOR_STATE_CLOSED);
    }
    else if (strcmp(command, "light-on") == 0)
    {
      err = matter_notify_light_state(true);
    }
    else if (strcmp(command, "light-off") == 0)
    {
      err = matter_notify_light_state(false);
    }
    else if (strcmp(command, "obstructed") == 0)
    {
      // err = gdo_simulate_obstruction_state(GDO_OBSTRUCTION_STATE_OBSTRUCTED);
    }
    else if (strcmp(command, "obst-cleared") == 0)
    {
      // err = gdo_simulate_obstruction_state(GDO_OBSTRUCTION_STATE_CLEAR);
    }
    else if (strcmp(command, "motion") == 0)
    {
      // err = gdo_simulate_motion_state(GDO_MOTION_STATE_DETECTED);
    }
    else if (strcmp(command, "motion-cleared") == 0)
    {
      // err = gdo_simulate_motion_state(GDO_MOTION_STATE_CLEAR);
    }
    else
    {
      ESP_LOGE(TAG, "Unknown GDO simulation command: %s", command);
      return 0;
    }
    // esp_err_t result = gdo_simulate_command(command);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to simulate GDO command: %s", esp_err_to_name(err));
    }
    return 0;
  }

  if (strcmp(argv[1], "send") == 0)
  {
    if (argc < 3)
    {
      printf("Usage: gdo send <command>\n");
      printf("  open\n");
      printf("  close\n");
      printf("  stop\n");
      printf("  toggle\n");
      printf("  light-on\n");
      printf("  light-off\n");
      return 0;
    }

    if (!gdo_comms_init_done)
    {
      ESP_LOGE(TAG, "GDO communications not initialized, cannot send commands");
      return 0;
    }

    const char *command = argv[2];
    ESP_LOGI(TAG, "Sending GDO command: %s", command);

    if (strcmp(command, "open") == 0)
    {
      err = gdo_door_open();
    }
    else if (strcmp(command, "close") == 0)
    {
      err = gdo_door_close();
    }
    else if (strcmp(command, "stop") == 0)
    {
      err = gdo_door_stop();
    }
    else if (strcmp(command, "toggle") == 0)
    {
      err = gdo_door_toggle();
    }
    else if (strcmp(command, "light-on") == 0)
    {
      err = gdo_light_on();
    }
    else if (strcmp(command, "light-off") == 0)
    {
      err = gdo_light_off();
    }
    else if (strcmp(command, "light-toggle") == 0)
    {
      err = gdo_light_toggle();
    }
    else
    {
      ESP_LOGE(TAG, "Unknown GDO command: %s", command);
      return 0;
    }
    // esp_err_t result = gdo_send_command(command);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to send GDO command: %s", esp_err_to_name(err));
    }
    return 0;
  }

  ESP_LOGE(TAG, "Unknown subcommand: %s", argv[1]);

  return 0;
}

#endif // CONFIG_ENABLE_CHIP_SHELL
