/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#define CHIP_CONFIG_ENABLE_CONDITION_LOGGING 1
// C/C++ language include files...

// ESP-IDF include files...
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>

// ESP-Matter SDK include files...
#include <clusters/closure_control/integration.h>
#include <clusters/closure_dimension/integration.h>
#include <clusters/occupancy_sensing/integration.h>
#include <clusters/time_synchronization/integration.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_cluster.h>
#include <esp_matter_console.h>
#include <esp_matter_data_model.h>
#include <esp_matter_data_model_provider.h>
#include <esp_matter_ota.h>

// Connected Home Alliance CHIP include files...
#include <app/clusters/closure-control-server/ClosureControlCluster.h>
#include <app/clusters/closure-dimension-server/ClosureDimensionCluster.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>
#include <app/clusters/time-synchronization-server/DefaultTimeSyncDelegate.h>
#include <platform/CHIPDeviceLayer.h>

// Our project include files...
#include "app_config.h"
#include "app_driver_gdo.h"
#include "app_log.h"
#include "app_matter_comms.h"
#include "app_utils.h"
#include <gdo.h>

// Set namespaces for convenience...
using namespace esp_matter;
// using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;
using namespace esp_matter::attribute;

using namespace chip::app;
using namespace chip::app::DataModel;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::Globals;
using namespace chip::app::Clusters::ClosureControl;
using namespace chip::app::Clusters::ClosureDimension;
using namespace chip::app::Clusters::OccupancySensing;

using namespace chip::app::Clusters::ModeSelect;

/****************************************************************************
 * Global storage definitions
 *
 */
static constexpr char *TAG = "app_matter_comms";
bool matter_comms_init_done = false;

uint16_t closure_endpoint_id = 0;
uint16_t dimension_endpoint_id = 0;
uint16_t light_endpoint_id = 0;
uint16_t lock_endpoint_id = 0;
uint16_t mode_select_endpoint_id = 0;
uint16_t occupancy_endpoint_id = 0;

static ClosureControlCluster *control_cluster = nullptr;
static ClosureDimensionCluster *dimension_cluster = nullptr;
static OccupancySensingCluster *occupancy_cluster = nullptr;

constexpr uint16_t k_timeout_seconds = 300;

// Values of the ModeSelect cluster's mode options
enum class GdoMode : uint8_t {
  Normal = 0,
  Reboot = 1,
  Calibrate = 2,
  CalibrateAuto = 3,
};

static TaskHandle_t matter_calibrate_task_handle = nullptr;
static void matter_calibrate_task(void *arg);
static bool calibrating = false;
static uint32_t expected_calibration_ms = 0;
static constexpr const uint32_t DEFAULT_EXPECTED_CALIBRATION_MS = 120 * 1000;
static esp_err_t matter_start_calibration(bool automatic); // defined after closure_delegate

/****************************************************************************
 * Callbacks
 *
 */

void emberAfDoorLockClusterInitCallback(chip::EndpointId) {}

bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId, const chip::app::DataModel::Nullable<chip::FabricIndex> &, const chip::app::DataModel::Nullable<chip::NodeId> &,
                                            const chip::Optional<chip::ByteSpan> &, chip::app::Clusters::DoorLock::OperationErrorEnum &error)
{
  esp_err_t result = gdo_lock();
  if (result != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to queue GDO lock command: %s", esp_err_to_name(result));
    error = chip::app::Clusters::DoorLock::OperationErrorEnum::kUnspecified;
    return false;
  }

  ESP_LOGI(TAG, "Queued GDO lock command");
  return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId, const chip::app::DataModel::Nullable<chip::FabricIndex> &, const chip::app::DataModel::Nullable<chip::NodeId> &,
                                              const chip::Optional<chip::ByteSpan> &, chip::app::Clusters::DoorLock::OperationErrorEnum &error)
{
  esp_err_t result = gdo_unlock();
  if (result != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to queue GDO unlock command: %s", esp_err_to_name(result));
    error = chip::app::Clusters::DoorLock::OperationErrorEnum::kUnspecified;
    return false;
  }

  ESP_LOGI(TAG, "Queued GDO unlock command");
  return true;
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
  ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
  return ESP_OK;
}

// esp_matter has no generic accessor, so widen the value according to its type (for logging).
static int64_t attr_val_as_int64(const esp_matter_attr_val_t *val)
{
  switch (val->get_storage_type())
  {
  case ESP_MATTER_VAL_TYPE_BOOLEAN:
    return val->val.b;
  case ESP_MATTER_VAL_TYPE_INT8:
    return val->val.i8;
  case ESP_MATTER_VAL_TYPE_UINT8:
    return val->val.u8;
  case ESP_MATTER_VAL_TYPE_INT16:
    return val->val.i16;
  case ESP_MATTER_VAL_TYPE_UINT16:
    return val->val.u16;
  case ESP_MATTER_VAL_TYPE_INT32:
    return val->val.i32;
  case ESP_MATTER_VAL_TYPE_UINT32:
    return val->val.u32;
  case ESP_MATTER_VAL_TYPE_INT64:
    return val->val.i64;
  case ESP_MATTER_VAL_TYPE_UINT64:
    return (int64_t)val->val.u64;
  default:
    return 0; // float, strings, arrays etc. are not integers
  }
}

// Endpoint IDs are assigned at runtime, so compare against the ones we created.
static const char *endpoint_name(uint16_t endpoint_id)
{
  if (endpoint_id == 0)
    return "root";
  if (endpoint_id == closure_endpoint_id)
    return "closure";
  if (endpoint_id == dimension_endpoint_id)
    return "dimension";
  if (endpoint_id == light_endpoint_id)
    return "light";
  if (endpoint_id == lock_endpoint_id)
    return "lock";
  if (endpoint_id == mode_select_endpoint_id)
    return "mode_select";
  if (endpoint_id == occupancy_endpoint_id)
    return "occupancy";
  return "unknown";
}

static const char *cluster_name(uint32_t cluster_id)
{
  switch (cluster_id)
  {
  case BasicInformation::Id:
    return "BasicInformation";
  case ClosureControl::Id:
    return "ClosureControl";
  case ClosureDimension::Id:
    return "ClosureDimension";
  case DoorLock::Id:
    return "DoorLock";
  case ModeSelect::Id:
    return "ModeSelect";
  case OccupancySensing::Id:
    return "OccupancySensing";
  case OnOff::Id:
    return "OnOff";
  default:
    return "unknown";
  }
}

// Attribute IDs are only unique within a cluster, so the cluster is needed too.
static const char *attribute_name(uint32_t cluster_id, uint32_t attribute_id)
{
  // Global attributes common to every cluster
  switch (attribute_id)
  {
  case 0xFFFC:
    return "FeatureMap";
  case 0xFFFD:
    return "ClusterRevision";
  default:
    break;
  }

  switch (cluster_id)
  {
  case ClosureDimension::Id:
    if (attribute_id == ClosureDimension::Attributes::Resolution::Id)
      return "Resolution";
    if (attribute_id == ClosureDimension::Attributes::StepValue::Id)
      return "StepValue";
    break;
  case DoorLock::Id:
    if (attribute_id == DoorLock::Attributes::LockState::Id)
      return "LockState";
    break;
  case ModeSelect::Id:
    if (attribute_id == ModeSelect::Attributes::CurrentMode::Id)
      return "CurrentMode";
    break;
  case OccupancySensing::Id:
    if (attribute_id == OccupancySensing::Attributes::Occupancy::Id)
      return "Occupancy";
    if (attribute_id == OccupancySensing::Attributes::HoldTime::Id)
      return "HoldTime";
    break;
  case OnOff::Id:
    if (attribute_id == OnOff::Attributes::OnOff::Id)
      return "OnOff";
    break;
  default:
    break;
  }
  return "unknown";
}

// This callback is called for every attribute update. The callback
// implementation shall handle the desired attributes and return an appropriate
// error code. If the attribute is not of your interest, please do not return an
// error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
  uint32_t int_val = static_cast<uint32_t>(attr_val_as_int64(val));
  ESP_LOGI(TAG, "Attribute %s, endpoint: %u (%s), cluster: 0x%04x (%s), attribute: 0x%04x (%s), type: %s, value: %ld", ENUM_NAME(type), endpoint_id, endpoint_name(endpoint_id), cluster_id,
           cluster_name(cluster_id), attribute_id, attribute_name(cluster_id, attribute_id), ENUM_NAME(val->type), int_val);

  if (type == PRE_UPDATE)
  {
    // PRE_UPDATE means the attribute is about to be updated in the Matter SDK's database.
    // No validity check has been performed yet, so value may be invalid.
    // For example, we WILL get here if the update is to turn on the light, regardless of its current state.

    if (endpoint_id == light_endpoint_id || endpoint_id == lock_endpoint_id)
    {
      // Handle light and lock even if the value is same as current state, just in case
      // we have got out-of-sync with the actual state.
      return app_driver_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }

    if (endpoint_id == closure_endpoint_id)
    {
      // Handle closure, dimension
      return app_driver_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }
  }
  else if (type == POST_UPDATE)
  {
    // POST_UPDATE means the attribute has been updated successfully in the Matter SDK's database.
    // We will only get here if attribute value is in correct range and an update was made.
    // For example, we will NOT get here if the update is to turn on the light, and the light was already on.

    if (endpoint_id == occupancy_endpoint_id)
    {
      // Handle occupancy endpoint specific attributes
      return app_driver_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }

    if (endpoint_id == dimension_endpoint_id)
    {
      // Handle dimension endpoint specific attributes
      return app_driver_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }

    if (endpoint_id == mode_select_endpoint_id)
    {
      // Handle mode select endpoint specific attributes
      // ID = 3, Type = 8, Value = 0 (normal), 1 (reboot) or 2 (calibrate)
      if (int_val == (uint32_t)GdoMode::Reboot)
      {
        ESP_LOGI(TAG, "Mode select attribute triggered reboot (1 second delay)");
        schedule_reboot();
        matter_set_mode((uint8_t)GdoMode::Normal); // return to normal
      }
      else if (int_val == (uint32_t)GdoMode::Calibrate || int_val == (uint32_t)GdoMode::CalibrateAuto)
      {
        // Using mode_select to trigger calibration as current clients (like Home Assistant) do not have a dedicated calibration command yet
        // TODO: Remove/replace with a dedicated calibration command when available
        bool automatic = (int_val == (uint32_t)GdoMode::CalibrateAuto);
        ESP_LOGI(TAG, "Mode select attribute triggered %s calibration", automatic ? "automatic" : "simple");
        esp_err_t err = matter_start_calibration(automatic);
        matter_set_mode((uint8_t)GdoMode::Normal); // return to normal so calibrate can be selected again
        return err;
      }
      return ESP_OK;
    }
  }

  return ESP_OK;
}

/****************************************************************************
 * Matter event callback
 *
 */
static void matter_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
  switch (event->Type)
  {
  case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
  {
    chip::DeviceLayer::InterfaceIpChangeType changeType = event->InterfaceIpAddressChanged.Type;
    ESP_LOGD(TAG, "Interface IP Address changed: %s", ENUM_NAME(changeType));
    break;
  }

  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    clockSet = time(0);
    ESP_LOGI(TAG, "Time synchronized: %s", timeString());
    // disable BLE to free up resources.
    // app_ble_disable();
    break;

  case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGE(TAG, "Commissioning failed, fail safe timer expired");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
    ESP_LOGI(TAG, "Commissioning session started");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
    ESP_LOGI(TAG, "Commissioning session stopped");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
  {
    ESP_LOGI(TAG, "Fabric removed successfully");
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
    {
      chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
      constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
      if (!commissionMgr.IsCommissioningWindowOpen())
      {
        /* After removing last fabric, this example does not remove the Wi-Fi
         * credentials and still has IP connectivity so, only advertising on
         * DNS-SD.
         */
        CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds, chip::CommissioningWindowAdvertisement::kDnssdOnly);
        if (err != CHIP_NO_ERROR)
        {
          ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
        }
      }
    }
    break;
  }

  case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
    ESP_LOGI(TAG, "Fabric will be removed");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
    ESP_LOGI(TAG, "Fabric is updated");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
    ESP_LOGI(TAG, "Fabric is committed");
    break;

  case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
    ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
    break;

  case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
  {
    const auto &sys = event->Platform.ESPSystemEvent;
    if (sys.Base == IP_EVENT)
    {
      if (sys.Id == IP_EVENT_GOT_IP6)
      {
        const ip_event_got_ip6_t &got = sys.Data.IpGotIp6;
        ESP_LOGI(TAG, "ESPSystemEvent: IPv6 address: " IPV6STR, IPV62STR(got.ip6_info.ip));
      }
      else if (sys.Id == IP_EVENT_STA_GOT_IP)
      {
        const ip_event_got_ip_t &got = sys.Data.IpGotIp;
        ESP_LOGI(TAG, "ESPSystemEvent: IPv4 address: " IPSTR, IP2STR(&got.ip_info.ip));
      }
      else
      {
        ESP_LOGW(TAG, "ESPSystemEvent: Unhandled IP event %s (%ld)", ENUM_CAST_NAME(ip_event_t, sys.Id), sys.Id);
      }
    }
    else if (sys.Base == WIFI_EVENT)
    {
      ESP_LOGD(TAG, "ESPSystemEvent: Wi-Fi event %s (%ld)", ENUM_CAST_NAME(wifi_event_t, sys.Id), sys.Id);
    }
    else
    {
      ESP_LOGW(TAG, "ESPSystemEvent: Unhandled event: base=%s id=%ld", sys.Base, sys.Id);
    }
    break;
  }
  case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
    ESP_LOGI(TAG, "Wi-Fi connectivity change: %s", ENUM_CAST_NAME(chip::DeviceLayer::ConnectivityChange, event->WiFiConnectivityChange.Result));
    break;
  case chip::DeviceLayer::DeviceEventType::kInternetConnectivityChange:
  {
    chip::DeviceLayer::ConnectivityChange Ipv4Change = event->InternetConnectivityChange.IPv4;
    chip::DeviceLayer::ConnectivityChange Ipv6Change = event->InternetConnectivityChange.IPv6;
    ESP_LOGD(TAG, "Internet connectivity change event received, IPv4 status: %s", ENUM_NAME(Ipv4Change));
    ESP_LOGD(TAG, "Internet connectivity change event received, IPv6 status: %s", ENUM_NAME(Ipv6Change));
    break;
  }
  case chip::DeviceLayer::DeviceEventType::kDnssdInitialized:
    ESP_LOGI(TAG, "DNS-SD initialized");
    // We can perform additional actions here if needed after DNS-SD
    // initialization, such as starting service discovery or advertising.
#ifdef CONFIG_GDO_ENABLE_SYSLOG
    start_syslog_discovery();
#endif // CONFIG_GDO_ENABLE_SYSLOG
    break;
  case chip::DeviceLayer::DeviceEventType::kServerReady:
    // Check if we have valid real time before setting the clock.
    // Time is retrieved either from SNTP server or the Matter Commissioning
    // process.
    if (time(0) > 1767225600) // January 1st 2026
      clockSet = time(0);
    ESP_LOGI(TAG, "Server is ready at %s", timeString());
    break;
  case chip::DeviceLayer::DeviceEventType::kServiceConnectivityChange:
    ESP_LOGI(TAG, "Service connectivity change event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
    ESP_LOGI(TAG, "Thread connectivity change event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kTimeSyncChange:
    if (event->TimeSyncChange.IsTimeSynchronized)
    {
      clockSet = time(0);
      ESP_LOGI(TAG, "Clock set to current time: %s", timeString());
    }
    else
    {
      ESP_LOGI(TAG, "Time synchronization change: not synchronized");
    }
    break;
  case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
    ESP_LOGI(TAG, "Secure session established");
    break;
  case chip::DeviceLayer::DeviceEventType::kFactoryReset:
    ESP_LOGI(TAG, "Factory reset event received");
    // Erase all NVS storage
    erase_nvs_data();
    // Shutdown GDO communications
    app_driver_gdo_comms_shutdown();
    break;
  case chip::DeviceLayer::DeviceEventType::kServiceProvisioningChange:
    ESP_LOGI(TAG, "Service provisioning change event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
    ESP_LOGI(TAG, "Thread state change event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kOperationalNetworkEnabled:
    ESP_LOGI(TAG, "Operational network enabled event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished:
    ESP_LOGI(TAG, "CHIPoBLE connection established event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionClosed:
    ESP_LOGI(TAG, "CHIPoBLE connection closed event received");
    break;
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
    ESP_LOGI(TAG, "CHIPoBLE advertising change event received");
    break;
  default:
    ESP_LOGW(TAG, "Unknown callback value: 0x%04X", event->Type);
    break;
  }
}

/****************************************************************************
 * Calibration task for the garage door opener.
 * This task handles the automatic calibration of the garage door opener.
 * It checks the current door status and performs the necessary movements
 * to calibrate the open and close durations.
 */
static void matter_calibrate_task(void *arg)
{
  enum class CalState {
    kStart,       // Determine initial door state
    kInitOpening, // Door position unknown, moving to fully open
    kOpen1,       // Door started closed: open first...
    kClose2,      // ...then close
    kClose1,      // Door started open (or was initialized open): close first...
    kOpen2,       // ...then open
    kComplete,
    kFailed
  };
  constexpr TickType_t kMoveTimeout = pdMS_TO_TICKS(120 * 1000);

  // Issue a command, then block until a door state notification arrives.
  // Returns true if the door ended in the expected state.
  enum class Move { to_open, to_closed };
  auto move_door = [](Move move) -> bool
  {
    const gdo_door_state_t expected = (move == Move::to_open) ? GDO_DOOR_STATE_OPEN : GDO_DOOR_STATE_CLOSED;
    ulTaskNotifyTake(pdTRUE, 0); // discard any stale notification
    if (move == Move::to_open)
      gdo_door_open();
    else
      gdo_door_close();
    if (ulTaskNotifyTake(pdTRUE, kMoveTimeout) == 0)
    {
      ESP_LOGE(TAG, "Calibration timed out waiting for door to be %s", ENUM_NAME(expected));
      return false;
    }
    if (gdo_status.door != expected)
    {
      ESP_LOGE(TAG, "Calibration expected door %s but got %s", ENUM_NAME(expected), ENUM_NAME(gdo_status.door));
      return false;
    }
    return true;
  };

  calibrating = true;
  CalState state = CalState::kStart;

  while (state != CalState::kComplete && state != CalState::kFailed)
  {
    switch (state)
    {
    case CalState::kStart:
      if (gdo_status.door == GDO_DOOR_STATE_CLOSED)
      {
        ESP_LOGI(TAG, "Door is fully closed, calibrating open then close");
        state = CalState::kOpen1;
      }
      else if (gdo_status.door == GDO_DOOR_STATE_OPEN)
      {
        ESP_LOGI(TAG, "Door is fully open, calibrating close then open");
        state = CalState::kClose1;
      }
      else
      {
        ESP_LOGI(TAG, "Door position unknown, moving to fully open first");
        state = CalState::kInitOpening;
      }
      break;
    case CalState::kInitOpening:
      state = move_door(Move::to_open) ? CalState::kClose1 : CalState::kFailed;
      break;
    case CalState::kOpen1:
      state = move_door(Move::to_open) ? CalState::kClose2 : CalState::kFailed;
      break;
    case CalState::kClose2:
      state = move_door(Move::to_closed) ? CalState::kComplete : CalState::kFailed;
      break;
    case CalState::kClose1:
      state = move_door(Move::to_closed) ? CalState::kOpen2 : CalState::kFailed;
      break;
    case CalState::kOpen2:
      state = move_door(Move::to_open) ? CalState::kComplete : CalState::kFailed;
      break;
    default:
      state = CalState::kFailed;
      break;
    }
  }

  if (state == CalState::kComplete)
    ESP_LOGI(TAG, "Calibration complete");
  else
    ESP_LOGE(TAG, "Calibration failed");

  calibrating = false;
  matter_notify_door_state(gdo_status.door);

  vTaskDelete(NULL);
}

/****************************************************************************
 * Matter closure cluster delegate...
 *
 */
namespace chip {
namespace app {
namespace Clusters {

using MatterStatus = Protocols::InteractionModel::Status;
using MatterElapsedS = ElapsedS;

class ClosureDelegate : public ClosureControlClusterDelegate, public ClosureDimensionClusterDelegate {
private:
  static constexpr char *TAG = "app_matter_closure";

public:
  MatterStatus HandleStopCommand() override
  {
    ESP_LOGI(TAG, "HandleStopCommand called");
    gdo_door_stop();
    return MatterStatus::Success;
  }

  MatterStatus HandleMoveToCommand(const Optional<TargetPositionEnum> &position, const Optional<bool> &latch, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    if (position.HasValue())
    {
      switch (position.Value())
      {
      case TargetPositionEnum::kMoveToFullyClosed:
        ESP_LOGI(TAG, "Target position: kMoveToFullyClosed");
        gdo_door_close();
        break;
      case TargetPositionEnum::kMoveToFullyOpen:
        ESP_LOGI(TAG, "Target position: kMoveToFullyOpen");
        gdo_door_open();
        break;
      case TargetPositionEnum::kMoveToPedestrianPosition:
        ESP_LOGW(TAG, "TODO: Unsupported Target position: kMoveToPedestrianPosition");
        break;
      case TargetPositionEnum::kMoveToVentilationPosition:
        ESP_LOGW(TAG, "TODO: Unsupported Target position: kMoveToVentilationPosition");
        break;
      case TargetPositionEnum::kMoveToSignaturePosition:
        ESP_LOGW(TAG, "TODO: Target position: kMoveToSignaturePosition");
        break;
      default:
        ESP_LOGW(TAG, "Unknown target position");
        break;
      }
    }
    // We should set translation direction to upward or downward
    // need to handle open/close/signature positions
    return MatterStatus::Success;
  }

  MatterStatus HandleCalibrateCommand() override { return Calibrate(false); }

  MatterStatus Calibrate(bool automatic)
  {
    // Record the pre-calibration open and close durations, using the other if one is not available.
    // Multiply by 2 to account for a full open and close cycle plus some wiggle room
    expected_calibration_ms = ((gdo_status.open_ms ? gdo_status.open_ms : gdo_status.close_ms) + (gdo_status.close_ms ? gdo_status.close_ms : gdo_status.open_ms)) * 2;
    expected_calibration_ms = automatic ? expected_calibration_ms ? expected_calibration_ms : DEFAULT_EXPECTED_CALIBRATION_MS : 0;
    // A simple calibration just resets the open and close durations and history.
    // The next time the door moves, it will record new durations.
    ESP_LOGI(TAG, "Starting calibration, expected duration %u seconds", (expected_calibration_ms + 500) / 1000);
    gdo_set_open_duration(0);
    gdo_set_close_duration(0);
    write_nvs_int(nvram_open_history, (int32_t)0);
    write_nvs_int(nvram_close_history, (int32_t)0);
    if (automatic)
    {
      if (!IsReadyToMove())
      {
        ESP_LOGE(TAG, "Door is not ready to move for automatic calibration");
        return MatterStatus::Failure;
      }
#if defined(CONFIG_FREERTOS_UNICORE) || defined(NO_CORE_PINNING)
      if (xTaskCreate(matter_calibrate_task, "matter_calibrate_task", 4096, NULL, 14, &matter_calibrate_task_handle) != pdPASS)
#else
      if (xTaskCreatePinnedToCore(matter_calibrate_task, "matter_calibrate_task", 4096, NULL, 14, &matter_calibrate_task_handle, 1) != pdPASS)
#endif
      {
        ESP_LOGE(TAG, "Failed to start calibration task");
        return MatterStatus::Failure;
      }
    }
    return MatterStatus::Success;
  }

  bool IsReadyToMove() override
  {
    gdo_get_status(&gdo_status);
    // Door must not be obstructed and must be in OPEN/CLOSED/STOPPED state
    if (gdo_status.obstruction != GDO_OBSTRUCTION_STATE_CLEAR)
      return false;
    if (gdo_status.door == GDO_DOOR_STATE_OPEN)
      return true;
    if (gdo_status.door == GDO_DOOR_STATE_CLOSED)
      return true;
    if (gdo_status.door == GDO_DOOR_STATE_STOPPED)
      return true;

    return false;
  }

  MatterElapsedS GetCalibrationCountdownTime() override
  {
    // We don't know exactly how long the calibration will take, so we estimate it as twice
    // the sum of previous recorded open and close durations (if known, else will be zero)
    ESP_LOGD(TAG, "GetCalibrationCountdownTime called, using %u ms", expected_calibration_ms);
    return MatterElapsedS((expected_calibration_ms + 500) / 1000);
  }

  MatterElapsedS GetMovingCountdownTime() override
  {
    Nullable<GenericOverallTargetState> state = control_cluster->GetOverallTargetState();
    TargetPositionEnum position = (!state.IsNull()) ? state.Value().position.Value().Value() : TargetPositionEnum::kUnknownEnumValue;
    if (position == TargetPositionEnum::kMoveToFullyOpen)
    {
      ESP_LOGD(TAG, "GetMovingCountdownTime called, expected open duration %u ms", gdo_status.open_ms);
      return MatterElapsedS((gdo_status.open_ms + 500) / 1000);
    }
    else if (position == TargetPositionEnum::kMoveToFullyClosed)
    {
      ESP_LOGD(TAG, "GetMovingCountdownTime called, expected close duration %u ms", gdo_status.close_ms);
      return MatterElapsedS((gdo_status.close_ms + 500) / 1000);
    }
    else
    {
      ESP_LOGW(TAG, "GetMovingCountdownTime called but target position invalid");
      return MatterElapsedS(0);
    }
  }

  MatterElapsedS GetWaitingForMotionCountdownTime() override
  {
    // If we have a delay before starting to open or close the door then we should report the length of that delay here.
    // Examples would be a timed delay (with beeping / light flashing)before the door starts moving after receiving a command.
    // TODO: Currently, we don't have such information, so we return 0.
    ESP_LOGD(TAG, "GetWaitingForMotionCountdownTime called");
    return MatterElapsedS(0);
  }

  MatterStatus HandleSetTarget(const Optional<Percent100ths> &position, const Optional<bool> &latch, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    if (position.HasValue())
    {
      ESP_LOGW(TAG, "TODO: HandleSetTarget called with value %d", position.Value());
    }
    return MatterStatus::Success;
  }

  MatterStatus HandleStep(const StepDirectionEnum &direction, const uint16_t &numberOfSteps, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    ESP_LOGW(TAG, "TODO: HandleStep called with direction %s", ENUM_NAME(direction));
    return MatterStatus::Success;
  }
};

} // namespace Clusters
} // namespace app
} // namespace chip

static ClosureDelegate closure_delegate;

static esp_err_t matter_start_calibration(bool automatic) { return closure_delegate.Calibrate(automatic) == chip::Protocols::InteractionModel::Status::Success ? ESP_OK : ESP_FAIL; }

/****************************************************************************
 * Matter ModeSelect supported nodes manager...
 *
 */
namespace chip {
namespace app {
namespace Clusters {
namespace ModeSelect {

class GdoSupportedModesManager : public SupportedModesManager {
private:
  using ModeOption = Structs::ModeOptionStruct::Type;

  static const ModeOption *options(size_t &count)
  {
    static const ModeOption mode_options[] = {
        {CharSpan::fromCharString("Normal"), (uint8_t)GdoMode::Normal, {}},
        {CharSpan::fromCharString("Reboot"), (uint8_t)GdoMode::Reboot, {}},
        {CharSpan::fromCharString("Calibrate"), (uint8_t)GdoMode::Calibrate, {}},
        {CharSpan::fromCharString("Calibrate Auto"), (uint8_t)GdoMode::CalibrateAuto, {}},
    };
    count = sizeof(mode_options) / sizeof(mode_options[0]); // compile-time constant
    return mode_options;
  }

public:
  static size_t optionCount()
  {
    size_t count = 0;
    options(count);
    return count;
  }

  ModeOptionsProvider getModeOptionsProvider(EndpointId endpoint_id) const override
  {
    if (endpoint_id != mode_select_endpoint_id)
      return {};

    size_t count = 0;
    const ModeOption *mode_options = options(count);
    return {mode_options, mode_options + count};
  }

  Protocols::InteractionModel::Status getModeOptionByMode(EndpointId endpoint_id, uint8_t mode, const ModeOption **data) const override
  {
    ModeOptionsProvider mode_options = getModeOptionsProvider(endpoint_id);
    for (const ModeOption *option = mode_options.begin(); option != mode_options.end(); ++option)
    {
      if (option->mode == mode)
      {
        *data = option;
        return Protocols::InteractionModel::Status::Success;
      }
    }

    *data = nullptr;
    return Protocols::InteractionModel::Status::InvalidCommand;
  }
};

} // namespace ModeSelect
} // namespace Clusters
} // namespace app
} // namespace chip

static GdoSupportedModesManager mode_select_manager;

/****************************************************************************
 * Initialization
 *
 */
extern void matter_comms_init()
{
  if (matter_comms_init_done)
    return;

  esp_err_t err = ESP_OK;

  ESP_LOGI(TAG, "Initializing Matter communications");

  /* Create a Matter node and add the mandatory Root Node device type on
   * endpoint 0 */
  node::config_t node_config;
  strcpy(node_config.root_node.basic_information.node_label, "GDO Prototype");
  // Create the mandatory Root Node device type on endpoint 0.
  node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
  ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));
  endpoint_t *root_node_ep = endpoint::get_first(node);

  // Update the Basic Information cluster on endpoint 0.
  cluster_t *basic_info_cluster = cluster::get(root_node_ep, chip::app::Clusters::BasicInformation::Id);

  char product_url[] = "https://github.com/dkerr64/matter-gdo";
  char product_label[] = "GDO Prototype";

  cluster::basic_information::attribute::create_product_url(basic_info_cluster, product_url, strlen(product_url));
  cluster::basic_information::attribute::create_product_label(basic_info_cluster, product_label, strlen(product_label));

  /*
  // We are setting product URL and product label, but for some reason they are not getting set (clients get empty strings)
  // This was code attempting to debug the issue, but it remains unresolved.
  cluster::basic_information::attribute::create_product_url(basic_info_cluster, NULL, 0);
  cluster::basic_information::attribute::create_product_label(basic_info_cluster, NULL, 0);
  attribute_t *product_url_attr = attribute::get(basic_info_cluster, chip::app::Clusters::BasicInformation::Attributes::ProductURL::Id);
  attribute_t *product_label_attr = attribute::get(basic_info_cluster, chip::app::Clusters::BasicInformation::Attributes::ProductLabel::Id);
  esp_matter_attr_val_t url_val = esp_matter_char_str((char *)product_url, strlen(product_url));
  esp_matter_attr_val_t label_val = esp_matter_char_str((char *)product_label, strlen(product_label));
  attribute::set_val(product_url_attr, &url_val);
  attribute::set_val(product_label_attr, &label_val);
  */

  // We have similar problem with the serial number and manufacturing date attributes not being set correctly.
  uint8_t mac[6];
  char serial_number[32];
  esp_efuse_mac_get_default(mac);
  snprintf(serial_number, sizeof(serial_number), "GDO-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cluster::basic_information::attribute::create_serial_number(basic_info_cluster, serial_number, strlen(serial_number));
  ESP_LOGI(TAG, "Serial Number: %s", serial_number);
  char manufacturing_date[] = "20260925";
  cluster::basic_information::attribute::create_manufacturing_date(basic_info_cluster, manufacturing_date, strlen(manufacturing_date));
  ESP_LOGI(TAG, "Basic Information Cluster initialized!");

  cluster::time_synchronization::config_t time_sync_cfg;
  // Create the mandatory cluster delegate required by the Matter specification
  static chip::app::Clusters::TimeSynchronization::DefaultTimeSyncDelegate time_sync_delegate;
  time_sync_cfg.delegate = &time_sync_delegate;
  // Register the cluster on Endpoint 0 as a SERVER
  cluster_t *time_sync_cluster = cluster::time_synchronization::create(root_node_ep, &time_sync_cfg, CLUSTER_FLAG_SERVER);
  // Add the Time Zone feature to allow regional time translations
  cluster::time_synchronization::feature::time_zone::config_t tz_cfg;
  cluster::time_synchronization::feature::time_zone::add(time_sync_cluster, &tz_cfg);
  ESP_LOGI(TAG, "Time Synchronization Cluster initialized!");

  // ENDPOINT CREATION.
  // The order we create these determine the endpoint IDs assigned to each cluster.  Don't change order as
  // it may affect clients (in particular if anyone created scripts with chip-tool)
  //

  // #1 Start with mode select endpoint
  {
    mode_select::config_t mode_select_config = {};
    mode_select_config.mode_select.delegate = &mode_select_manager;
    endpoint_t *mode_select_endpoint = mode_select::create(node, &mode_select_config, ENDPOINT_FLAG_DESTROYABLE, NULL);
    mode_select_endpoint_id = endpoint::get_id(mode_select_endpoint);
    ESP_LOGI(TAG, "Mode Select cluster created with endpoint_id %d", mode_select_endpoint_id);
  }

  // #2 Create a Closure Control endpoint
  {
    closure::config_t closure_config;
    closure_config.closure_control.feature_flags = cluster::closure_control::feature::positioning::get_id() | cluster::closure_control::feature::calibration::get_id();
    endpoint_t *closure_endpoint = closure::create(node, &closure_config, ENDPOINT_FLAG_NONE, nullptr);
    closure_endpoint_id = endpoint::get_id(closure_endpoint);
    ClosureControl::MatterClosureControlSetDelegate(closure_endpoint_id, closure_delegate);
    ESP_LOGI(TAG, "Closure created with endpoint_id %d", closure_endpoint_id);
  }

  // #3 Create a Closure Dimension endpoint
  {
    closure_panel::config_t closure_dimension;
    closure_dimension.closure_dimension.feature_flags = cluster::closure_dimension::feature::positioning::get_id() | cluster::closure_dimension::feature::translation::get_id();
    endpoint_t *dimension_endpoint = closure_panel::create(node, &closure_dimension, ENDPOINT_FLAG_NONE, nullptr);
    dimension_endpoint_id = endpoint::get_id(dimension_endpoint);
    // 2500 represents 25%, this means we will set door to 1/4 steps between open
    // and closed (after calibration)
    esp_matter_attr_val_t resolution = esp_matter_uint16(2500);
    // A request for a single step will move the door 25%
    esp_matter_attr_val_t step_value = esp_matter_uint16(2500);
    attribute::set_val(dimension_endpoint_id, ClosureDimension::Id, ClosureDimension::Attributes::Resolution::Id, &resolution);
    attribute::set_val(dimension_endpoint_id, ClosureDimension::Id, ClosureDimension::Attributes::StepValue::Id, &step_value);
    ClosureDimension::MatterClosureDimensionSetDelegate(dimension_endpoint_id, closure_delegate);
    ESP_LOGI(TAG, "Closure dimension created with endpoint_id %d", dimension_endpoint_id);
  }

  // #4 Create a Light endpoint
  {
    on_off_light::config_t light_config;
    light_config.on_off.on_off = false;
    endpoint_t *light_endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    light_endpoint_id = endpoint::get_id(light_endpoint);
    ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);
  }

  // #5 Create a Door Lock endpoint, this actually enables/disables the GDO remote
  {
    door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = chip::to_underlying(DoorLock::DlLockState::kUnlocked);
    lock_config.door_lock.lock_type = chip::to_underlying(DoorLock::DlLockType::kOther);
    // lock_config.door_lock.delegate = &chip::app::Clusters::lock_delegate;
    endpoint_t *lock_endpoint = door_lock::create(node, &lock_config, ENDPOINT_FLAG_NONE, nullptr);
    cluster::door_lock::attribute::create_auto_relock_time(cluster::get(lock_endpoint, DoorLock::Id), 0);
    lock_endpoint_id = endpoint::get_id(lock_endpoint);
    ESP_LOGI(TAG, "Lock created with endpoint_id %d", lock_endpoint_id);
  }

  // #6 Create an occupancy sensor endpoint (which covers motion)
  {
    occupancy_sensor::config_t occupancy_config;
    // Assuming passive infrared for now.  On Sec+1.0 doors we may not have that, it may be based on
    // an object crossing the obstruction sensor (either kOccupancyEvent or kOther?)
    occupancy_config.occupancy_sensing.feature_flags = chip::to_underlying(OccupancySensing::Feature::kPassiveInfrared);
    // Keep deprecated sensor type bitmap set too, for clients that don't yet read the Feature flags.
    occupancy_config.occupancy_sensing.occupancy_sensor_type = chip::to_underlying(OccupancySensing::OccupancySensorTypeEnum::kPir);
    occupancy_config.occupancy_sensing.occupancy_sensor_type_bitmap = chip::to_underlying(OccupancySensing::OccupancySensorTypeBitmap::kPir);
    endpoint_t *occupancy_endpoint = occupancy_sensor::create(node, &occupancy_config, ENDPOINT_FLAG_NONE, nullptr);
    occupancy_endpoint_id = endpoint::get_id(occupancy_endpoint);
    cluster_t *cluster = cluster::get(occupancy_endpoint, OccupancySensing::Id);

    LOG_ON_FALSE(cluster::occupancy_sensing::attribute::create_hold_time(cluster, 3), "Failed to create hold time attribute");
    // We should pass in a TLV-encoded structure for the hold time limits instead of nullptr, 0, 0. But under the covers that is
    // not supported yet, so we pass nullptr, 0, 0 for now.  The ESP-Matter SDK sets Min/Max/Defaults to 1, 300 and 30 seconds respectively.
    // We can change these after the matter server has started and the cluster instance is available (see below)
    LOG_ON_FALSE(cluster::occupancy_sensing::attribute::create_hold_time_limits(cluster, nullptr, 0, 0), "Failed to create hold time limits attribute");
    ESP_LOGI(TAG, "Occupancy sensor created with endpoint_id %d", occupancy_endpoint_id);
  }

  /* Matter start */
  err = esp_matter::start(matter_event_cb);
  ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

  // After starting Matter, we can read the initial (non-volatile) values of attributes and update the driver accordingly.
  esp_matter_attr_val_t val;
  attribute::get_val(occupancy_endpoint_id, OccupancySensing::Id, OccupancySensing::Attributes::HoldTime::Id, &val);
  app_driver_attribute_update(occupancy_endpoint_id, OccupancySensing::Id, OccupancySensing::Attributes::HoldTime::Id, &val);

  // Now find and save the clusters created for each endpoint
  control_cluster = ClosureControl::GetClusterInstance(closure_endpoint_id);
  // dimension_cluster = ClosureDimension::GetClusterInstance(dimension_endpoint_id);
  auto &registry = esp_matter::data_model::provider::get_instance().registry();
  dimension_cluster = static_cast<ClosureDimensionCluster *>(registry.Get(ConcreteClusterPath(dimension_endpoint_id, ClosureDimension::Id)));
  occupancy_cluster = OccupancySensing::FindClusterOnEndpoint(occupancy_endpoint_id);

  {
    // The occupancy sensing cluster instance exists only once the endpoint is enabled by start(), the ESP-Matter SDK
    // sets default values for the hold time limits min/max/default to 1, 300 and 30 seconds respectively.
    chip::DeviceLayer::StackLock lock;
    OccupancySensing::Structs::HoldTimeLimitsStruct::Type hold_time_limits;
    hold_time_limits.holdTimeMin = 1;
    hold_time_limits.holdTimeMax = 10;
    hold_time_limits.holdTimeDefault = 3;
    occupancy_cluster->SetHoldTimeLimits(hold_time_limits);
  }

  ESP_LOGI(TAG, "Matter comms initialization complete");
  matter_comms_init_done = true;

#if CONFIG_ENABLE_CHIP_SHELL
  esp_matter::console::diagnostics_register_commands();
  esp_matter::console::wifi_register_commands();
  esp_matter::console::factoryreset_register_commands();
  esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
  esp_matter::console::otcli_register_commands();
#endif
  esp_matter::console::init();
#endif
}

extern void matter_comms_shutdown()
{
  if (!matter_comms_init_done)
    return;

  ESP_LOGI(TAG, "Shutting down Matter communications");
  matter_comms_init_done = false;
}

/****************************************************************************
 * Helper functions used internally to update Matter attributes
 *
 */

static void set_main_state(MainStateEnum context)
{
  CHIP_ERROR err = CHIP_NO_ERROR;
  MainStateEnum state = control_cluster->GetMainState();
  ESP_LOGD(TAG, "Main state: %s, change to: %s", ENUM_NAME(state), ENUM_NAME(context));

  err = control_cluster->SetMainState(context);
  if (err != CHIP_NO_ERROR)
  {
    ESP_LOGE(TAG, "Failed to set MainState: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

static void set_current_state(CurrentPositionEnum context)
{
  CHIP_ERROR err = CHIP_NO_ERROR;
  Nullable<GenericOverallCurrentState> state = control_cluster->GetOverallCurrentState();
  CurrentPositionEnum position = (!state.IsNull()) ? state.Value().position.Value().Value() : CurrentPositionEnum::kUnknownEnumValue;
  ESP_LOGD(TAG, "Current position: %s, change to: %s (%u%%)", ENUM_NAME(position), ENUM_NAME(context), gdo_status.door_position / 100);

  if (context == CurrentPositionEnum::kUnknownEnumValue)
  {
    if ((err = control_cluster->SetOverallCurrentState(Nullable<GenericOverallCurrentState>(chip::NullOptional))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetCurrentState(Nullable<GenericDimensionStateStruct>(chip::NullOptional));
  }
  else
  {
    chip::Percent100ths posValue = (context == CurrentPositionEnum::kPartiallyOpened) ? gdo_status.door_position : (context == CurrentPositionEnum::kFullyOpened) ? 0 : 10000;
    GenericOverallCurrentState state(chip::Optional(MakeNullable(context)), chip::NullOptional, chip::NullOptional, chip::NullOptional);
    GenericDimensionStateStruct dimension(chip::Optional(chip::app::DataModel::MakeNullable(posValue)), chip::NullOptional, chip::NullOptional);
    if ((err = control_cluster->SetOverallCurrentState(Nullable<GenericOverallCurrentState>(state))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetCurrentState(Nullable<GenericDimensionStateStruct>(dimension));
  }

  if (err != CHIP_NO_ERROR)
  {
    ESP_LOGE(TAG, "Failed to set OverallCurrentState: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

static void set_overall_target_state(TargetPositionEnum context)
{
  CHIP_ERROR err = CHIP_NO_ERROR;
  Nullable<GenericOverallTargetState> state = control_cluster->GetOverallTargetState();
  TargetPositionEnum position = (!state.IsNull()) ? state.Value().position.Value().Value() : TargetPositionEnum::kUnknownEnumValue;
  ESP_LOGD(TAG, "Target position: %s, change to: %s", ENUM_NAME(position), ENUM_NAME(context));

  if (context == TargetPositionEnum::kUnknownEnumValue)
  {
    if ((err = control_cluster->SetOverallTargetState(Nullable<GenericOverallTargetState>(chip::NullOptional))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetTargetState(Nullable<GenericDimensionStateStruct>(chip::NullOptional));
  }
  else
  {
    GenericOverallTargetState state(chip::Optional(MakeNullable(context)), chip::NullOptional, chip::NullOptional);
    chip::Percent100ths posValue = (context == TargetPositionEnum::kMoveToFullyOpen) ? 0 : 10000;
    GenericDimensionStateStruct dimension(chip::Optional(chip::app::DataModel::MakeNullable(posValue)), chip::NullOptional, chip::NullOptional);
    if ((err = control_cluster->SetOverallTargetState(Nullable<GenericOverallTargetState>(state))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetTargetState(Nullable<GenericDimensionStateStruct>(dimension));
  }
  if (err != CHIP_NO_ERROR)
  {
    ESP_LOGE(TAG, "Failed to set OverallTargetState: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

/****************************************************************************
 * Helper functions called by our code to notify Matter of changes
 *
 */
esp_err_t matter_set_mode(uint8_t mode)
{
  if (mode >= GdoSupportedModesManager::optionCount()) // mode values are the array indices
    return ESP_ERR_INVALID_ARG;

  esp_matter_attr_val_t value = esp_matter_uint8(mode);

  return attribute::set_val(mode_select_endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &value);
}

esp_err_t matter_notify_light_state(bool light_on)
{
  if (!matter_comms_init_done)
    return ESP_OK;

  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                      [](intptr_t arg)
                                      {
                                        esp_matter_attr_val_t val;
                                        attribute::get_val(attribute::get(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id), &val);
                                        ESP_LOGD(TAG, "Current light state: %s, set to %s", val.val.b ? "ON" : "OFF", arg ? "ON" : "OFF");
                                        val.val.b = arg;
                                        // report() will not call callbacks, but will inform clients of the change.
                                        attribute::report(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
                                      },
                                      light_on));
  return ESP_OK;
}

esp_err_t matter_notify_lock_state(bool lock_on)
{
  if (!matter_comms_init_done)
    return ESP_OK;

  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                      [](intptr_t arg)
                                      {
                                        esp_matter_attr_val_t val;
                                        attribute::get_val(attribute::get(lock_endpoint_id, DoorLock::Id, DoorLock::Attributes::LockState::Id), &val);
                                        ESP_LOGD(TAG, "Current Remotes lock state: %s, set to %s", val.val.b ? "ON" : "OFF", arg ? "ON" : "OFF");
                                        val.val.b = arg;
                                        // report() will not call callbacks, but will inform clients of the change.
                                        attribute::report(lock_endpoint_id, DoorLock::Id, DoorLock::Attributes::LockState::Id, &val);
                                      },
                                      lock_on));
  return ESP_OK;
}

esp_err_t matter_notify_occupancy_state(bool occupied)
{
  if (!matter_comms_init_done)
    return ESP_OK;

  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                      [](intptr_t arg)
                                      {
                                        esp_matter_attr_val_t val = esp_matter_bitmap8(arg ? chip::to_underlying(OccupancySensing::OccupancyBitmap::kOccupied) : 0);
                                        ESP_LOGD(TAG, "Set occupancy state to %s", arg ? "OCCUPIED" : "UNOCCUPIED");
                                        // report() will not call callbacks, but will inform clients of the change.
                                        attribute::report(occupancy_endpoint_id, OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id, &val);
                                      },
                                      occupied));
  return ESP_OK;
}

esp_err_t matter_notify_door_state(gdo_door_state_t door_state)
{
  if (!matter_comms_init_done)
    return ESP_OK;

  Nullable<GenericOverallCurrentState> state = control_cluster->GetOverallCurrentState();
  CurrentPositionEnum current_position = (!state.IsNull()) ? state.Value().position.Value().Value() : CurrentPositionEnum::kUnknownEnumValue;

  switch (door_state)
  {
  case GDO_DOOR_STATE_OPEN:
    // Set MainState to stopped, OverallCurrentState to fully open, and
    // OverallTargetState to NULL.
    {
      if (calibrating && current_position != CurrentPositionEnum::kFullyOpened)
      {
        // Sometimes we get a "duplicate" open event, so only notify if we're not already fully opened.
        xTaskNotifyGive(matter_calibrate_task_handle);
      }
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(calibrating ? MainStateEnum::kCalibrating : MainStateEnum::kStopped);
                                            set_current_state(CurrentPositionEnum::kFullyOpened);
                                            set_overall_target_state(TargetPositionEnum::kUnknownEnumValue);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_CLOSED:
    // Set MainState to stopped, OverallCurrentState to fully closed, and
    // OverallTargetState to NULL.
    {
      if (calibrating && current_position != CurrentPositionEnum::kFullyClosed)
      {
        // Sometimes we get a "duplicate" closed event, so only notify if we're not already fully closed.
        xTaskNotifyGive(matter_calibrate_task_handle);
      }
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(calibrating ? MainStateEnum::kCalibrating : MainStateEnum::kStopped);
                                            set_current_state(CurrentPositionEnum::kFullyClosed);
                                            set_overall_target_state(TargetPositionEnum::kUnknownEnumValue);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_STOPPED:
    // Set MainState to stopped, OverallCurrentState to partially opened, and
    // OverallTargetState to NULL.
    {
      if (calibrating)
      {
        xTaskNotifyGive(matter_calibrate_task_handle);
      }
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(calibrating ? MainStateEnum::kCalibrating : MainStateEnum::kStopped);
                                            set_current_state(CurrentPositionEnum::kPartiallyOpened);
                                            set_overall_target_state(TargetPositionEnum::kUnknownEnumValue);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_OPENING:
    // Set MainState to moving (unless calibrating), OverallCurrentState to
    // partially opened, and OverallTargetState to move to fully open.
    {
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(calibrating ? MainStateEnum::kCalibrating : MainStateEnum::kMoving);
                                            set_current_state(CurrentPositionEnum::kPartiallyOpened);
                                            set_overall_target_state(TargetPositionEnum::kMoveToFullyOpen);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_CLOSING:
    // Set MainState to moving (unless calibrating), OverallCurrentState to
    // partially opened, and OverallTargetState to move to fully closed.
    {
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(calibrating ? MainStateEnum::kCalibrating : MainStateEnum::kMoving);
                                            set_current_state(CurrentPositionEnum::kPartiallyOpened);
                                            set_overall_target_state(TargetPositionEnum::kMoveToFullyClosed);
                                          }));
      break;
    }
  default:
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t matter_notify_obstruction_state(bool obstructed)
{
  esp_err_t err = ESP_OK;
  if (!matter_comms_init_done)
    return err;

  ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                      [](intptr_t arg)
                                      {
                                        // ClosureControlCluster *cluster = ClosureControl::GetClusterInstance(closure_endpoint_id);
                                        if (arg)
                                        {
                                          //  Obstruction detected
                                          ChipLogIfFalse(CHIP_NO_ERROR == control_cluster->AddErrorToCurrentErrorList(ClosureErrorEnum::kBlockedBySensor));
                                          set_main_state(MainStateEnum::kError);
                                        }
                                        else
                                        {
                                          control_cluster->ClearCurrentErrorList();
                                          set_main_state(MainStateEnum::kStopped);
                                        }
                                      },
                                      obstructed));

  return err;
}
