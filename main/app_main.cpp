/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

// C/C++ language includes

// ESP-IDF include files...
#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

// Our project include files...
#include "app_console.h"
#include "app_driver_gdo.h"
#include "app_log.h"
#include "app_matter_comms.h"
#include "app_utils.h"
#include <gdo.h>

static constexpr char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

extern "C" void app_main()
{
  // Initialize our log hooks so we can log to syslog server advertized by mDNS
  app_log_init();

#ifdef CONFIG_CHIP_LOG_FILTERING
  // Default logging is way too verbose...
  chip::Logging::SetLogFilter(chip::Logging::kLogCategory_Error);
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
  /* Set OpenThread platform config */
  esp_openthread_platform_config_t config = {
      .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
      .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
      .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
  };
  set_openthread_platform_config(&config);
#endif

  // Initialize Garage Door Opener (GDO) communications
  app_driver_gdo_comms_init();
  // Initialize Matter communications
  matter_comms_init();
  // Initialize SNTP for time synchronization
  initialize_sntp();

#if CONFIG_ENABLE_ENCRYPTED_OTA
  err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
  ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
  // Register our custom GDO command handler with the console.
  const esp_console_cmd_t gdo_cmd = {
      .command = "gdo",
      .help = "send or simulate GDO commands for testing",
      .hint = NULL,
      .func = &gdo_command_handler,
  };
  esp_console_cmd_register(&gdo_cmd);
#endif
}
