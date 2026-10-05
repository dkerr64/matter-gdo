/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

// System build includes
#include <sdkconfig.h>

// C/C++ includes
#include <map>

// ESP-IDF includes
#include <app-common/zap-generated/attributes/Accessors.h>

// Our project include files...
#include "app_config.h"
#include "app_driver_gdo.h"
#include "app_matter_comms.h"
#include "app_utils.h"
#include "gdo.h"

using namespace chip::app::Clusters;
using namespace chip::app::Clusters::Globals;
using namespace chip::app::Clusters::ClosureControl;
using namespace chip::app::Clusters::ClosureDimension;

#if !defined(CONFIG_GDO_UART_TX_PIN) || CONFIG_GDO_UART_TX_PIN == 0
#error "CONFIG_GDO_UART_TX_PIN not defined, run menuconfig"
#endif
#if !defined(CONFIG_GDO_UART_RX_PIN) || CONFIG_GDO_UART_RX_PIN == 0
#error "CONFIG_GDO_UART_RX_PIN not defined, run menuconfig"
#endif
#if !defined(CONFIG_GDO_OBST_PIN) || CONFIG_GDO_OBST_PIN == 0
#error "CONFIG_GDO_OBST_PIN not defined, run menuconfig"
#endif
#if !defined(CONFIG_GDO_DRY_CONTACT_OPEN_PIN) || CONFIG_GDO_DRY_CONTACT_OPEN_PIN == 0
#error "CONFIG_GDO_DRY_CONTACT_OPEN_PIN not defined, run menuconfig"
#endif
#if !defined(CONFIG_GDO_DRY_CONTACT_CLOSE_PIN) || CONFIG_GDO_DRY_CONTACT_CLOSE_PIN == 0
#error "CONFIG_GDO_DRY_CONTACT_CLOSE_PIN not defined, run menuconfig"
#endif
#if !defined(CONFIG_GDO_DRY_CONTACT_LIGHT_PIN) || CONFIG_GDO_DRY_CONTACT_LIGHT_PIN == 0
#error "CONFIG_GDO_DRY_CONTACT_LIGHT_PIN not defined, run menuconfig"
#endif

static constexpr char *TAG = "app_driver_gdo";
bool gdo_comms_init_done = false;

static gdo_status_t gdo_status;
uint32_t last_saved_code = 0;
#define MAX_CODES_WITHOUT_FLASH_WRITE 10

constexpr std::string_view ESP32_BOARD = CONFIG_IDF_TARGET;

/****************************************************************************
 * Helper functions for GDO communications.
 */
static void save_rolling_code(uint32_t rolling_code = 0)
{
  if (rolling_code == 0 && gdo_status.rolling_code != 0)
    gdo_get_status(&gdo_status); // get most recent rolling code if we are not resetting it.
  else
    gdo_status.rolling_code = rolling_code;

  ESP_LOGI(TAG, "Save rolling code: %d", gdo_status.rolling_code);
  write_nvs_int(nvram_rolling, gdo_status.rolling_code);
  last_saved_code = gdo_status.rolling_code;
}

/****************************************************************************
 * Callback for GDOLIB status
 */
static void gdo_event_handler(const gdo_status_t *status, gdo_cb_event_t event, void *arg)
{
  // make a local copy of the status
  // we call function because it makes an interrupt/ISR and thread-safe copy of the status
  gdo_get_status(&gdo_status);

  switch (event)
  {
  case GDO_CB_EVENT_SYNCED:
    ESP_LOGI(TAG, "GDO event: synced: %s, protocol: %s", gdo_status.synced ? "true" : "false", gdo_protocol_type_to_string(gdo_status.protocol));
    if (gdo_status.protocol == GDO_PROTOCOL_SEC_PLUS_V2)
    {
      ESP_LOGI(TAG, "Client ID: %" PRIu32 ", Rolling code: %" PRIu32, gdo_status.client_id, gdo_status.rolling_code);
    }

    if (!gdo_status.synced)
    {
      if (gdo_set_rolling_code(gdo_status.rolling_code + 100) != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to set rolling code");
      }
      else
      {
        ESP_LOGI(TAG, "Rolling code set to %" PRIu32 ", retrying sync", gdo_status.rolling_code);
        gdo_sync();
      }
    }
    break;
  case GDO_CB_EVENT_LIGHT:
    ESP_LOGI(TAG, "GDO event: light: %s (%s)", gdo_light_state_to_string(gdo_status.light), timeString());
    matter_notify_light_state(gdo_status.light == gdo_light_state_t::GDO_LIGHT_STATE_ON);
    break;
  case GDO_CB_EVENT_LOCK:
    ESP_LOGI(TAG, "GDO event: lock remotes: %s (%s)", gdo_lock_state_to_string(gdo_status.lock), timeString());
    matter_notify_lock_state(gdo_status.lock == gdo_lock_state_t::GDO_LOCK_STATE_LOCKED);
    break;
  case GDO_CB_EVENT_DOOR_POSITION:
  {
    ESP_LOGI(TAG, "GDO event: door: %s, %3d, target: %3d", gdo_door_state_to_string(gdo_status.door), gdo_status.door_position / 100,
             (gdo_status.door_target >= 0) ? gdo_status.door_target / 100 : -1);

    matter_notify_door_state(gdo_status.door);
    /*
    garage_door.active = true;
    if ((garage_door.current_state != gdo_to_homekit_door_current_state[gdo_status.door]) && (gdo_status.door != GDO_DOOR_STATE_UNKNOWN))
    {
      // notify_homekit_current_door_state_change(gdo_to_homekit_door_current_state[gdo_status.door]);
      garage_door.current_state = gdo_to_homekit_door_current_state[gdo_status.door];
      // digitalWrite(STATUS_DOOR_PIN, garage_door.current_state == GarageDoorCurrentState::CURR_CLOSED ? LOW : HIGH);
      // notify_homekit_target_door_state_change(gdo_to_homekit_door_target_state[gdo_status.door]);

      // If we are using Sec+2.0 built-in time-to-close then reset the TTC to zero when door is closed
      if (gdo_status.door == GDO_DOOR_STATE_CLOSED && doorControlType == DOOR_CONTROL_SEC_PLUS_V2 && userConfig->getBuiltInTTC())
        gdo_set_time_to_close(0);
    }
    */
    break;
  }
  case GDO_CB_EVENT_LEARN:
    ESP_LOGI(TAG, "GDO event: learn: %s", gdo_learn_state_to_string(gdo_status.learn));
    break;
  case GDO_CB_EVENT_OBSTRUCTION:
    ESP_LOGI(TAG, "GDO event: obstruction: %s (%s)", gdo_obstruction_state_to_string(gdo_status.obstruction), timeString());
    matter_notify_obstruction_state(gdo_status.obstruction == gdo_obstruction_state_t::GDO_OBSTRUCTION_STATE_OBSTRUCTED);
    break;
  case GDO_CB_EVENT_MOTION:
    ESP_LOGI(TAG, "GDO event: motion: %s", gdo_motion_state_to_string(gdo_status.motion));
    matter_notify_occupancy_state(gdo_status.motion == gdo_motion_state_t::GDO_MOTION_STATE_DETECTED);
    break;
  case GDO_CB_EVENT_BATTERY:
    ESP_LOGI(TAG, "GDO event: battery: %s", gdo_battery_state_to_string(gdo_status.battery));
    // garage_door.batteryState = gdo_status.battery;
    break;
  case GDO_CB_EVENT_BUTTON:
    ESP_LOGI(TAG, "GDO event: button: %s", gdo_button_state_to_string(gdo_status.button));
    // if (gdo_status.button == GDO_BUTTON_STATE_PRESSED)
    //  manual_recovery();
    break;
  case GDO_CB_EVENT_MOTOR:
    ESP_LOGI(TAG, "GDO event: motor: %s", gdo_motor_state_to_string(gdo_status.motor));
    break;
  case GDO_CB_EVENT_OPENINGS:
    ESP_LOGI(TAG, "GDO event: openings: %d", gdo_status.openings);
    // garage_door.openingsCount = gdo_status.openings;
    break;
  case GDO_CB_EVENT_SET_TTC:
    ESP_LOGI(TAG, "GDO event: set TTC: %d", gdo_status.ttc_seconds);
    break;
  case GDO_CB_EVENT_UPDATE_TTC:
    ESP_LOGI(TAG, "GDO event: update TTC: %d", gdo_status.ttc_seconds);
    break;
  case GDO_CB_EVENT_CANCEL_TTC:
    ESP_LOGI(TAG, "GDO event: cancel TTC");
    /*
    if (doorControlType == DOOR_CONTROL_SEC_PLUS_V2 && userConfig->getBuiltInTTC())
      gdo_set_time_to_close(0);
    */
    break;
  case GDO_CB_EVENT_PAIRED_DEVICES:
    ESP_LOGI(TAG, "GDO event: paired devices, %d remotes, %d keypads, %d wall controls, %d accessories, %d total", gdo_status.paired_devices.total_remotes, gdo_status.paired_devices.total_keypads,
             gdo_status.paired_devices.total_wall_controls, gdo_status.paired_devices.total_accessories, gdo_status.paired_devices.total_all);
    break;
  case GDO_CB_EVENT_OPEN_DURATION_MEASUREMENT:
    ESP_LOGI(TAG, "GDO event: open duration: %d seconds (%s)", gdo_status.open_ms, timeString());
    write_nvs_int(nvram_open_history, gdo_status.open_ms);
    break;
  case GDO_CB_EVENT_CLOSE_DURATION_MEASUREMENT:
    ESP_LOGI(TAG, "GDO event: close duration: %d seconds (%s)", gdo_status.close_ms, timeString());
    write_nvs_int(nvram_close_history, gdo_status.close_ms);
    break;
  default:
    ESP_LOGI(TAG, "GDO event: unknown: %d", event);
    break;
  }
  // Save rolling code if we have exceeded max limit.
  if (gdo_status.rolling_code >= (last_saved_code + MAX_CODES_WITHOUT_FLASH_WRITE))
  {
    save_rolling_code(gdo_status.rolling_code);
  }
}

/****************************************************************************
 * Initialization
 *
 */
void app_driver_gdo_comms_init()
{
  if (gdo_comms_init_done)
    return;

  ESP_LOGI(TAG, "Initialize GDO communications on UART%d (TX: %d, RX: %d), Obstruction pin: %d", UART_NUM_1, CONFIG_GDO_UART_TX_PIN, CONFIG_GDO_UART_RX_PIN, CONFIG_GDO_OBST_PIN);

  esp_err_t err = ESP_OK;

  {
    gdo_config_t gdo_conf = {
        .uart_num = UART_NUM_1,
        .obst_from_status = false, // userConfig->getObstFromStatus(),
        .invert_uart = true,
        .uart_tx_pin = static_cast<gpio_num_t>(CONFIG_GDO_UART_TX_PIN),
        .uart_rx_pin = static_cast<gpio_num_t>(CONFIG_GDO_UART_RX_PIN),
        .obst_in_pin = static_cast<gpio_num_t>(CONFIG_GDO_OBST_PIN),
        .obst_tp_pin = GPIO_NUM_0,  // only used for testing obstruction sensor
        .dc_open_pin = GPIO_NUM_0,  // disable dry-contact
        .dc_close_pin = GPIO_NUM_0, // disable dry-contact
        .dc_discrete_open_pin = GPIO_NUM_0,
        .dc_discrete_close_pin = GPIO_NUM_0,
        //.use_sw_serial = userConfig->getUseSWserial(),
    };
    /*
    if (userConfig->getDCOpenClose())
    {
      // Enable dry-contact (to trigger door open/close)
      gdo_conf.dc_open_pin = DRY_CONTACT_OPEN_PIN;
      gdo_conf.dc_close_pin = DRY_CONTACT_CLOSE_PIN;
    }
    */
    if ((err = gdo_init(&gdo_conf)) != ESP_OK)
    {
      ESP_LOGE(TAG, "gdo_init failed with error: %d", err);
      return;
    }

    // read from flash, default of 0 if file not exist
    uint32_t id_code = read_nvs_int(nvram_id_code);
    uint32_t rolling_code = read_nvs_int(nvram_rolling);
    if (!id_code || !rolling_code)
    {
      ESP_LOGI(TAG, "Generate new Sec+2.0 id code");
      id_code = ((1 + (esp_random() % 0xFFE)) << 12) | 0x539;
      write_nvs_int(nvram_id_code, id_code);
    }
    ESP_LOGI(TAG, "id code %lu (0x%02lX)", id_code, id_code);

    // last saved rolling code may be behind what the GDO thinks, so bump it up so that it will
    // always be ahead of what the GDO thinks it should be, and save it.
    rolling_code = (rolling_code != 0) ? rolling_code + MAX_CODES_WITHOUT_FLASH_WRITE : 0;
    ESP_LOGI(TAG, "rolling code %lu (0x%02X)", rolling_code, rolling_code);
    gdo_set_client_id(id_code);
    gdo_set_rolling_code(rolling_code);
    save_rolling_code(rolling_code);

    gdo_set_open_duration(read_nvs_int(nvram_open_history));
    gdo_set_close_duration(read_nvs_int(nvram_close_history));
  }

  if ((err = gdo_start(gdo_event_handler, NULL)) != ESP_OK)
  {
    ESP_LOGE(TAG, "gdo_start failed with error: %d", err);
    return;
  }
  gdo_get_status(&gdo_status);

  gdo_comms_init_done = true;
}

void app_driver_gdo_comms_shutdown()
{
  if (!gdo_comms_init_done)
    return;

  gdo_comms_init_done = false;
  esp_err_t err = gdo_deinit();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "GDO communications shutdown failed: %s", esp_err_to_name(err));
  }
}

gdo_door_state_t app_driver_get_door_state() { return gdo_status.door; }

/****************************************************************************
 * Notification from Matter that some attribute has changed
 *
 */
esp_err_t app_driver_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val)
{
  esp_err_t err = ESP_OK;
  if (!gdo_comms_init_done)
    return err;

  if (endpoint_id == closure_endpoint_id)
  {
    return err;
  }

  if (endpoint_id == dimension_endpoint_id)
  {
    // val->type for this attribute is expected to be ESP_MATTER_VAL_TYPE_UINT16;
    if (cluster_id == ClosureDimension::Id)
    {
      if (attribute_id == ClosureDimension::Attributes::Resolution::Id)
      {
        ESP_LOGI(TAG, "Closure dimension attribute updated: resolution = %u", val->val.u16);
      }
      else if (attribute_id == ClosureDimension::Attributes::StepValue::Id)
      {
        ESP_LOGI(TAG, "Closure dimension attribute updated: step value = %u", val->val.u16);
      }
    }
    return err;
  }

  if (endpoint_id == light_endpoint_id)
  {
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)
    {
      if (val->val.b)
        err = gdo_light_on();
      else
        err = gdo_light_off();
    }
    return err;
  }

  if (endpoint_id == lock_endpoint_id)
  {
    return err;
  }

  if (endpoint_id == occupancy_endpoint_id)
  {
    if (cluster_id == OccupancySensing::Id && attribute_id == OccupancySensing::Attributes::HoldTime::Id)
    {
      // val->type for this attribute is expected to be ESP_MATTER_VAL_TYPE_UINT16;
      err = gdo_set_motion_timeout(val->val.u16 * 1000); // Convert hold time from seconds to milliseconds
      ESP_LOGI(TAG, "Occupancy attribute updated: hold time = %u", val->val.u16);
    }
    return err;
  }

  return err;
}
