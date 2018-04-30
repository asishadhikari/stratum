// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "third_party/stratum/hal/lib/bcm/bcm_chassis_manager.h"

#include <pthread.h>

#include <algorithm>
#include <set>
#include <sstream>  // IWYU pragma: keep

#include "base/commandlineflags.h"
#include "google/protobuf/message.h"
#include "third_party/stratum/glue/logging.h"
#include "third_party/stratum/hal/lib/bcm/utils.h"
#include "third_party/stratum/hal/lib/common/common.pb.h"
#include "third_party/stratum/hal/lib/common/constants.h"
#include "third_party/stratum/hal/lib/common/utils.h"
#include "third_party/stratum/lib/constants.h"
#include "third_party/stratum/lib/macros.h"
#include "third_party/stratum/lib/utils.h"
#include "third_party/stratum/public/lib/error.h"
#include "third_party/absl/base/integral_types.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/synchronization/mutex.h"
#include "util/gtl/flat_hash_map.h"
#include "util/gtl/map_util.h"
#include "util/gtl/stl_util.h"

DEFINE_string(base_bcm_chassis_map_file, "",
              "The file to read the base_bcm_chassis_map proto.");
DEFINE_string(bcm_sdk_config_file, "/tmp/hercules/config.bcm",
              "The BCM config file loaded by SDK while initializing.");
DEFINE_string(bcm_sdk_config_flush_file, "/tmp/hercules/config.bcm.tmp",
              "The BCM config flush file loaded by SDK while initializing.");
DEFINE_string(bcm_sdk_shell_log_file, "/tmp/hercules/bcm.log",
              "The BCM shell log file loaded by SDK while initializing.");
DEFINE_string(bcm_sdk_checkpoint_dir, "",
              "The dir used by SDK to save checkpoints. Default is empty and "
              "it is expected to be explicitly given by flags.");

namespace stratum {
namespace hal {
namespace bcm {

using LinkscanEvent = BcmSdkInterface::LinkscanEvent;
using TransceiverEvent = PhalInterface::TransceiverEvent;

constexpr int BcmChassisManager::kTomahawkMaxBcmPortsPerChip;
constexpr int BcmChassisManager::kTrident2MaxBcmPortsPerChip;
constexpr int BcmChassisManager::kMaxLinkscanEventDepth;
constexpr int BcmChassisManager::kMaxXcvrEventDepth;

ABSL_CONST_INIT absl::Mutex chassis_lock(absl::kConstInit);

bool shutdown = false;

BcmChassisManager::BcmChassisManager(OperationMode mode,
                                     PhalInterface* phal_interface,
                                     BcmSdkInterface* bcm_sdk_interface,
                                     BcmSerdesDbManager* bcm_serdes_db_manager)
    : mode_(mode),
      initialized_(false),
      linkscan_event_writer_id_(kInvalidWriterId),
      transceiver_event_writer_id_(kInvalidWriterId),
      base_bcm_chassis_map_(nullptr),
      applied_bcm_chassis_map_(nullptr),
      unit_to_bcm_chip_(),
      slot_port_channel_to_bcm_port_(),
      slot_port_to_flex_bcm_ports_(),
      slot_port_to_non_flex_bcm_ports_(),
      slot_port_to_transceiver_state_(),
      unit_to_logical_ports_(),
      node_id_to_unit_(),
      unit_to_node_id_(),
      port_id_to_slot_port_channel_(),
      unit_logical_port_to_port_id_(),
      slot_port_channel_to_port_state_(),
      xcvr_event_channel_(nullptr),
      linkscan_event_channel_(nullptr),
      phal_interface_(CHECK_NOTNULL(phal_interface)),
      bcm_sdk_interface_(CHECK_NOTNULL(bcm_sdk_interface)),
      bcm_serdes_db_manager_(CHECK_NOTNULL(bcm_serdes_db_manager)) {}

// Default constructor is called by the mock class only.
BcmChassisManager::BcmChassisManager()
    : mode_(OPERATION_MODE_STANDALONE),
      initialized_(false),
      linkscan_event_writer_id_(kInvalidWriterId),
      transceiver_event_writer_id_(kInvalidWriterId),
      base_bcm_chassis_map_(nullptr),
      applied_bcm_chassis_map_(nullptr),
      unit_to_bcm_chip_(),
      slot_port_channel_to_bcm_port_(),
      slot_port_to_flex_bcm_ports_(),
      slot_port_to_non_flex_bcm_ports_(),
      slot_port_to_transceiver_state_(),
      unit_to_logical_ports_(),
      node_id_to_unit_(),
      port_id_to_slot_port_channel_(),
      unit_logical_port_to_port_id_(),
      slot_port_channel_to_port_state_(),
      xcvr_event_channel_(nullptr),
      linkscan_event_channel_(nullptr),
      phal_interface_(nullptr),
      bcm_sdk_interface_(nullptr),
      bcm_serdes_db_manager_(nullptr) {}

BcmChassisManager::~BcmChassisManager() {
  // NOTE: We should not detach any unit or unregister any handler in the
  // deconstructor as phal_interface_ or bcm_sdk_interface_ or can be deleted
  // before this class. Make sure you call Shutdown() before deleting the class
  // instance.
  if (initialized_) {
    LOG(ERROR) << "Deleting BcmChassisManager while initialized_ is still "
               << "true. You did not call Shutdown() before deleting the class "
               << "instance. This can lead to unexpected behavior.";
  }
  CleanupInternalState();
}

// TODO: Make sure CPU port ID is not used as ID for any port.
::util::Status BcmChassisManager::PushChassisConfig(
    const ChassisConfig& config) {
  if (!initialized_) {
    // If the class is not initialized. Perform an end-to-end coldboot
    // initialization sequence.
    if (mode_ == OPERATION_MODE_STANDALONE) {
      RETURN_IF_ERROR(bcm_serdes_db_manager_->Load());
    }
    BcmChassisMap base_bcm_chassis_map, target_bcm_chassis_map;
    RETURN_IF_ERROR(GenerateBcmChassisMapFromConfig(
        config, &base_bcm_chassis_map, &target_bcm_chassis_map));
    RETURN_IF_ERROR(
        InitializeBcmChips(base_bcm_chassis_map, target_bcm_chassis_map));
    RETURN_IF_ERROR(
        InitializeInternalState(base_bcm_chassis_map, target_bcm_chassis_map));
    RETURN_IF_ERROR(SyncInternalState(config));
    RETURN_IF_ERROR(ConfigurePortGroups());
    RETURN_IF_ERROR(RegisterEventWriters());
    initialized_ = true;
  } else {
    // If already initialized, sync the internal state and (re-)configure the
    // the flex and non-flex port groups.
    RETURN_IF_ERROR(SyncInternalState(config));
    RETURN_IF_ERROR(ConfigurePortGroups());
  }

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::VerifyChassisConfig(
    const ChassisConfig& config) {
  // Try creating the bcm_chassis_map based on the given config. This will
  // verify almost everything in the config as far as this class is concerned.
  BcmChassisMap base_bcm_chassis_map, target_bcm_chassis_map;
  RETURN_IF_ERROR(GenerateBcmChassisMapFromConfig(config, &base_bcm_chassis_map,
                                                  &target_bcm_chassis_map));

  // If the class is initialized, we also need to check if the new config will
  // require a change in bcm_chassis_map or node_id_to_unit. If so,
  // report reboot required.
  if (initialized_) {
    if (!ProtoEqual(target_bcm_chassis_map, *applied_bcm_chassis_map_)) {
      return MAKE_ERROR(ERR_REBOOT_REQUIRED)
             << "The switch is already initialized, but we detected the newly "
             << "pushed config requires a change in applied_bcm_chassis_map_. "
             << "The stack needs to be rebooted to finish config push.";
    }
    // Find node_id_to_unit that will be generated based on this config.
    std::map<uint64, int> node_id_to_unit;
    for (const auto& singleton_port : config.singleton_ports()) {
      for (const auto& bcm_port : base_bcm_chassis_map.bcm_ports()) {
        if (IsSingletonPortMatchesBcmPort(singleton_port, bcm_port)) {
          node_id_to_unit[singleton_port.node()] = bcm_port.unit();
        }
      }
    }
    if (node_id_to_unit != node_id_to_unit_) {
      return MAKE_ERROR(ERR_REBOOT_REQUIRED)
             << "The switch is already initialized, but we detected the newly "
             << "pushed config requires a change in node_id_to_unit. "
             << "The stack needs to be rebooted to finish config push.";
    }
  }

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::Shutdown() {
  ::util::Status status = ::util::OkStatus();
  APPEND_STATUS_IF_ERROR(status, UnregisterEventWriters());
  APPEND_STATUS_IF_ERROR(status, bcm_sdk_interface_->ShutdownAllUnits());
  initialized_ = false;  // Set to false even if there is an error
  CleanupInternalState();

  return status;
}

::util::StatusOr<BcmChip> BcmChassisManager::GetBcmChip(int unit) const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  const BcmChip* bcm_chip = gtl::FindPtrOrNull(unit_to_bcm_chip_, unit);
  CHECK_RETURN_IF_FALSE(bcm_chip != nullptr)
      << "Failed to find unit as key " << unit << " in unit_to_bcm_chip_.";

  return BcmChip(*bcm_chip);
}

::util::StatusOr<BcmPort> BcmChassisManager::GetBcmPort(int slot, int port,
                                                        int channel) const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  const BcmPort* bcm_port = gtl::FindPtrOrNull(
      slot_port_channel_to_bcm_port_, std::make_tuple(slot, port, channel));
  CHECK_RETURN_IF_FALSE(bcm_port != nullptr)
      << "Failed to find a key (slot: " << slot << ", port: " << port
      << ", channel: " << channel << ") in slot_port_channel_to_bcm_port_.";

  return BcmPort(*bcm_port);
}

::util::StatusOr<std::map<uint64, int>> BcmChassisManager::GetNodeIdToUnitMap()
    const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  return node_id_to_unit_;
}

::util::StatusOr<int> BcmChassisManager::GetUnitFromNodeId(
    uint64 node_id) const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  const int* unit = gtl::FindOrNull(node_id_to_unit_, node_id);
  if (!unit) {
    return MAKE_ERROR(ERR_INVALID_PARAM)
           << "Node " << node_id << " is not configured.";
  }
  return *unit;
}

::util::StatusOr<std::map<uint64, std::pair<int, int>>>
BcmChassisManager::GetPortIdToUnitLogicalPortMap() const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  std::map<uint64, std::pair<int, int>> port_id_to_unit_logical_port = {};
  for (const auto& e : unit_logical_port_to_port_id_) {
    port_id_to_unit_logical_port[e.second] = e.first;
  }

  return port_id_to_unit_logical_port;
}

::util::StatusOr<std::map<uint64, std::pair<int, int>>>
BcmChassisManager::GetTrunkIdToUnitTrunkPortMap() const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }
  std::map<uint64, std::pair<int, int>> trunk_id_to_unit_trunk_port = {};
  // TODO: Implement this.

  return trunk_id_to_unit_trunk_port;
}

::util::StatusOr<PortState> BcmChassisManager::GetPortState(
    uint64 port_id) const {
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }

  const std::tuple<int, int, int>* slot_port_channel_tuple =
      gtl::FindOrNull(port_id_to_slot_port_channel_, port_id);
  CHECK_RETURN_IF_FALSE(slot_port_channel_tuple != nullptr)
      << "Unknown port_id: " << port_id << ".";
  const PortState* port_state = gtl::FindOrNull(
      slot_port_channel_to_port_state_, *slot_port_channel_tuple);
  CHECK_RETURN_IF_FALSE(port_state != nullptr)
      << "Inconsistent state. (slot, port, channel) = ("
      << std::get<0>(*slot_port_channel_tuple) << ", "
      << std::get<1>(*slot_port_channel_tuple) << ", "
      << std::get<2>(*slot_port_channel_tuple)
      << ") is not found as key in slot_port_channel_to_port_state_!";

  return *port_state;
}

std::unique_ptr<BcmChassisManager> BcmChassisManager::CreateInstance(
    OperationMode mode, PhalInterface* phal_interface,
    BcmSdkInterface* bcm_sdk_interface,
    BcmSerdesDbManager* bcm_serdes_db_manager) {
  return absl::WrapUnique(new BcmChassisManager(
      mode, phal_interface, bcm_sdk_interface, bcm_serdes_db_manager));
}

::util::Status BcmChassisManager::GenerateBcmChassisMapFromConfig(
    const ChassisConfig& config, BcmChassisMap* base_bcm_chassis_map,
    BcmChassisMap* target_bcm_chassis_map) const {
  if (base_bcm_chassis_map == nullptr || target_bcm_chassis_map == nullptr) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Null base_bcm_chassis_map or target_bcm_chassis_map.";
  }

  // Clear the map explicitly and re-generate everything from scratch.
  base_bcm_chassis_map->Clear();
  target_bcm_chassis_map->Clear();

  // Load base_bcm_chassis_map before anything else if not done before.
  std::string bcm_chassis_map_id = "";
  if (config.has_vendor_config() &&
      config.vendor_config().has_google_config()) {
    bcm_chassis_map_id =
        config.vendor_config().google_config().bcm_chassis_map_id();
  }
  RETURN_IF_ERROR(
      ReadBaseBcmChassisMapFromFile(bcm_chassis_map_id, base_bcm_chassis_map));

  // Before doing anything, we populate the slot based on the pushed chassis
  // config if we need to do so.
  if (base_bcm_chassis_map->auto_add_slot()) {
    RETURN_IF_ERROR(
        PopulateSlotFromPushedChassisConfig(config, base_bcm_chassis_map));
  }

  // Find the supported BCM chip types based on the given platform.
  CHECK_RETURN_IF_FALSE(config.has_chassis() && config.chassis().platform())
      << "Config needs a Chassis message with correct platform.";
  std::set<BcmChip::BcmChipType> supported_chip_types;
  switch (config.chassis().platform()) {
    case PLT_GENERIC_TRIDENT_PLUS:
      supported_chip_types.insert(BcmChip::TRIDENT_PLUS);
      break;
    case PLT_GENERIC_TRIDENT2:
      supported_chip_types.insert(BcmChip::TRIDENT2);
      break;
    case PLT_GENERIC_TOMAHAWK:
      supported_chip_types.insert(BcmChip::TOMAHAWK);
      break;
    default:
      return MAKE_ERROR(ERR_INTERNAL)
             << "Unsupported platform: "
             << Platform_Name(config.chassis().platform());
  }

  // IDs should match (if there).
  if (!base_bcm_chassis_map->id().empty()) {
    target_bcm_chassis_map->set_id(base_bcm_chassis_map->id());
  }

  // auto_add_logical_ports should match (if there).
  target_bcm_chassis_map->set_auto_add_logical_ports(
      base_bcm_chassis_map->auto_add_logical_ports());

  // auto_add_slot should match (if there).
  target_bcm_chassis_map->set_auto_add_slot(
      base_bcm_chassis_map->auto_add_slot());

  // Include the BcmChassis from base_bcm_chassis_map.
  if (base_bcm_chassis_map->has_bcm_chassis()) {
    *target_bcm_chassis_map->mutable_bcm_chassis() =
        base_bcm_chassis_map->bcm_chassis();
  }

  // Validate Node messages. Make sure there is no two nodes with the same id.
  std::map<uint64, int> node_id_to_unit;
  for (const auto& node : config.nodes()) {
    CHECK_RETURN_IF_FALSE(node.slot() > 0)
        << "No positive slot in " << node.ShortDebugString();
    CHECK_RETURN_IF_FALSE(node.id() > 0)
        << "No positive ID in " << node.ShortDebugString();
    CHECK_RETURN_IF_FALSE(InsertIfNotPresent(&node_id_to_unit, node.id(), -1))
        << "The id for Node " << PrintNode(node) << " was already recorded "
        << "for another Node in the config.";
  }

  // Go over all the ports in the config:
  // 1- For non-flex ports, find the corresponding BcmPort in the
  //    base_bcm_chassis_map and add them to bcm_chassis_map.
  // 2- For flex ports, just save the (slot, port) pairs in flex_ports set but
  //    do not add anything to bcm_chassis_map just yet.
  // 3- Make sure there is no two ports with the same (slot, port, channel).
  // 4- Make sure all the ports with the same (slot, port) have the same
  //    speed.
  // 5- Make sure for each (slot, port) pair, the channels of all the ports
  //    are valid. This depends on the port speed.
  // 6- Make sure no singleton port has the reserved CPU port ID. CPU port is
  //    a special port and is not in the list of singleton ports. It is
  //    configured separately.
  // 7- Keep the set of unit numbers that ports are using so that we can later
  //    add the corresponding BcmChips.

  // TODO: Include MGMT ports in the config if needed.
  std::set<uint64> port_ids;
  std::set<std::tuple<int, int, int>> slot_port_channel_tuples;
  std::set<std::pair<int, int>> flex_slot_port_pairs;
  std::map<std::pair<int, int>, std::set<int>> slot_port_to_channels;
  std::map<std::pair<int, int>, std::set<uint64>> slot_port_to_speed_bps;
  std::map<std::pair<int, int>, std::set<bool>> slot_port_to_internal;
  for (const auto& singleton_port : config.singleton_ports()) {
    CHECK_RETURN_IF_FALSE(singleton_port.id() > 0)
        << "No positive ID in " << PrintSingletonPort(singleton_port) << ".";
    CHECK_RETURN_IF_FALSE(singleton_port.id() != kCpuPortId)
        << "SingletonPort " << PrintSingletonPort(singleton_port)
        << " has the reserved CPU port ID (" << kCpuPortId << ").";
    CHECK_RETURN_IF_FALSE(!port_ids.count(singleton_port.id()))
        << "The id for SingletonPort " << PrintSingletonPort(singleton_port)
        << " was already recorded for another SingletonPort in the config.";
    port_ids.insert(singleton_port.id());
    CHECK_RETURN_IF_FALSE(singleton_port.slot() > 0)
        << "No valid slot in " << singleton_port.ShortDebugString() << ".";
    CHECK_RETURN_IF_FALSE(singleton_port.port() > 0)
        << "No valid port in " << singleton_port.ShortDebugString() << ".";
    CHECK_RETURN_IF_FALSE(singleton_port.speed_bps() > 0)
        << "No valid speed_bps in " << singleton_port.ShortDebugString() << ".";
    const std::tuple<int, int, int>& slot_port_channel_tuple = std::make_tuple(
        singleton_port.slot(), singleton_port.port(), singleton_port.channel());
    CHECK_RETURN_IF_FALSE(
        !slot_port_channel_tuples.count(slot_port_channel_tuple))
        << "The (slot, port, channel) tuple for SingletonPort "
        << PrintSingletonPort(singleton_port)
        << " was already recorded for another SingletonPort in the config.";
    CHECK_RETURN_IF_FALSE(singleton_port.node() > 0)
        << "No valid node ID in " << singleton_port.ShortDebugString() << ".";
    CHECK_RETURN_IF_FALSE(node_id_to_unit.count(singleton_port.node()))
        << "Node ID " << singleton_port.node() << " given for SingletonPort "
        << PrintSingletonPort(singleton_port)
        << " has not been given to any Node in the config.";
    bool found = false;
    const std::pair<int, int>& slot_port_pair =
        std::make_pair(singleton_port.slot(), singleton_port.port());
    for (const auto& bcm_port : base_bcm_chassis_map->bcm_ports()) {
      if (IsSingletonPortMatchesBcmPort(singleton_port, bcm_port)) {
        if (bcm_port.flex_port()) {
          // Flex port detected. Add (slot, port) pairs to flex_ports set.
          flex_slot_port_pairs.insert(slot_port_pair);
        } else {
          // Make sure the (slot, port) for this port are not in the
          // flex_ports vector. This is an invalid situation. We either have all
          // the channels of a frontpanel port flex or all non-flex.
          CHECK_RETURN_IF_FALSE(!flex_slot_port_pairs.count(slot_port_pair))
              << "The (slot, port) pair for the non-flex SingletonPort "
              << PrintSingletonPort(singleton_port)
              << " is in flex_slot_port_pairs.";
          *target_bcm_chassis_map->add_bcm_ports() = bcm_port;
        }
        if (node_id_to_unit[singleton_port.node()] == -1) {
          // First time we are recording unit for this node.
          node_id_to_unit[singleton_port.node()] = bcm_port.unit();
        } else {
          CHECK_RETURN_IF_FALSE(node_id_to_unit[singleton_port.node()] ==
                                bcm_port.unit())
              << "Inconsistent config. SingletonPort "
              << PrintSingletonPort(singleton_port) << " has Node ID "
              << singleton_port.node()
              << " which was previously attched to unit "
              << node_id_to_unit[singleton_port.node()]
              << ". But BcmChassisMap now suggests unit " << bcm_port.unit()
              << " for this port.";
        }
        found = true;
        slot_port_channel_tuples.insert(slot_port_channel_tuple);
        slot_port_to_internal[slot_port_pair].insert(bcm_port.internal());
        break;
      }
    }
    CHECK_RETURN_IF_FALSE(found)
        << "Could not find any BcmPort in base_bcm_chassis_map  whose (slot, "
        << "port, channel, speed_bps) tuple matches non-flex SingletonPort "
        << PrintSingletonPort(singleton_port) << ".";
    slot_port_to_channels[slot_port_pair].insert(singleton_port.channel());
    slot_port_to_speed_bps[slot_port_pair].insert(singleton_port.speed_bps());
  }

  // 1- Add all the BcmChips corresponding to the nodes with the detected unit
  //    numbers.
  // 2- Make sure the chip type is supported.
  for (const auto& e : node_id_to_unit) {
    int unit = e.second;
    if (unit < 0) continue;  // A node with no port. Discard.
    bool found = false;
    for (const auto& bcm_chip : base_bcm_chassis_map->bcm_chips()) {
      if (unit == bcm_chip.unit()) {
        CHECK_RETURN_IF_FALSE(supported_chip_types.count(bcm_chip.type()))
            << "Chip type " << BcmChip::BcmChipType_Name(bcm_chip.type())
            << " is not supported on platform "
            << Platform_Name(config.chassis().platform()) << ".";
        *target_bcm_chassis_map->add_bcm_chips() = bcm_chip;
        found = true;
        break;
      }
    }
    CHECK_RETURN_IF_FALSE(found) << "Could not find any BcmChip for unit "
                                 << unit << " in base_bcm_chassis_map.";
  }

  // Validate internal ports if any.
  for (const auto& e : slot_port_to_internal) {
    CHECK_RETURN_IF_FALSE(e.second.size() == 1)
        << "For SingletonPorts with (slot, port) = (" << e.first.first << ", "
        << e.first.second << ") found both internal and external BCM ports. "
        << "This is invalid.";
  }

  // Validate the speed_bps and channels for all (slot, port) pairs.
  gtl::flat_hash_map<uint64, std::set<int>> speed_bps_to_expected_channels = {
      {kHundredGigBps, {0}},
      {kFortyGigBps, {0}},
      {kFiftyGigBps, {1, 2}},
      {kTwentyGigBps, {1, 2}},
      {kTwentyFiveGigBps, {1, 2, 3, 4}},
      {kTenGigBps, {1, 2, 3, 4}}};
  for (const auto& e : slot_port_to_speed_bps) {
    const std::pair<int, int>& slot_port_pair = e.first;
    CHECK_RETURN_IF_FALSE(e.second.size() == 1)
        << "For SingletonPorts with (slot, port) = (" << slot_port_pair.first
        << ", " << slot_port_pair.second << ") found " << e.second.size()
        << " different "
        << "speed_bps. This is invalid.";
    uint64 speed_bps = *e.second.begin();
    const std::set<int>* channels =
        gtl::FindOrNull(speed_bps_to_expected_channels, speed_bps);
    CHECK_RETURN_IF_FALSE(channels != nullptr)
        << "Unsupported speed_bps: " << speed_bps << ".";
    CHECK_RETURN_IF_FALSE(slot_port_to_channels[slot_port_pair] == *channels)
        << "For SingletonPorts with (slot, port) = (" << slot_port_pair.first
        << ", " << slot_port_pair.second << ") and speed_bps = " << speed_bps
        << " found "
        << "invalid channels.";
  }

  // Now add the flex ports. For each flex port, we add all the 4 channels
  // with a specific speed which depends on the chip.
  for (const auto& slot_port_pair : flex_slot_port_pairs) {
    // Find the BcmChip that contains this (slot, port) pair. We expect the will
    // be one and only one BcmChip what contains this pair.
    std::set<int> units;
    for (const auto& bcm_port : base_bcm_chassis_map->bcm_ports()) {
      if (bcm_port.slot() == slot_port_pair.first &&
          bcm_port.port() == slot_port_pair.second) {
        units.insert(bcm_port.unit());
      }
    }
    CHECK_RETURN_IF_FALSE(units.size() == 1U)
        << "Found ports with (slot, port) = (" << slot_port_pair.first << ", "
        << slot_port_pair.second << ") are on different chips.";
    int unit = *units.begin();
    // We dont use GetBcmChip as unit_to_bcm_chip_ may not be populated when
    // this function is called. This function must be self contained.
    BcmChip::BcmChipType chip_type = BcmChip::UNKNOWN;
    for (const auto& bcm_chip : base_bcm_chassis_map->bcm_chips()) {
      if (bcm_chip.unit() == unit) {
        chip_type = bcm_chip.type();
        break;
      }
    }
    // For each (slot, port) pair, we need to populate all the 4 channels. The
    // speed for these channels depends on the chip type.
    std::vector<int> channels = {1, 2, 3, 4};
    uint64 min_speed_bps;
    switch (chip_type) {
      case BcmChip::TOMAHAWK:
        min_speed_bps = kTwentyFiveGigBps;
        break;
      case BcmChip::TRIDENT2:
        min_speed_bps = kTenGigBps;
        break;
      default:
        return MAKE_ERROR(ERR_INTERNAL) << "Un-supported BCM chip type: "
                                        << BcmChip::BcmChipType_Name(chip_type);
    }
    for (const int channel : channels) {
      SingletonPort singleton_port;
      singleton_port.set_slot(slot_port_pair.first);
      singleton_port.set_port(slot_port_pair.second);
      singleton_port.set_channel(channel);
      singleton_port.set_speed_bps(min_speed_bps);
      bool found = false;
      for (const auto& bcm_port : base_bcm_chassis_map->bcm_ports()) {
        if (IsSingletonPortMatchesBcmPort(singleton_port, bcm_port)) {
          *target_bcm_chassis_map->add_bcm_ports() = bcm_port;
          found = true;
          break;
        }
      }
      CHECK_RETURN_IF_FALSE(found)
          << "Could not find any BcmPort in base_bcm_chassis_map whose (slot, "
          << "port, channel, speed_bps) tuple matches flex SingletonPort "
          << PrintSingletonPort(singleton_port);
    }
  }

  // Now, we need to find the map form unit to (slot, port, channel) tuples
  // and map from unit to chip types. These maps are used for two things:
  // 1- Check for max number of ports per chip.
  // 2- For the case logical ports are expected to be auto added by the
  //    software. In this case, we rewrite the logical port numbers based on
  //    the index of the port within the chip, starting from '1'.
  std::map<int, std::set<std::tuple<int, int, int>>> unit_to_slot_port_channels;
  std::map<int, BcmChip::BcmChipType> unit_to_chip_type;
  for (const auto& bcm_chip : target_bcm_chassis_map->bcm_chips()) {
    unit_to_chip_type[bcm_chip.unit()] = bcm_chip.type();
  }
  for (const auto& bcm_port : target_bcm_chassis_map->bcm_ports()) {
    unit_to_slot_port_channels[bcm_port.unit()].insert(
        std::make_tuple(bcm_port.slot(), bcm_port.port(), bcm_port.channel()));
  }

  // Check for max num of ports per chip.
  std::map<BcmChip::BcmChipType, size_t> chip_type_to_max_num_ports = {
      {BcmChip::TOMAHAWK, kTomahawkMaxBcmPortsPerChip},
      {BcmChip::TRIDENT2, kTrident2MaxBcmPortsPerChip}};
  for (const auto& e : unit_to_chip_type) {
    CHECK_RETURN_IF_FALSE(unit_to_slot_port_channels[e.first].size() <=
                          chip_type_to_max_num_ports[e.second])
        << "Max num of BCM ports for a " << BcmChip::BcmChipType_Name(e.second)
        << " chip is " << chip_type_to_max_num_ports[e.second]
        << ", but we found " << unit_to_slot_port_channels[e.first].size()
        << " ports.";
  }

  // Auto add logical_port numbers for the BCM ports if requested.
  if (target_bcm_chassis_map->auto_add_logical_ports()) {
    // The logical_port will be the 1-based index of the corresponding
    // (slot, port, channel) tuple in the sorted list of tuples found for the
    // unit hosting the port.
    for (int i = 0; i < target_bcm_chassis_map->bcm_ports_size(); ++i) {
      auto* bcm_port = target_bcm_chassis_map->mutable_bcm_ports(i);
      const auto& slot_port_channels =
          unit_to_slot_port_channels[bcm_port->unit()];
      auto it = slot_port_channels.find(std::make_tuple(
          bcm_port->slot(), bcm_port->port(), bcm_port->channel()));
      CHECK_RETURN_IF_FALSE(it != slot_port_channels.end())
          << "Invalid state. (slot, port, channel) = (" << bcm_port->slot()
          << ", " << bcm_port->port() << ", " << bcm_port->channel()
          << ") is not found on unit " << bcm_port->unit() << ".";
      int idx = std::distance(slot_port_channels.begin(), it);
      // Make sure the logical ports start from 1, so we skip the CMIC port (
      // logical port 0).
      bcm_port->set_logical_port(idx + 1);
    }
  }

  // Post validation of target_bcm_chassis_map.
  std::map<int, std::set<int>> unit_to_physical_ports;
  std::map<int, std::set<int>> unit_to_diag_ports;
  std::map<int, std::set<int>> unit_to_logical_ports;
  for (const auto& bcm_chip : target_bcm_chassis_map->bcm_chips()) {
    // For all the BCM unit, logical_port 0 is the CMIC port which cannot be
    // used for anything else.
    unit_to_logical_ports[bcm_chip.unit()].insert(0);
  }

  for (const auto& bcm_port : target_bcm_chassis_map->bcm_ports()) {
    CHECK_RETURN_IF_FALSE(!unit_to_physical_ports[bcm_port.unit()].count(
        bcm_port.physical_port()))
        << "Duplicate physcial_port for unit " << bcm_port.unit() << ": "
        << bcm_port.physical_port();
    CHECK_RETURN_IF_FALSE(
        !unit_to_diag_ports[bcm_port.unit()].count(bcm_port.diag_port()))
        << "Duplicate diag_port for unit " << bcm_port.unit() << ": "
        << bcm_port.diag_port();
    CHECK_RETURN_IF_FALSE(
        !unit_to_logical_ports[bcm_port.unit()].count(bcm_port.logical_port()))
        << "Duplicate logical_port for unit " << bcm_port.unit() << ": "
        << bcm_port.logical_port();
    unit_to_physical_ports[bcm_port.unit()].insert(bcm_port.physical_port());
    unit_to_diag_ports[bcm_port.unit()].insert(bcm_port.diag_port());
    unit_to_logical_ports[bcm_port.unit()].insert(bcm_port.logical_port());
  }

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::InitializeBcmChips(
    const BcmChassisMap& base_bcm_chassis_map,
    const BcmChassisMap& target_bcm_chassis_map) {
  if (initialized_) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "InitializeBcmChips() can be called only before the class is "
           << "initialized.";
  }

  // Need to make sure target_bcm_chassis_map given here is a pruned version of
  // the base_bcm_chassis_map.
  CHECK_RETURN_IF_FALSE(base_bcm_chassis_map.id() ==
                        target_bcm_chassis_map.id())
      << "The value of 'id' in base_bcm_chassis_map and "
      << "target_bcm_chassis_map must match (" << base_bcm_chassis_map.id()
      << " != " << target_bcm_chassis_map.id() << ").";
  CHECK_RETURN_IF_FALSE(base_bcm_chassis_map.auto_add_logical_ports() ==
                        target_bcm_chassis_map.auto_add_logical_ports())
      << "The value of 'auto_add_logical_ports' in base_bcm_chassis_map and "
      << "target_bcm_chassis_map must match.";
  CHECK_RETURN_IF_FALSE(base_bcm_chassis_map.has_bcm_chassis() ==
                        target_bcm_chassis_map.has_bcm_chassis())
      << "Both base_bcm_chassis_map and target_bcm_chassis_map must either "
      << "have 'bcm_chassis' or miss it.";
  if (target_bcm_chassis_map.has_bcm_chassis()) {
    CHECK_RETURN_IF_FALSE(ProtoEqual(target_bcm_chassis_map.bcm_chassis(),
                                     base_bcm_chassis_map.bcm_chassis()))
        << "BcmChassis in base_bcm_chassis_map and target_bcm_chassis_map do "
        << "not match.";
  }
  for (const auto& bcm_chip : target_bcm_chassis_map.bcm_chips()) {
    CHECK_RETURN_IF_FALSE(std::any_of(base_bcm_chassis_map.bcm_chips().begin(),
                                      base_bcm_chassis_map.bcm_chips().end(),
                                      [&bcm_chip](const ::google::protobuf::Message& x) {
                                        return ProtoEqual(x, bcm_chip);
                                      }))
        << "BcmChip " << bcm_chip.ShortDebugString() << " was not found in "
        << "base_bcm_chassis_map.";
  }
  for (const auto& bcm_port : target_bcm_chassis_map.bcm_ports()) {
    BcmPort p(bcm_port);
    if (target_bcm_chassis_map.auto_add_logical_ports()) {
      // The base comes with no logical_port assigned.
      p.clear_logical_port();
    }
    CHECK_RETURN_IF_FALSE(std::any_of(
        base_bcm_chassis_map.bcm_ports().begin(),
        base_bcm_chassis_map.bcm_ports().end(),
        [&p](const ::google::protobuf::Message& x) { return ProtoEqual(x, p); }))
        << "BcmPort " << p.ShortDebugString() << " was not found in "
        << "base_bcm_chassis_map.";
  }

  // Generate the config.bcm file given target_bcm_chassis_map.
  RETURN_IF_ERROR(
      WriteBcmConfigFile(base_bcm_chassis_map, target_bcm_chassis_map));

  // Create SDK checkpoint dir. This needs to be create before SDK is
  // initialized.
  RETURN_IF_ERROR(RecursivelyCreateDir(FLAGS_bcm_sdk_checkpoint_dir));

  // Initialize the SDK.
  RETURN_IF_ERROR(bcm_sdk_interface_->InitializeSdk(
      FLAGS_bcm_sdk_config_file, FLAGS_bcm_sdk_config_flush_file,
      FLAGS_bcm_sdk_shell_log_file));

  // Attach all the units. Note that we keep the things simple. We will move
  // forward iff all the units are attched successfully.
  for (const auto& bcm_chip : target_bcm_chassis_map.bcm_chips()) {
    RETURN_IF_ERROR(
        bcm_sdk_interface_->FindUnit(bcm_chip.unit(), bcm_chip.pci_bus(),
                                     bcm_chip.pci_slot(), bcm_chip.type()));
    RETURN_IF_ERROR(bcm_sdk_interface_->InitializeUnit(bcm_chip.unit(),
                                                       /*warm_boot=*/false));
    RETURN_IF_ERROR(
        bcm_sdk_interface_->SetModuleId(bcm_chip.unit(), bcm_chip.module()));
  }

  // Initialize all the ports (flex or not).
  for (const auto& bcm_port : target_bcm_chassis_map.bcm_ports()) {
    RETURN_IF_ERROR(bcm_sdk_interface_->InitializePort(
        bcm_port.unit(), bcm_port.logical_port()));
  }

  // Start the diag thread.
  RETURN_IF_ERROR(bcm_sdk_interface_->StartDiagShellServer());

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::InitializeInternalState(
    const BcmChassisMap& base_bcm_chassis_map,
    const BcmChassisMap& target_bcm_chassis_map) {
  if (initialized_) {
    return MAKE_ERROR(ERR_INTERNAL) << "InitializeInternalState() can be "
                                    << "called only before the class is "
                                    << "initialized.";
  }

  // By the time we get here, target_bcm_chassis_map is verified and the chips
  // has been initialized using it, save the copy of this proto and
  // base_bcm_chassis_map.
  base_bcm_chassis_map_ =
      absl::make_unique<BcmChassisMap>(base_bcm_chassis_map);
  applied_bcm_chassis_map_ =
      absl::make_unique<BcmChassisMap>(target_bcm_chassis_map);

  // Also, after initialization is done for all the ports, set the initial state
  // of the transceivers.
  slot_port_to_transceiver_state_.clear();
  for (const auto& bcm_port : target_bcm_chassis_map.bcm_ports()) {
    const std::pair<int, int>& slot_port_pair =
        std::make_pair(bcm_port.slot(), bcm_port.port());
    // For external ports, wait for transceiver module event handler to find all
    // the inserted transceiver modules (QSFPs, SFPs, etc). For internal ports,
    // there is no transceiver module event. They are always up, but we set them
    // as HW_STATE_PRESENT (unconfigured) so they get
    // configured later.
    if (bcm_port.internal()) {
      slot_port_to_transceiver_state_[slot_port_pair] = HW_STATE_PRESENT;
    } else {
      slot_port_to_transceiver_state_[slot_port_pair] = HW_STATE_UNKNOWN;
    }
  }

  // TODO: write base_bcm_chassis_map_ and applied_bcm_chassis_map_
  // protos into files for debugging purposes?

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::SyncInternalState(
    const ChassisConfig& config) {
  // Populate the internal map. We have done verification before we get to
  // this point. So, no need to re-verify the config.
  gtl::STLDeleteValues(&unit_to_bcm_chip_);
  gtl::STLDeleteValues(&slot_port_channel_to_bcm_port_);
  // No need to delete the pointer in the following two maps. They are already
  // deleted by calling STLDeleteValues(&slot_port_channel_to_bcm_port_).
  slot_port_to_flex_bcm_ports_.clear();
  slot_port_to_non_flex_bcm_ports_.clear();
  unit_to_logical_ports_.clear();
  node_id_to_unit_.clear();
  unit_to_node_id_.clear();
  node_id_to_port_ids_.clear();
  port_id_to_slot_port_channel_.clear();
  unit_logical_port_to_port_id_.clear();
  // A tmp map to hold port state data. At the end of this function, we replace
  // slot_port_channel_to_port_state_ with this map. This is to make sure we
  // do not lose any state.
  std::map<std::tuple<int, int, int>, PortState>
      tmp_slot_port_channel_to_port_state;

  // Initialize the maps that have node ID as key, i.e. node_id_to_unit_ and
  // node_id_to_port_ids_. There might be a case where not all the nodes are
  // used by the singleton ports.
  for (const auto& node : config.nodes()) {
    node_id_to_unit_[node.id()] = -1;
    node_id_to_port_ids_[node.id()] = {};
  }

  // Now populate unit_to_bcm_chip_. The nodes are already in
  // applied_bcm_chassis_map_ which was updated in InitializeInternalState().
  // The nodes in applied_bcm_chassis_map_ cannot be changed after the first
  // config push.
  for (const auto& bcm_chip : applied_bcm_chassis_map_->bcm_chips()) {
    unit_to_bcm_chip_[bcm_chip.unit()] = new BcmChip(bcm_chip);
    // CMIC port included by default.
    unit_to_logical_ports_[bcm_chip.unit()].insert(0);
  }

  // Now populate the rest of the maps. Everything that is port related.
  for (const auto& singleton_port : config.singleton_ports()) {
    for (const auto& bcm_port : base_bcm_chassis_map_->bcm_ports()) {
      if (IsSingletonPortMatchesBcmPort(singleton_port, bcm_port)) {
        const std::tuple<int, int, int>& slot_port_channel_tuple =
            std::make_tuple(singleton_port.slot(), singleton_port.port(),
                            singleton_port.channel());
        CHECK_RETURN_IF_FALSE(
            !slot_port_channel_to_bcm_port_.count(slot_port_channel_tuple))
            << "The (slot, port, channel) tuple for SingletonPort "
            << PrintSingletonPort(singleton_port)
            << " already exists as a key in slot_port_channel_to_bcm_port_. "
            << "Have you called VerifyChassisConfig()?";
        auto* p = new BcmPort(bcm_port);
        // If auto_add_logical_ports=true, the logical_port needs to come
        // from applied_bcm_chassis_map_.
        if (applied_bcm_chassis_map_->auto_add_logical_ports()) {
          bool found = false;
          for (const auto& q : applied_bcm_chassis_map_->bcm_ports()) {
            if (p->unit() == q.unit() &&
                p->physical_port() == q.physical_port() &&
                p->diag_port() == q.diag_port()) {
              p->set_logical_port(q.logical_port());
              found = true;
              break;
            }
          }
          CHECK_RETURN_IF_FALSE(found)
              << "Found not matching BcmPort in applied_bcm_chassis_map_ which "
              << "matches unit, physical_port and diag_port of BcmPort '"
              << p->ShortDebugString() << "'.";
        }
        slot_port_channel_to_bcm_port_[slot_port_channel_tuple] = p;
        node_id_to_unit_[singleton_port.node()] = p->unit();
        unit_to_node_id_[p->unit()] = singleton_port.node();
        node_id_to_port_ids_[singleton_port.node()].insert(singleton_port.id());
        unit_to_logical_ports_[p->unit()].insert(p->logical_port());
        port_id_to_slot_port_channel_[singleton_port.id()] =
            slot_port_channel_tuple;
        const std::pair<int, int>& unit_logical_port_pair =
            std::make_pair(p->unit(), p->logical_port());
        unit_logical_port_to_port_id_[unit_logical_port_pair] =
            singleton_port.id();
        const std::pair<int, int>& slot_port_pair =
            std::make_pair(singleton_port.slot(), singleton_port.port());
        CHECK_RETURN_IF_FALSE(
            slot_port_to_transceiver_state_.count(slot_port_pair))
            << "Something is wrong. ChassisConfig contains a (slot, port) "
            << "which we dont know about: (" << slot_port_pair.first << ", "
            << slot_port_pair.second << ").";
        if (bcm_port.flex_port()) {
          slot_port_to_flex_bcm_ports_[slot_port_pair].push_back(p);
        } else {
          slot_port_to_non_flex_bcm_ports_[slot_port_pair].push_back(p);
        }
        // If (slot, port, channel) tuple already exists as a key in
        // slot_port_channel_to_port_state_, we keep the same state. Otherwise
        // we assume this is the first time we are seeing this port and set the
        // state to PORT_STATE_UNKNOWN.
        const PortState* port_state = gtl::FindOrNull(
            slot_port_channel_to_port_state_, slot_port_channel_tuple);
        if (port_state != nullptr) {
          tmp_slot_port_channel_to_port_state[slot_port_channel_tuple] =
              *port_state;
        } else {
          tmp_slot_port_channel_to_port_state[slot_port_channel_tuple] =
              PORT_STATE_UNKNOWN;
        }
        break;
      }
    }
  }

  // Update slot_port_channel_to_port_state_ as the end.
  slot_port_channel_to_port_state_ = tmp_slot_port_channel_to_port_state;

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::RegisterEventWriters() {
  if (initialized_) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "RegisterEventWriters() can be called only before the class is "
           << "initialized.";
  }

  // If we have not done that yet, create linkscan event Channel, register
  // Writer, and create Reader thread.
  if (linkscan_event_writer_id_ == kInvalidWriterId) {
    linkscan_event_channel_ =
        Channel<LinkscanEvent>::Create(kMaxLinkscanEventDepth);
    // Create and hand-off Writer to the BcmSdkInterface.
    auto writer = ChannelWriter<LinkscanEvent>::Create(linkscan_event_channel_);
    int priority = BcmSdkInterface::kLinkscanEventWriterPriorityHigh;
    ASSIGN_OR_RETURN(linkscan_event_writer_id_,
                     bcm_sdk_interface_->RegisterLinkscanEventWriter(
                         std::move(writer), priority));
    // Create and hand-off Reader to new reader thread.
    auto reader = ChannelReader<LinkscanEvent>::Create(linkscan_event_channel_);
    pthread_t linkscan_event_reader_tid;
    int ret = pthread_create(
        &linkscan_event_reader_tid, nullptr, LinkscanEventHandlerThreadFunc,
        new ReaderArgs<LinkscanEvent>{this, std::move(reader)});
    if (ret != 0) {
      return MAKE_ERROR(ERR_INTERNAL)
             << "Failed to create linkscan thread. Err: " << ret << ".";
    }
    // We don't care about the return value. The thread should exit following
    // the closing of the Channel in UnregisterEventWriters().
    ret = pthread_detach(linkscan_event_reader_tid);
    if (ret != 0) {
      return MAKE_ERROR(ERR_INTERNAL)
             << "Failed to detach linkscan thread. Err: " << ret << ".";
    }
    // Start the linkscan.
    for (const auto& e : unit_to_bcm_chip_) {
      RETURN_IF_ERROR(bcm_sdk_interface_->StartLinkscan(e.first));
    }
  }

  // If we have not done that yet, create transceiver module insert/removal
  // event Channel, register ChannelWriter, and create ChannelReader thread.
  if (transceiver_event_writer_id_ == kInvalidWriterId) {
    xcvr_event_channel_ = Channel<TransceiverEvent>::Create(kMaxXcvrEventDepth);
    // Create and hand-off ChannelWriter to the PhalInterface.
    auto writer = ChannelWriter<TransceiverEvent>::Create(xcvr_event_channel_);
    int priority = PhalInterface::kTransceiverEventWriterPriorityHigh;
    ASSIGN_OR_RETURN(transceiver_event_writer_id_,
                     phal_interface_->RegisterTransceiverEventWriter(
                         std::move(writer), priority));
    // Create and hand-off ChannelReader to new reader thread.
    auto reader = ChannelReader<TransceiverEvent>::Create(xcvr_event_channel_);
    pthread_t xcvr_event_reader_tid;
    int ret = pthread_create(
        &xcvr_event_reader_tid, nullptr, TransceiverEventHandlerThreadFunc,
        new ReaderArgs<TransceiverEvent>{this, std::move(reader)});
    if (ret != 0) {
      return MAKE_ERROR(ERR_INTERNAL)
             << "Failed to create transceiver event thread. Err: " << ret
             << ".";
    }
    // We don't care about the return value of the thread. It should exit once
    // the Channel is closed in UnregisterEventWriters().
    ret = pthread_detach(xcvr_event_reader_tid);
    if (ret != 0) {
      return MAKE_ERROR(ERR_INTERNAL)
             << "Failed to detach transceiver event thread. Err: " << ret
             << ".";
    }
  }

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::UnregisterEventWriters() {
  ::util::Status status = ::util::OkStatus();
  // Unregister the linkscan and transceiver module event Writers.
  if (linkscan_event_writer_id_ != kInvalidWriterId) {
    APPEND_STATUS_IF_ERROR(status,
                           bcm_sdk_interface_->UnregisterLinkscanEventWriter(
                               linkscan_event_writer_id_));
    linkscan_event_writer_id_ = kInvalidWriterId;
    // Close Channel.
    if (!linkscan_event_channel_ || !linkscan_event_channel_->Close()) {
      APPEND_ERROR(status) << "Linkscan event Channel is already closed.";
    }
    linkscan_event_channel_.reset();
  }
  if (transceiver_event_writer_id_ != kInvalidWriterId) {
    APPEND_STATUS_IF_ERROR(status,
                           phal_interface_->UnregisterTransceiverEventWriter(
                               transceiver_event_writer_id_));
    transceiver_event_writer_id_ = kInvalidWriterId;
    // Close Channel.
    if (!xcvr_event_channel_ || !xcvr_event_channel_->Close()) {
      APPEND_ERROR(status) << "Transceiver event Channel is already closed.";
    }
    xcvr_event_channel_.reset();
  }

  return status;
}

::util::Status BcmChassisManager::RegisterEventNotifyWriter(
    const std::shared_ptr<WriterInterface<GnmiEventPtr>>& writer) {
  absl::WriterMutexLock l(&gnmi_event_lock_);
  gnmi_event_writer_ = writer;
  return ::util::OkStatus();
}

::util::Status BcmChassisManager::UnregisterEventNotifyWriter() {
  absl::WriterMutexLock l(&gnmi_event_lock_);
  gnmi_event_writer_ = nullptr;
  return ::util::OkStatus();
}

::util::Status BcmChassisManager::ConfigurePortGroups() {
  ::util::Status status = ::util::OkStatus();
  // Set the speed for flex port groups first.
  for (const auto& e : slot_port_to_flex_bcm_ports_) {
    ::util::StatusOr<bool> ret = SetSpeedForFlexPortGroup(e.first);
    if (!ret.ok()) {
      APPEND_STATUS_IF_ERROR(status, ret.status());
      continue;
    }
    bool speed_changed = ret.ValueOrDie();
    // If there is a change in port speed and port is HW_STATE_READY, set it
    // to HW_STATE_PRESENT (non-configured state) so it gets configured next.
    if (speed_changed &&
        slot_port_to_transceiver_state_[e.first] == HW_STATE_READY) {
      slot_port_to_transceiver_state_[e.first] = HW_STATE_PRESENT;
    }
  }
  // Then continue with port options.
  for (auto& e : slot_port_to_transceiver_state_) {
    if (e.second != HW_STATE_READY) {
      BcmPortOptions options;
      options.set_enabled(e.second == HW_STATE_PRESENT ? TRI_STATE_TRUE
                                                       : TRI_STATE_FALSE);
      options.set_blocked(e.second != HW_STATE_PRESENT ? TRI_STATE_TRUE
                                                       : TRI_STATE_FALSE);
      ::util::Status error = SetPortOptionsForPortGroup(e.first, options);
      if (!error.ok()) {
        APPEND_STATUS_IF_ERROR(status, error);
        continue;
      }
      if (e.second == HW_STATE_PRESENT) {
        // A HW_STATE_PRESENT port group after configuration is HW_STATE_READY.
        e.second = HW_STATE_READY;
      }
    }
  }

  return status;
}

void BcmChassisManager::CleanupInternalState() {
  gtl::STLDeleteValues(&unit_to_bcm_chip_);
  gtl::STLDeleteValues(&slot_port_channel_to_bcm_port_);
  // No need to delete the pointer in these two maps. They are already deleted
  // by calling STLDeleteValues(&slot_port_channel_to_bcm_port_).
  slot_port_to_flex_bcm_ports_.clear();
  slot_port_to_non_flex_bcm_ports_.clear();
  slot_port_to_transceiver_state_.clear();
  unit_to_logical_ports_.clear();
  node_id_to_unit_.clear();
  unit_to_node_id_.clear();
  node_id_to_port_ids_.clear();
  port_id_to_slot_port_channel_.clear();
  unit_logical_port_to_port_id_.clear();
  slot_port_channel_to_port_state_.clear();
  base_bcm_chassis_map_ = nullptr;
  applied_bcm_chassis_map_ = nullptr;
}

::util::Status BcmChassisManager::ReadBaseBcmChassisMapFromFile(
    const std::string& bcm_chassis_map_id,
    BcmChassisMap* base_bcm_chassis_map) const {
  if (base_bcm_chassis_map == nullptr) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Did you pass a null base_bcm_chassis_map pointer?";
  }

  // Read the proto from the path given by base_bcm_chassis_map_file flag.
  BcmChassisMapList bcm_chassis_map_list;
  RETURN_IF_ERROR(ReadProtoFromTextFile(FLAGS_base_bcm_chassis_map_file,
                                        &bcm_chassis_map_list));
  base_bcm_chassis_map->Clear();
  bool found = false;
  for (const auto& bcm_chassis_map : bcm_chassis_map_list.bcm_chassis_maps()) {
    if (bcm_chassis_map_id.empty() ||
        bcm_chassis_map_id == bcm_chassis_map.id()) {
      *base_bcm_chassis_map = bcm_chassis_map;
      found = true;
      break;
    }
  }
  CHECK_RETURN_IF_FALSE(found)
      << "Did not find a BcmChassisMap with id " << bcm_chassis_map_id << " in "
      << FLAGS_base_bcm_chassis_map_file;

  // Verify the messages base_bcm_chassis_map.
  std::set<int> slots;
  std::set<int> units;
  std::set<int> modules;
  for (const auto& bcm_chip : base_bcm_chassis_map->bcm_chips()) {
    CHECK_RETURN_IF_FALSE(bcm_chip.type())
        << "Invalid type in " << bcm_chip.ShortDebugString();
    if (base_bcm_chassis_map->auto_add_slot()) {
      CHECK_RETURN_IF_FALSE(bcm_chip.slot() == 0)
          << "auto_add_slot is True and slot is non-zero for chip "
          << bcm_chip.ShortDebugString();
    } else {
      CHECK_RETURN_IF_FALSE(bcm_chip.slot() > 0)
          << "Invalid slot in " << bcm_chip.ShortDebugString();
      slots.insert(bcm_chip.slot());
    }
    CHECK_RETURN_IF_FALSE(bcm_chip.unit() >= 0 && !units.count(bcm_chip.unit()))
        << "Invalid unit in " << bcm_chip.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_chip.module() >= 0 &&
                          !modules.count(bcm_chip.module()))
        << "Invalid module in " << bcm_chip.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_chip.pci_bus() >= 0)
        << "Invalid pci_bus in " << bcm_chip.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_chip.pci_slot() >= 0)
        << "Invalid pci_slot in " << bcm_chip.ShortDebugString();
    units.insert(bcm_chip.unit());
    modules.insert(bcm_chip.module());
  }
  for (const auto& bcm_port : base_bcm_chassis_map->bcm_ports()) {
    CHECK_RETURN_IF_FALSE(bcm_port.type())
        << "Invalid type in " << bcm_port.ShortDebugString();
    if (base_bcm_chassis_map->auto_add_slot()) {
      CHECK_RETURN_IF_FALSE(bcm_port.slot() == 0)
          << "auto_add_slot is True and slot is non-zero for port "
          << bcm_port.ShortDebugString();
    } else {
      CHECK_RETURN_IF_FALSE(bcm_port.slot() > 0 && slots.count(bcm_port.slot()))
          << "Invalid slot in " << bcm_port.ShortDebugString();
    }
    CHECK_RETURN_IF_FALSE(bcm_port.port() > 0)
        << "Invalid port in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.channel() >= 0 && bcm_port.channel() <= 4)
        << "Invalid channel in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.unit() >= 0 && units.count(bcm_port.unit()))
        << "Invalid unit in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.speed_bps() > 0 &&
                          bcm_port.speed_bps() % kBitsPerGigabit == 0)
        << "Invalid speed_bps in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.physical_port() >= 0)
        << "Invalid physical_port in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.diag_port() >= 0)
        << "Invalid diag_port in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.module() >= 0 &&
                          modules.count(bcm_port.module()))
        << "Invalid module in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.serdes_core() >= 0)
        << "Invalid serdes_core in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.serdes_lane() >= 0 &&
                          bcm_port.serdes_lane() <= 3)
        << "Invalid serdes_lane in " << bcm_port.ShortDebugString();
    if (bcm_port.type() != BcmPort::MGMT) {
      CHECK_RETURN_IF_FALSE(bcm_port.num_serdes_lanes() >= 1 &&
                            bcm_port.num_serdes_lanes() <= 4)
          << "Invalid num_serdes_lanes in " << bcm_port.ShortDebugString();
    }
    CHECK_RETURN_IF_FALSE(bcm_port.tx_lane_map() >= 0)
        << "Invalid tx_lane_map in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.rx_lane_map() >= 0)
        << "Invalid rx_lane_map in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.tx_polarity_flip() >= 0)
        << "Invalid tx_polarity_flip in " << bcm_port.ShortDebugString();
    CHECK_RETURN_IF_FALSE(bcm_port.rx_polarity_flip() >= 0)
        << "Invalid rx_polarity_flip in " << bcm_port.ShortDebugString();
    if (base_bcm_chassis_map->auto_add_logical_ports()) {
      CHECK_RETURN_IF_FALSE(bcm_port.logical_port() == 0)
          << "auto_add_logical_ports is True and logical_port is non-zero "
          << bcm_port.ShortDebugString();
    } else {
      CHECK_RETURN_IF_FALSE(bcm_port.logical_port() > 0)
          << "auto_add_logical_ports is False and logical_port is not positive "
          << bcm_port.ShortDebugString();
    }
  }

  return ::util::OkStatus();
}

::util::Status BcmChassisManager::PopulateSlotFromPushedChassisConfig(
    const ChassisConfig& config, BcmChassisMap* base_bcm_chassis_map) const {
  std::set<int> slots;
  for (const auto& node : config.nodes()) {
    slots.insert(node.slot());
  }
  for (const auto& singleton_port : config.singleton_ports()) {
    slots.insert(singleton_port.slot());
  }
  CHECK_RETURN_IF_FALSE(slots.size() == 1U)
      << "Cannot support a case where auto_add_slot is true and we have more "
      << "than one slot number specified in the ChassisConfig.";
  int slot = *slots.begin();
  for (int i = 0; i < base_bcm_chassis_map->bcm_chips_size(); ++i) {
    base_bcm_chassis_map->mutable_bcm_chips(i)->set_slot(slot);
  }
  for (int i = 0; i < base_bcm_chassis_map->bcm_ports_size(); ++i) {
    base_bcm_chassis_map->mutable_bcm_ports(i)->set_slot(slot);
  }
  VLOG(1) << "Automatically added slot " << slot << " to all the BcmChips & "
          << "BcmPorts in the base BcmChassisMap.";

  return ::util::OkStatus();
}

bool BcmChassisManager::IsSingletonPortMatchesBcmPort(
    const SingletonPort& singleton_port, const BcmPort& bcm_port) const {
  if (bcm_port.type() != BcmPort::XE && bcm_port.type() != BcmPort::CE) {
    return false;
  }

  bool result = (singleton_port.slot() == bcm_port.slot() &&
                 singleton_port.port() == bcm_port.port() &&
                 singleton_port.channel() == bcm_port.channel() &&
                 singleton_port.speed_bps() == bcm_port.speed_bps());

  return result;
}

::util::Status BcmChassisManager::WriteBcmConfigFile(
    const BcmChassisMap& base_bcm_chassis_map,
    const BcmChassisMap& target_bcm_chassis_map) const {
  std::stringstream buffer;

  // initialize the port mask. The total number of chips supported comes from
  // base_bcm_chassis_map.
  const size_t max_num_units = base_bcm_chassis_map.bcm_chips_size();
  std::vector<uint64> xe_pbmp_mask0(max_num_units, 0);
  std::vector<uint64> xe_pbmp_mask1(max_num_units, 0);
  std::vector<uint64> xe_pbmp_mask2(max_num_units, 0);
  std::vector<bool> is_chip_oversubscribed(max_num_units, false);

  // Chassis-level SDK properties.
  if (target_bcm_chassis_map.has_bcm_chassis()) {
    const auto& bcm_chassis = target_bcm_chassis_map.bcm_chassis();
    for (const std::string& sdk_property : bcm_chassis.sdk_properties()) {
      buffer << sdk_property << std::endl;
    }
    // In addition to SDK properties in the config, in the sim mode we need to
    // also add properties to disable DMA.
    if (mode_ == OPERATION_MODE_SIM) {
      buffer << "tdma_intr_enable=0" << std::endl;
      buffer << "tslam_dma_enable=0" << std::endl;
      buffer << "table_dma_enable=0" << std::endl;
    }
    buffer << std::endl;
  }

  // Chip-level SDK properties.
  for (const auto& bcm_chip : target_bcm_chassis_map.bcm_chips()) {
    int unit = bcm_chip.unit();
    if (bcm_chip.sdk_properties_size()) {
      for (const std::string& sdk_property : bcm_chip.sdk_properties()) {
        buffer << sdk_property << std::endl;
      }
      buffer << std::endl;
    }
    if (bcm_chip.is_oversubscribed()) {
      is_chip_oversubscribed[unit] = true;
    }
  }

  // XE port maps.
  // TODO: See if there is some BCM macros to work with pbmp's.
  for (const auto& bcm_port : target_bcm_chassis_map.bcm_ports()) {
    if (bcm_port.type() == BcmPort::XE || bcm_port.type() == BcmPort::CE) {
      int idx = bcm_port.logical_port();
      int unit = bcm_port.unit();
      if (idx < 64) {
        xe_pbmp_mask0[unit] |= static_cast<uint64>(0x01) << idx;
      } else if (idx < 128) {
        xe_pbmp_mask1[unit] |= static_cast<uint64>(0x01) << (idx - 64);
      } else {
        xe_pbmp_mask2[unit] |= static_cast<uint64>(0x01) << (idx - 128);
      }
    }
  }
  for (size_t i = 0; i < max_num_units; ++i) {
    if (xe_pbmp_mask1[i] || xe_pbmp_mask0[i] || xe_pbmp_mask2[i]) {
      std::stringstream mask(std::stringstream::in | std::stringstream::out);
      std::stringstream t0(std::stringstream::in | std::stringstream::out);
      std::stringstream t1(std::stringstream::in | std::stringstream::out);
      if (xe_pbmp_mask2[i]) {
        t0 << std::hex << std::uppercase << xe_pbmp_mask0[i];
        t1 << std::hex << std::uppercase << xe_pbmp_mask1[i];
        mask << std::hex << std::uppercase << xe_pbmp_mask2[i]
             << std::string(2 * sizeof(uint64) - t1.str().length(), '0')
             << t1.str()
             << std::string(2 * sizeof(uint64) - t0.str().length(), '0')
             << t0.str();
      } else if (xe_pbmp_mask1[i]) {
        t0 << std::hex << std::uppercase << xe_pbmp_mask0[i];
        mask << std::hex << std::uppercase << xe_pbmp_mask1[i]
             << std::string(2 * sizeof(uint64) - t0.str().length(), '0')
             << t0.str();
      } else {
        mask << std::hex << std::uppercase << xe_pbmp_mask0[i];
      }
      buffer << "pbmp_xport_xe." << i << "=0x" << mask.str() << std::endl;
      if (is_chip_oversubscribed[i]) {
        buffer << "pbmp_oversubscribe." << i << "=0x" << mask.str()
               << std::endl;
      }
    }
  }
  buffer << std::endl;

  // Port properties. Before that we create a map from chip-type to
  // map of channel to speed_bps for the flex ports.
  std::map<BcmChip::BcmChipType, std::map<int, uint64>>
      flex_chip_to_channel_to_speed = {{BcmChip::TOMAHAWK,
                                        {{1, kHundredGigBps},
                                         {2, kTwentyFiveGigBps},
                                         {3, kFiftyGigBps},
                                         {4, kTwentyFiveGigBps}}},
                                       {BcmChip::TRIDENT2,
                                        {{1, kFortyGigBps},
                                         {2, kTenGigBps},
                                         {3, kTwentyGigBps},
                                         {4, kTenGigBps}}}};
  for (const auto& bcm_port : target_bcm_chassis_map.bcm_ports()) {
    uint64 speed_bps = 0;
    if (bcm_port.type() == BcmPort::XE || bcm_port.type() == BcmPort::CE) {
      // Find the type of the chip hosting this port. Then find the speed
      // which we need to set in the config.bcm, which depends on whether
      // the port is flex or not. We dont use GetBcmChip as unit_to_bcm_chip_
      // may not be populated when this function is called.
      BcmChip::BcmChipType chip_type = BcmChip::UNKNOWN;
      for (const auto& bcm_chip : target_bcm_chassis_map.bcm_chips()) {
        if (bcm_chip.unit() == bcm_port.unit()) {
          chip_type = bcm_chip.type();
          break;
        }
      }
      if (bcm_port.flex_port()) {
        CHECK_RETURN_IF_FALSE(chip_type == BcmChip::TOMAHAWK ||
                              chip_type == BcmChip::TRIDENT2)
            << "Un-supported BCM chip type: "
            << BcmChip::BcmChipType_Name(chip_type);
        CHECK_RETURN_IF_FALSE(bcm_port.channel() >= 1 &&
                              bcm_port.channel() <= 4)
            << "Flex-port with no channel: " << bcm_port.ShortDebugString();
        speed_bps =
            flex_chip_to_channel_to_speed[chip_type][bcm_port.channel()];
      } else {
        speed_bps = bcm_port.speed_bps();
      }
    } else if (bcm_port.type() == BcmPort::MGMT) {
      CHECK_RETURN_IF_FALSE(!bcm_port.flex_port())
          << "Mgmt ports cannot be flex.";
      speed_bps = bcm_port.speed_bps();
    } else {
      return MAKE_ERROR(ERR_INTERNAL)
             << "Un-supported BCM port type: " << bcm_port.type();
    }

    // Port speed and diag port setting.
    buffer << "portmap_" << bcm_port.logical_port() << "." << bcm_port.unit()
           << "=" << bcm_port.physical_port() << ":"
           << speed_bps / kBitsPerGigabit;
    if (bcm_port.flex_port() && bcm_port.serdes_lane()) {
      buffer << ":i";
    }
    buffer << std::endl;
    buffer << "dport_map_port_" << bcm_port.logical_port() << "."
           << bcm_port.unit() << "=" << bcm_port.diag_port() << std::endl;
    // Lane remapping handling.
    if (bcm_port.tx_lane_map() > 0) {
      buffer << "xgxs_tx_lane_map_xe" << bcm_port.diag_port() << "."
             << bcm_port.unit() << "=0x" << std::hex << std::uppercase
             << bcm_port.tx_lane_map() << std::dec << std::nouppercase
             << std::endl;
    }
    if (bcm_port.rx_lane_map() > 0) {
      buffer << "xgxs_rx_lane_map_xe" << bcm_port.diag_port() << "."
             << bcm_port.unit() << "=0x" << std::hex << std::uppercase
             << bcm_port.rx_lane_map() << std::dec << std::nouppercase
             << std::endl;
    }
    // XE ports polarity flip handling for RX and TX.
    if (bcm_port.tx_polarity_flip() > 0) {
      buffer << "phy_xaui_tx_polarity_flip_xe" << bcm_port.diag_port() << "."
             << bcm_port.unit() << "=0x" << std::hex << std::uppercase
             << bcm_port.tx_polarity_flip() << std::dec << std::nouppercase
             << std::endl;
    }
    if (bcm_port.rx_polarity_flip() > 0) {
      buffer << "phy_xaui_rx_polarity_flip_xe" << bcm_port.diag_port() << "."
             << bcm_port.unit() << "=0x" << std::hex << std::uppercase
             << bcm_port.rx_polarity_flip() << std::dec << std::nouppercase
             << std::endl;
    }
    // Port-level SDK properties.
    if (bcm_port.sdk_properties_size()) {
      for (const std::string& sdk_property : bcm_port.sdk_properties()) {
        buffer << sdk_property << std::endl;
      }
    }
    buffer << std::endl;
  }

  RETURN_IF_ERROR(WriteStringToFile(buffer.str(), FLAGS_bcm_sdk_config_file));

  return ::util::OkStatus();
}

void* BcmChassisManager::LinkscanEventHandlerThreadFunc(void* arg) {
  CHECK_NOTNULL(arg);
  // Retrieve arguments.
  auto* args = reinterpret_cast<ReaderArgs<LinkscanEvent>*>(arg);
  auto* manager = args->manager;
  std::unique_ptr<ChannelReader<LinkscanEvent>> reader =
      std::move(args->reader);
  delete args;
  return manager->ReadLinkscanEvents(reader);
}

void* BcmChassisManager::ReadLinkscanEvents(
    const std::unique_ptr<ChannelReader<LinkscanEvent>>& reader) {
  do {
    // Check switch shutdown.
    {
      absl::ReaderMutexLock l(&chassis_lock);
      if (shutdown) break;
    }
    LinkscanEvent event;
    // Block on the next linkscan event message from the Channel.
    int code = reader->Read(&event, absl::InfiniteDuration()).error_code();
    // Exit if the Channel is closed.
    if (code == ERR_CANCELLED) break;
    // Read should never timeout.
    if (code == ERR_ENTRY_NOT_FOUND) {
      LOG(ERROR) << "Read with infinite timeout failed with ENTRY_NOT_FOUND.";
      continue;
    }
    // Handle received message.
    LinkscanEventHandler(event.unit, event.port, event.state);
  } while (true);
  return nullptr;
}

void BcmChassisManager::LinkscanEventHandler(int unit, int logical_port,
                                             PortState new_state) {
  absl::WriterMutexLock l(&chassis_lock);
  if (shutdown) {
    VLOG(1) << "The class is already shutdown. Exiting.";
    return;
  }

  const uint64* node_id = gtl::FindOrNull(unit_to_node_id_, unit);
  const uint64* port_id =
      gtl::FindOrNull(unit_logical_port_to_port_id_, {unit, logical_port});
  if (port_id == nullptr || node_id == nullptr) {
    VLOG(1) << "Ignored unknown port with (unit, logical_port) = (" << unit
            << ", " << logical_port << "). Most probably this is a "
            << "non-configured channel of a flex port.";
    return;
  }
  const std::tuple<int, int, int>* slot_port_channel_tuple =
      gtl::FindOrNull(port_id_to_slot_port_channel_, *port_id);
  if (slot_port_channel_tuple == nullptr) {
    LOG(ERROR) << "Inconsistent state. No (slot, port, channel) for port_id "
               << *port_id << "!";
    return;
  }
  slot_port_channel_to_port_state_[*slot_port_channel_tuple] = new_state;
  const BcmPort* bcm_port = gtl::FindPtrOrNull(slot_port_channel_to_bcm_port_,
                                               *slot_port_channel_tuple);
  if (bcm_port == nullptr) {
    LOG(ERROR) << "Inconsistent state. (slot, port, channel) = ("
               << std::get<0>(*slot_port_channel_tuple) << ", "
               << std::get<1>(*slot_port_channel_tuple) << ", "
               << std::get<2>(*slot_port_channel_tuple)
               << ") is not found as key in slot_port_channel_to_bcm_port_!";
    return;
  }
  // Notify gNMI about the change of logical port state.
  SendPortOperStateGnmiEvent(*node_id, *port_id, new_state);
  LOG(INFO) << "State of SingletonPort " << PrintBcmPort(*port_id, *bcm_port)
            << ": " << PrintPortState(new_state);
}

void BcmChassisManager::SendPortOperStateGnmiEvent(uint64 node_id,
                                                   uint64 port_id,
                                                   PortState new_state) {
  absl::ReaderMutexLock l(&gnmi_event_lock_);
  if (!gnmi_event_writer_) return;
  // Allocate and initialize a PortOperStateChangedEvent event and pass it to
  // the gNMI publisher using the gNMI event notification channel.
  // The GnmiEventPtr is a smart pointer (shared_ptr<>) and it takes care of
  // the memory allocated to this event object once the event is handled by
  // the GnmiPublisher.
  if (!gnmi_event_writer_->Write(GnmiEventPtr(
          new PortOperStateChangedEvent(node_id, port_id, new_state)))) {
    // Remove WriterInterface if it is no longer operational.
    gnmi_event_writer_.reset();
  }
}

void* BcmChassisManager::TransceiverEventHandlerThreadFunc(void* arg) {
  CHECK_NOTNULL(arg);
  // Retrieve arguments.
  auto* args = reinterpret_cast<ReaderArgs<TransceiverEvent>*>(arg);
  auto* manager = args->manager;
  std::unique_ptr<ChannelReader<TransceiverEvent>> reader =
      std::move(args->reader);
  delete args;
  return manager->ReadTransceiverEvents(reader);
}

void* BcmChassisManager::ReadTransceiverEvents(
    const std::unique_ptr<ChannelReader<TransceiverEvent>>& reader) {
  do {
    // Check switch shutdown.
    {
      absl::ReaderMutexLock l(&chassis_lock);
      if (shutdown) break;
    }
    TransceiverEvent event;
    // Block on the next transceiver event message from the Channel.
    int code = reader->Read(&event, absl::InfiniteDuration()).error_code();
    // Exit if the Channel is closed.
    if (code == ERR_CANCELLED) break;
    // Read should never timeout.
    if (code == ERR_ENTRY_NOT_FOUND) {
      LOG(ERROR) << "Read with infinite timeout failed with ENTRY_NOT_FOUND.";
      continue;
    }
    // Handle received message.
    TransceiverEventHandler(event.slot, event.port, event.state);
  } while (true);
  return nullptr;
}

void BcmChassisManager::TransceiverEventHandler(int slot, int port,
                                                HwState new_state) {
  absl::WriterMutexLock l(&chassis_lock);
  if (shutdown) {
    VLOG(1) << "The class is already shutdown. Exiting.";
    return;
  }

  const std::pair<int, int>& slot_port_pair = std::make_pair(slot, port);
  // See if we know about this transceiver module. Find a mutable state pointer
  // so we can override it later.
  HwState* mutable_state =
      gtl::FindOrNull(slot_port_to_transceiver_state_, slot_port_pair);
  if (mutable_state == nullptr) {
    LOG(ERROR) << "Detected unknown (slot, port) in TransceiverEventHandler: ("
               << slot << ", " << port << "). This should not happen!";
    return;
  }
  HwState old_state = *mutable_state;

  // This handler is supposed to return present or non present for the state of
  // the transceiver modules. Other values do no make sense.
  if (new_state != HW_STATE_PRESENT && new_state != HW_STATE_NOT_PRESENT) {
    LOG(ERROR) << "Invalid state for (slot, port) = (" << slot << ", " << port
               << ") in TransceiverEventHandler: " << HwState_Name(new_state)
               << ".";
    return;
  }

  // Discard some invalid situations and report the error. Then save the new
  // state
  if (old_state == HW_STATE_READY && new_state == HW_STATE_PRESENT) {
    if (!IsInternalPort(slot_port_pair)) {
      LOG(ERROR) << "Got present for a ready (slot, port) = (" << slot << ", "
                 << port << ") in TransceiverEventHandler.";
    } else {
      VLOG(1) << "Got present for a internal (e.g. BP) (slot, port) = (" << slot
              << ", " << port << ") in TransceiverEventHandler.";
    }
    return;
  }
  if (old_state == HW_STATE_UNKNOWN && new_state == HW_STATE_NOT_PRESENT) {
    LOG(ERROR) << "Got not-present for an unknown (slot, port) = (" << slot
               << ", " << port << ") in TransceiverEventHandler.";
    return;
  }
  *mutable_state = new_state;

  // Set the port options based on new_state.
  BcmPortOptions options;
  options.set_enabled(new_state == HW_STATE_PRESENT ? TRI_STATE_TRUE
                                                    : TRI_STATE_FALSE);
  if (old_state == HW_STATE_UNKNOWN) {
    // First time we are seeing this transceiver module. Need to set the block
    // state too. Otherwise, we do not touch the blocked state.
    options.set_blocked(TRI_STATE_FALSE);
  }
  ::util::Status status = SetPortOptionsForPortGroup(slot_port_pair, options);
  if (!status.ok()) {
    LOG(ERROR) << "Failure in TransceiverEventHandler: " << status;
    return;
  }

  // Finally, before we exit we make sure if the port was HW_STATE_PRESENT,
  // it is set to HW_STATE_READY to show it has been configured and ready.
  if (*mutable_state == HW_STATE_PRESENT) {
    LOG(INFO) << "Transceiver at (slot, port) = (" << slot << ", " << port
              << ") is ready.";
    *mutable_state = HW_STATE_READY;
  }
}

::util::StatusOr<bool> BcmChassisManager::SetSpeedForFlexPortGroup(
    std::pair<int, int> slot_port_pair) const {
  // First check to see if this is a flex port group.
  const std::vector<BcmPort*>* bcm_ports =
      gtl::FindOrNull(slot_port_to_flex_bcm_ports_, slot_port_pair);
  CHECK_RETURN_IF_FALSE(bcm_ports != nullptr)
      << "Ports with (slot, port) = (" << slot_port_pair.first << ", "
      << slot_port_pair.second << ") is not a flex port.";

  // Find info on this flex port group.
  std::set<int> units_set;
  std::set<int> min_speed_logical_ports_set;
  std::set<int> config_speed_logical_ports_set;
  std::set<int> config_num_serdes_lanes_set;
  std::set<uint64> config_speed_bps_set;
  for (const auto& bcm_port : applied_bcm_chassis_map_->bcm_ports()) {
    if (bcm_port.slot() == slot_port_pair.first &&
        bcm_port.port() == slot_port_pair.second) {
      CHECK_RETURN_IF_FALSE(bcm_port.flex_port())
          << "Detected unexpected non-flex SingletonPort: "
          << PrintBcmPort(bcm_port);
      units_set.insert(bcm_port.unit());
      min_speed_logical_ports_set.insert(bcm_port.logical_port());
    }
  }
  for (const auto* bcm_port : *bcm_ports) {
    units_set.insert(bcm_port->unit());
    config_speed_logical_ports_set.insert(bcm_port->logical_port());
    config_num_serdes_lanes_set.insert(bcm_port->num_serdes_lanes());
    config_speed_bps_set.insert(bcm_port->speed_bps());
  }

  // Check to see everythin makes sense.
  CHECK_RETURN_IF_FALSE(units_set.size() == 1U)
      << "Found ports with (slot, port) = (" << slot_port_pair.first << ", "
      << slot_port_pair.second << ") are on different chips.";
  CHECK_RETURN_IF_FALSE(config_num_serdes_lanes_set.size() == 1U)
      << "Found ports with (slot, port) = (" << slot_port_pair.first << ", "
      << slot_port_pair.second << ") have different num_serdes_lanes.";
  CHECK_RETURN_IF_FALSE(config_speed_bps_set.size() == 1U)
      << "Found ports with (slot, port) = (" << slot_port_pair.first << ", "
      << slot_port_pair.second << ") have different speed_bps.";
  int unit = *units_set.begin();
  int control_logical_port = *min_speed_logical_ports_set.begin();
  int config_num_serdes_lanes = *config_num_serdes_lanes_set.begin();
  uint64 config_speed_bps = *config_speed_bps_set.begin();
  CHECK_RETURN_IF_FALSE(*config_speed_logical_ports_set.begin() ==
                        control_logical_port)
      << "Control logical port mismatch: " << control_logical_port
      << " != " << *config_speed_logical_ports_set.begin() << ".";

  // Now try to get the current speed_bps from the control port
  BcmPortOptions options;
  RETURN_IF_ERROR(
      bcm_sdk_interface_->GetPortOptions(unit, control_logical_port, &options));

  // If no change in the speed, nothing to do. Just return. There will be no
  // serdes setting either.
  if (options.speed_bps() == config_speed_bps) {
    return false;
  }

  // Now that Fist disable all the channelized ports of the min speed.
  options.Clear();
  options.set_enabled(TRI_STATE_FALSE);
  options.set_blocked(TRI_STATE_TRUE);
  for (const int logical_port : min_speed_logical_ports_set) {
    RETURN_IF_ERROR(
        bcm_sdk_interface_->SetPortOptions(unit, logical_port, options));
  }

  // Now set the number of serdes lanes just for control logical ports.
  options.Clear();
  options.set_num_serdes_lanes(config_num_serdes_lanes);
  RETURN_IF_ERROR(
      bcm_sdk_interface_->SetPortOptions(unit, control_logical_port, options));

  // Finally, set the speed_bps. Note that we do not enable/unblock the port
  // now, this will be done later in SetPortOptionsForPortGroup() called
  // in ConfigurePortGroups().
  options.Clear();
  options.set_speed_bps(config_speed_bps);
  for (const int logical_port : config_speed_logical_ports_set) {
    RETURN_IF_ERROR(
        bcm_sdk_interface_->SetPortOptions(unit, logical_port, options));
  }

  LOG(INFO) << "Successfully set speed for flex port group (slot: "
            << slot_port_pair.first << ", port: " << slot_port_pair.second
            << ") to " << config_speed_bps / kBitsPerGigabit << "G.";

  return true;
}

::util::Status BcmChassisManager::SetPortOptionsForPortGroup(
    std::pair<int, int> slot_port_pair, const BcmPortOptions& options) const {
  std::vector<BcmPort*> bcm_ports = {};
  if (slot_port_to_flex_bcm_ports_.count(slot_port_pair)) {
    bcm_ports = slot_port_to_flex_bcm_ports_.at(slot_port_pair);
  } else if (slot_port_to_non_flex_bcm_ports_.count(slot_port_pair)) {
    bcm_ports = slot_port_to_non_flex_bcm_ports_.at(slot_port_pair);
  } else {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Unknown port group (slot: " << slot_port_pair.first
           << ", port: " << slot_port_pair.second << ").";
  }

  if (options.enabled() == TRI_STATE_TRUE &&
      mode_ == OPERATION_MODE_STANDALONE) {
    // We need to configure serdes for this port now. We reach to this point
    // in the following situations:
    // 1- When push config for the first time and there are some BP ports, we
    //    immediately set the serdes settings for these ports here.
    // 2- When we receive a presence detect signal for a front panel port (
    //    after stack comes up for the first time or after transceiver modules
    //    are inserted).
    // 3- When a config push changes the speed for a flex port.
    // We first get the front panel port info from PHAL. Then using this info
    // (read and parsed from the transceiver module EEPROM) we configure serdes
    // for all BCM ports.
    FrontPanelPortInfo fp_port_info;
    RETURN_IF_ERROR(phal_interface_->GetFrontPanelPortInfo(
        slot_port_pair.first, slot_port_pair.second, &fp_port_info));
    for (const auto* bcm_port : bcm_ports) {
      // Get the serdes config from serdes db for the given BCM port.
      BcmSerdesLaneConfig bcm_serdes_lane_config;
      RETURN_IF_ERROR(bcm_serdes_db_manager_->LookupSerdesConfigForPort(
          *bcm_port, fp_port_info, &bcm_serdes_lane_config));
      // Find the map from serdes register names to their values for this BCM
      // port.
      std::map<uint32, uint32> serdes_register_configs(
          bcm_serdes_lane_config.bcm_serdes_register_configs().begin(),
          bcm_serdes_lane_config.bcm_serdes_register_configs().end());
      std::map<std::string, uint32> serdes_attr_configs(
          bcm_serdes_lane_config.bcm_serdes_attribute_configs().begin(),
          bcm_serdes_lane_config.bcm_serdes_attribute_configs().end());
      // Config serdes for this BCM port.
      RETURN_IF_ERROR(bcm_sdk_interface_->ConfigSerdesForPort(
          bcm_port->unit(), bcm_port->logical_port(), bcm_port->speed_bps(),
          bcm_port->serdes_core(), bcm_port->serdes_lane(),
          bcm_port->num_serdes_lanes(), bcm_serdes_lane_config.intf_type(),
          serdes_register_configs, serdes_attr_configs));
      // TODO: For some transceivers (e.g. 100G cSR4 QSFPs) we also
      // need to write some control values to the QSFP module control registers.
      // Take care of that part too.
      VLOG(1) << "Serdes setting done for SingletonPort "
              << PrintBcmPort(*bcm_port) << ".";
    }
  }

  // The option applies to all the ports.
  for (const auto* bcm_port : bcm_ports) {
    RETURN_IF_ERROR(bcm_sdk_interface_->SetPortOptions(
        bcm_port->unit(), bcm_port->logical_port(), options));
    VLOG(1) << "Successfully set the following options for SingletonPort "
            << PrintBcmPort(*bcm_port) << ": " << PrintBcmPortOptions(options);
  }

  return ::util::OkStatus();
}

bool BcmChassisManager::IsInternalPort(
    std::pair<int, int> slot_port_pair) const {
  // Note that we have alreay verified that all the port that are part of a
  // flex/non-flex port groups are all internal or non internal. So we need to
  // check one port only.
  const std::vector<BcmPort*>* non_flex_ports =
      gtl::FindOrNull(slot_port_to_non_flex_bcm_ports_, slot_port_pair);
  if (non_flex_ports != nullptr && !non_flex_ports->empty()) {
    return non_flex_ports->front()->internal();
  }
  const std::vector<BcmPort*>* flex_ports =
      gtl::FindOrNull(slot_port_to_flex_bcm_ports_, slot_port_pair);
  if (flex_ports != nullptr && !flex_ports->empty()) {
    return flex_ports->front()->internal();
  }
  return false;
}

}  // namespace bcm
}  // namespace hal
}  // namespace stratum