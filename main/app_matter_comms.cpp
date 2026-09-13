/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

#define CHIP_CONFIG_ENABLE_CONDITION_LOGGING 1
// C/C++ language include files...

// ESP-IDF include files...
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>

// ESP-Matter SDK include files...
#include <clusters/closure_control/integration.h>
#include <clusters/closure_dimension/integration.h>
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

#include <app/clusters/time-synchronization-server/DefaultTimeSyncDelegate.h>

// Our project include files...
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

constexpr uint16_t k_timeout_seconds = 300;

/****************************************************************************
 * Callbacks
 *
 */
// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by
// flashing an LED or light).

// required to resolve linker errors for Level Control Coupled Color Temperature Change callback (even though we don't use it)
void emberAfPluginLevelControlCoupledColorTempChangeCallback(uint16_t endpoint) {}

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

// This callback is called for every attribute update. The callback
// implementation shall handle the desired attributes and return an appropriate
// error code. If the attribute is not of your interest, please do not return an
// error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
  esp_err_t err = ESP_OK;

  ESP_LOGI(TAG,
           "Attribute update callback: type: %u, endpoint_id: %u, cluster_id: "
           "0x%04x, attribute_id: 0x%04x value type: %u, value: %u",
           type, endpoint_id, cluster_id, attribute_id, val->type, val->val.u32);
  if (type == PRE_UPDATE)
  {
    if (endpoint_id == closure_endpoint_id || endpoint_id == dimension_endpoint_id || endpoint_id == light_endpoint_id || endpoint_id == lock_endpoint_id)
    {
      // Handle closure, dimension, light, or lock endpoint specific attributes
      app_driver_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }
    else if (endpoint_id == mode_select_endpoint_id)
    {
      // Handle mode select endpoint specific attributes
      // ID = 3, Type = 8, Value = 0 or 1
      if (val->val.u8 == 1)
      {
        ESP_LOGI(TAG, "Mode select attribute triggered reboot (1 second delay)");
        schedule_reboot();
      }
    }
  }

  return err;
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
    ESP_LOGI(TAG, "Interface IP Address changed: %s", ENUM_NAME(changeType));
    break;
  }

  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    clockSet = time(0);
    ESP_LOGI(TAG, "Time synchronized: %s", timeString());
    break;

  case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
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
    ESP_LOGI(TAG, "ESP system event received");
    // event->Platform.ESPSystemEvent.Base
    break;
  case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
    ESP_LOGI(TAG, "Wi-Fi connectivity change: %s", ENUM_CAST_NAME(chip::DeviceLayer::ConnectivityChange, event->WiFiConnectivityChange.Result));
    break;
  case chip::DeviceLayer::DeviceEventType::kInternetConnectivityChange:
  {
    chip::DeviceLayer::ConnectivityChange Ipv4Change = event->InternetConnectivityChange.IPv4;
    chip::DeviceLayer::ConnectivityChange Ipv6Change = event->InternetConnectivityChange.IPv6;
    if (Ipv4Change == chip::DeviceLayer::ConnectivityChange::kConnectivity_Established)
    {
      chip::Inet::IPAddress ipAddress = event->InternetConnectivityChange.ipAddress;
      deviceIPv4Address = ipAddress.ToIPv4();
      ipAddress.ToString(deviceIPv4String, sizeof(deviceIPv4String));
      ESP_LOGI(TAG, "IPv4 connectivity established with IP address: %s", deviceIPv4String);
    }
    else
    {
      ESP_LOGI(TAG, "Internet connectivity change event received, IPv4 status: %s", ENUM_NAME(Ipv4Change));
    }
    if (Ipv6Change == chip::DeviceLayer::ConnectivityChange::kConnectivity_Established)
    {
      chip::Inet::IPAddress ipAddress = event->InternetConnectivityChange.ipAddress;
      char ipAddressStr[INET6_ADDRSTRLEN];
      ipAddress.ToString(ipAddressStr, sizeof(ipAddressStr));
      ESP_LOGI(TAG, "IPv6 connectivity established with IP address: %s", ipAddressStr);
    }
    else
    {
      ESP_LOGI(TAG, "Internet connectivity change event received, IPv6 status: %s", ENUM_NAME(Ipv6Change));
    }
    break;
  }
  case chip::DeviceLayer::DeviceEventType::kDnssdInitialized:
    ESP_LOGI(TAG, "DNS-SD initialized");

    // We can perform additional actions here if needed after DNS-SD
    // initialization, such as starting service discovery or advertising.
    start_syslog_discovery();
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
    break;
  default:
    ESP_LOGW(TAG, "Unknown callback value: 0x%04X", event->Type);
    break;
  }
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
    return MatterStatus::Success;
  }

  MatterStatus HandleMoveToCommand(const Optional<TargetPositionEnum> &position, const Optional<bool> &latch, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    if (position.HasValue())
    {
      ESP_LOGI(TAG, "HandleMoveToCommand called with target position of %s", ENUM_NAME(position.Value()));
    }
    // We should set translation direction to upward or downward
    return MatterStatus::Success;
  }

  MatterStatus HandleCalibrateCommand() override
  {
    ESP_LOGI(TAG, "HandleCalibrateCommand called");
    return MatterStatus::Success;
  }

  bool IsReadyToMove() override { return true; }

  MatterElapsedS GetCalibrationCountdownTime() override { return MatterElapsedS(0); }

  MatterElapsedS GetMovingCountdownTime() override { return MatterElapsedS(0); }

  MatterElapsedS GetWaitingForMotionCountdownTime() override { return MatterElapsedS(0); }

  MatterStatus HandleSetTarget(const Optional<Percent100ths> &position, const Optional<bool> &latch, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    if (position.HasValue())
    {
      ESP_LOGI(TAG, "HandleSetTarget called with value %d", position.Value());
    }
    return MatterStatus::Success;
  }

  MatterStatus HandleStep(const StepDirectionEnum &direction, const uint16_t &numberOfSteps, const Optional<ThreeLevelAutoEnum> &speed) override
  {
    ESP_LOGI(TAG, "HandleStep called with direction %s", ENUM_NAME(direction));
    return MatterStatus::Success;
  }
};

} // namespace Clusters
} // namespace app
} // namespace chip

static ClosureDelegate closure_delegate;

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

  static const ModeOption *options()
  {
    static const ModeOption mode_options[] = {
        {CharSpan::fromCharString("Normal"), 0, {}},
        {CharSpan::fromCharString("Reboot"), 1, {}},
    };
    return mode_options;
  }

public:
  ModeOptionsProvider getModeOptionsProvider(EndpointId endpoint_id) const override
  {
    if (endpoint_id != mode_select_endpoint_id)
      return {};

    const ModeOption *mode_options = options();
    return {mode_options, mode_options + 2};
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
  mode_select::config_t mode_select_config = {};
  mode_select_config.mode_select.delegate = &mode_select_manager;
  endpoint_t *mode_select_endpoint = mode_select::create(node, &mode_select_config, ENDPOINT_FLAG_DESTROYABLE, NULL);
  ABORT_APP_ON_FAILURE(mode_select_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create mode select endpoint"));
  mode_select_endpoint_id = endpoint::get_id(mode_select_endpoint);
  ESP_LOGI(TAG, "Mode Select cluster created with endpoint_id %d", mode_select_endpoint_id);

  // #2 Create a Closure Control endpoint
  closure::config_t closure_config;
  closure_config.closure_control.feature_flags = cluster::closure_control::feature::positioning::get_id() | cluster::closure_control::feature::calibration::get_id();
  endpoint_t *closure_endpoint = closure::create(node, &closure_config, ENDPOINT_FLAG_NONE, nullptr);
  ABORT_APP_ON_FAILURE(closure_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create closure endpoint"));
  closure_endpoint_id = endpoint::get_id(closure_endpoint);
  ClosureControl::MatterClosureControlSetDelegate(closure_endpoint_id, closure_delegate);
  ESP_LOGI(TAG, "Closure created with endpoint_id %d", closure_endpoint_id);

  // #3 Create a Closure Dimension endpoint
  closure_panel::config_t closure_dimension;
  closure_dimension.closure_dimension.feature_flags = cluster::closure_dimension::feature::positioning::get_id() | cluster::closure_dimension::feature::translation::get_id();
  endpoint_t *dimension_endpoint = closure_panel::create(node, &closure_dimension, ENDPOINT_FLAG_NONE, nullptr);
  ABORT_APP_ON_FAILURE(dimension_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create closure dimension endpoint"));
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

  // #4 Create a Light endpoint
  on_off_light::config_t light_config;
  light_config.on_off.on_off = false;
  endpoint_t *light_endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
  ABORT_APP_ON_FAILURE(light_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create light endpoint"));
  light_endpoint_id = endpoint::get_id(light_endpoint);
  ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

  // #5 Create a Door Lock endpoint, this actually enables/disables the GDO remote
  door_lock::config_t lock_config;
  lock_config.door_lock.lock_state = chip::to_underlying(DoorLock::DlLockState::kUnlocked);
  lock_config.door_lock.lock_type = chip::to_underlying(DoorLock::DlLockType::kOther);
  // lock_config.door_lock.delegate = &chip::app::Clusters::lock_delegate;
  endpoint_t *lock_endpoint = door_lock::create(node, &lock_config, ENDPOINT_FLAG_NONE, nullptr);
  ABORT_APP_ON_FAILURE(lock_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create lock endpoint"));
  cluster::door_lock::attribute::create_auto_relock_time(cluster::get(lock_endpoint, DoorLock::Id), 0);
  lock_endpoint_id = endpoint::get_id(lock_endpoint);
  ESP_LOGI(TAG, "Lock created with endpoint_id %d", lock_endpoint_id);

// #6 Create an occupancy sensor endpoint (which covers motion)
  occupancy_sensor::config_t occupancy_config;
  occupancy_config.occupancy_sensing.feature_flags = chip::to_underlying(OccupancySensing::Feature::kPassiveInfrared);
  endpoint_t *occupancy_endpoint = occupancy_sensor::create(node, &occupancy_config, ENDPOINT_FLAG_NONE, nullptr);
  ABORT_APP_ON_FAILURE(occupancy_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create occupancy sensor endpoint"));
  occupancy_endpoint_id = endpoint::get_id(occupancy_endpoint);
  ESP_LOGI(TAG, "Occupancy sensor created with endpoint_id %d", occupancy_endpoint_id);

  /* Matter start */
  err = esp_matter::start(matter_event_cb);
  ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

  matter_set_mode(0); // Set initial mode (reset if remote reboot triggered)

  ESP_LOGI(TAG, "Matter comms initialization complete");
  matter_comms_init_done = true;

  // Initialize a door state
  // err = matter_notify_door_state(GDO_DOOR_STATE_CLOSED);

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
  auto &registry = esp_matter::data_model::provider::get_instance().registry();
  // ServerClusterInterface *interface =
  // registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id));
  ClosureControlCluster *cluster = static_cast<ClosureControlCluster *>(registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id)));
  MainStateEnum state = cluster->GetMainState();
  ESP_LOGI(TAG, "Current main state: %s, change to: %s", ENUM_NAME(state), ENUM_NAME(context));

  err = cluster->SetMainState(context);
  if (err != CHIP_NO_ERROR)
  {
    ESP_LOGE(TAG, "Failed to set OverallCurrentState: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

static void set_current_state(CurrentPositionEnum context)
{
  CHIP_ERROR err = CHIP_NO_ERROR;
  auto &registry = esp_matter::data_model::provider::get_instance().registry();
  // ServerClusterInterface *interface =
  // registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id));
  ClosureControlCluster *cluster = static_cast<ClosureControlCluster *>(registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id)));
  ClosureDimensionCluster *dimension_cluster = static_cast<ClosureDimensionCluster *>(registry.Get(ConcreteClusterPath(dimension_endpoint_id, ClosureDimension::Id)));
  Nullable<GenericOverallCurrentState> state = cluster->GetOverallCurrentState();
  if (!state.IsNull())
  {
    CurrentPositionEnum position = state.Value().position.Value().Value();
    ESP_LOGI(TAG, "Current position: %s, change to: %s", ENUM_NAME(position), ENUM_NAME(context));
  }
  else
  {
    ESP_LOGI(TAG, "Current position: Null, change to: %s", ENUM_NAME(context));
  }

  if (context == CurrentPositionEnum::kUnknownEnumValue)
  {
    if ((err = cluster->SetOverallCurrentState(Nullable<GenericOverallCurrentState>(chip::NullOptional))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetCurrentState(Nullable<GenericDimensionStateStruct>(chip::NullOptional));
  }
  else
  {
    chip::Percent100ths posValue = (context == CurrentPositionEnum::kFullyOpened) ? 0 : 10000;
    GenericOverallCurrentState state(chip::Optional(MakeNullable(context)), chip::NullOptional, chip::NullOptional, chip::NullOptional);
    GenericDimensionStateStruct dimension(chip::Optional(chip::app::DataModel::MakeNullable(posValue)), chip::NullOptional, chip::NullOptional);
    if ((err = cluster->SetOverallCurrentState(Nullable<GenericOverallCurrentState>(state))) == CHIP_NO_ERROR)
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
  auto &registry = esp_matter::data_model::provider::get_instance().registry();
  // ServerClusterInterface *interface =
  // registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id));
  ClosureControlCluster *cluster = static_cast<ClosureControlCluster *>(registry.Get(ConcreteClusterPath(closure_endpoint_id, ClosureControl::Id)));
  ClosureDimensionCluster *dimension_cluster = static_cast<ClosureDimensionCluster *>(registry.Get(ConcreteClusterPath(dimension_endpoint_id, ClosureDimension::Id)));
  Nullable<GenericOverallTargetState> state = cluster->GetOverallTargetState();
  if (!state.IsNull())
  {
    TargetPositionEnum position = state.Value().position.Value().Value();
    ESP_LOGI(TAG, "Current target position: %s, change to: %s", ENUM_NAME(position), ENUM_NAME(context));
  }
  else
  {
    ESP_LOGI(TAG, "Current target position: Null, change to: %s", ENUM_NAME(context));
  }

  if (context == TargetPositionEnum::kUnknownEnumValue)
  {
    if ((err = cluster->SetOverallTargetState(Nullable<GenericOverallTargetState>(chip::NullOptional))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetTargetState(Nullable<GenericDimensionStateStruct>(chip::NullOptional));
  }
  else
  {
    GenericOverallTargetState state(chip::Optional(MakeNullable(context)), chip::NullOptional, chip::NullOptional);
    chip::Percent100ths posValue = (context == TargetPositionEnum::kMoveToFullyOpen) ? 0 : 10000;
    GenericDimensionStateStruct dimension(chip::Optional(chip::app::DataModel::MakeNullable(posValue)), chip::NullOptional, chip::NullOptional);
    if ((err = cluster->SetOverallTargetState(Nullable<GenericOverallTargetState>(state))) == CHIP_NO_ERROR)
      err = dimension_cluster->SetTargetState(Nullable<GenericDimensionStateStruct>(dimension));
  }
  if (err != CHIP_NO_ERROR)
  {
    ESP_LOGE(TAG, "Failed to set OverallCurrentState: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

/****************************************************************************
 * Helper functions called by our code to notify Matter of changes
 *
 */
esp_err_t matter_set_mode(uint8_t mode)
{
  if (mode > 1)
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
                                        ESP_LOGI(TAG, "Current light state: %s, set to %s", val.val.b ? "ON" : "OFF", arg ? "ON" : "OFF");
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
                                        ESP_LOGI(TAG, "Current Remotes lock state: %s, set to %s", val.val.b ? "ON" : "OFF", arg ? "ON" : "OFF");
                                        val.val.b = arg;
                                        // report() will not call callbacks, but will inform clients of the change.
                                        attribute::report(lock_endpoint_id, DoorLock::Id, DoorLock::Attributes::LockState::Id, &val);
                                      },
                                      lock_on));
  return ESP_OK;
}

esp_err_t matter_notify_door_state(gdo_door_state_t door_state)
{
  if (!matter_comms_init_done)
    return ESP_OK;

  // Attributes are set according to rules in section 5.4 Closure Control
  // Cluster of the Matter Application Cluster Specifications
  esp_matter_attr_val_t main_state_val;
  esp_matter_attr_val_t current_state_val;

  ESP_LOGI(TAG, "Set door state to %s", ENUM_NAME(door_state));

  switch (door_state)
  {
  case GDO_DOOR_STATE_OPEN:
    // Set MainState to stopped, OverallCurrentState to fully open, and
    // OverallTargetState to NULL.
    {
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            ESP_LOGI(TAG, "OPEN");
                                            set_main_state(MainStateEnum::kStopped);
                                            set_current_state(CurrentPositionEnum::kFullyOpened);
                                            set_overall_target_state(TargetPositionEnum::kUnknownEnumValue);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_CLOSED:
    // Set MainState to stopped, OverallCurrentState to fully closed, and
    // OverallTargetState to NULL.
    {
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            ESP_LOGI(TAG, "CLOSED");
                                            set_main_state(MainStateEnum::kStopped);
                                            set_current_state(CurrentPositionEnum::kFullyClosed);
                                            set_overall_target_state(TargetPositionEnum::kUnknownEnumValue);
                                          }));
      break;
    }
  case GDO_DOOR_STATE_STOPPED:
    // Set MainState to stopped, OverallCurrentState to partially opened, and
    // OverallTargetState to NULL.
    {
      ChipLogIfFalse(CHIP_NO_ERROR == chip::DeviceLayer::PlatformMgr().ScheduleWork(
                                          [](intptr_t arg)
                                          {
                                            set_main_state(MainStateEnum::kStopped);
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
                                            set_main_state(MainStateEnum::kMoving);
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
                                            set_main_state(MainStateEnum::kMoving);
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