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


#include "third_party/stratum/hal/lib/bcm/bcm_switch.h"

#include <utility>

#include "third_party/stratum/glue/status/canonical_errors.h"
#include "third_party/stratum/glue/status/status_test_util.h"
#include "third_party/stratum/hal/lib/bcm/bcm_chassis_manager_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_node_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_packetio_manager_mock.h"
#include "third_party/stratum/hal/lib/common/gnmi_events.h"
#include "third_party/stratum/hal/lib/common/phal_mock.h"
#include "third_party/stratum/hal/lib/common/writer_mock.h"
#include "third_party/stratum/hal/lib/p4/p4_table_mapper_mock.h"
#include "third_party/stratum/lib/channel/channel_mock.h"
#include "third_party/stratum/lib/utils.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::Sequence;
using ::testing::WithArg;
using ::testing::WithArgs;

namespace stratum {
namespace hal {
namespace bcm {

namespace {

MATCHER_P(EqualsProto, proto, "") { return ProtoEqual(arg, proto); }

MATCHER_P(EqualsStatus, status, "") {
  return arg.error_code() == status.error_code() &&
         arg.error_message() == status.error_message();
}

MATCHER_P(DerivedFromStatus, status, "") {
  if (arg.error_code() != status.error_code()) {
    return false;
  }
  if (arg.error_message().find(status.error_message()) == std::string::npos) {
    *result_listener << "\nOriginal error string: \"" << status.error_message()
                     << "\" is missing from the actual status.";
    return false;
  }
  return true;
}

constexpr uint64 kNodeId = 13579;
constexpr int kUnit = 2;
constexpr char kErrorMsg[] = "Test error message";

const std::map<uint64, int>& NodeIdToUnitMap() {
  static auto* map = new std::map<uint64, int>({{kNodeId, kUnit}});
  return *map;
}

class BcmSwitchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    phal_mock_ = absl::make_unique<PhalMock>();
    bcm_chassis_manager_mock_ = absl::make_unique<BcmChassisManagerMock>();
    bcm_node_mock_ = absl::make_unique<BcmNodeMock>();
    unit_to_bcm_node_mock_[kUnit] = bcm_node_mock_.get();
    bcm_switch_ = BcmSwitch::CreateInstance(phal_mock_.get(),
                                            bcm_chassis_manager_mock_.get(),
                                            unit_to_bcm_node_mock_);
    shutdown = false;  // global variable initialization

    ON_CALL(*bcm_chassis_manager_mock_, GetNodeIdToUnitMap())
        .WillByDefault(Return(NodeIdToUnitMap()));
  }

  void TearDown() override { unit_to_bcm_node_mock_.clear(); }

  void PushChassisConfigSuccess() {
    ChassisConfig config;
    config.add_nodes()->set_id(kNodeId);
    {
      InSequence sequence;  // The order of the calls are important. Enforce it.
      EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_chassis_manager_mock_,
                  VerifyChassisConfig(EqualsProto(config)))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_node_mock_,
                  VerifyChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*phal_mock_, PushChassisConfig(EqualsProto(config)))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_chassis_manager_mock_,
                  PushChassisConfig(EqualsProto(config)))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_node_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
    }
    EXPECT_OK(bcm_switch_->PushChassisConfig(config));
  }

  ::util::Status DefaultError() {
    return ::util::Status(StratumErrorSpace(), ERR_UNKNOWN, kErrorMsg);
  }

  std::unique_ptr<PhalMock> phal_mock_;
  std::unique_ptr<BcmChassisManagerMock> bcm_chassis_manager_mock_;
  std::unique_ptr<BcmNodeMock> bcm_node_mock_;
  std::map<int, BcmNode*> unit_to_bcm_node_mock_;
  std::unique_ptr<BcmSwitch> bcm_switch_;
};

TEST_F(BcmSwitchTest, PushChassisConfigSuccess) { PushChassisConfigSuccess(); }

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenPhalVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenChassisManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenNodeVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenPhalPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*phal_mock_, PushChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenChassisManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*phal_mock_, PushChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              PushChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, PushChassisConfigFailureWhenNodePushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*phal_mock_, PushChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              PushChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_, PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_switch_->PushChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, VerifyChassisConfigSuccess) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  {
    InSequence sequence;  // The order of the calls are important. Enforce it.
    EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_chassis_manager_mock_,
                VerifyChassisConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_node_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_switch_->VerifyChassisConfig(config));
}

TEST_F(BcmSwitchTest, VerifyChassisConfigFailureWhenPhalVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_switch_->VerifyChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, VerifyChassisConfigFailureWhenChassisManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_switch_->VerifyChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, VerifyChassisConfigFailureWhenNodeVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_switch_->VerifyChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest,
       VerifyChassisConfigFailureWhenMoreThanOneManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*phal_mock_, VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_,
              VerifyChassisConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_node_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::Status(StratumErrorSpace(), ERR_INVALID_PARAM,
                                      "some other text")));

  // we keep the error code from the first error
  EXPECT_THAT(bcm_switch_->VerifyChassisConfig(config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, ShutdownSuccess) {
  EXPECT_CALL(*bcm_node_mock_, Shutdown()).WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_, Shutdown())
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*phal_mock_, Shutdown()).WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_switch_->Shutdown());
}

TEST_F(BcmSwitchTest, ShutdownFailureWhenSomeManagerShutdownFails) {
  EXPECT_CALL(*bcm_node_mock_, Shutdown()).WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_chassis_manager_mock_, Shutdown())
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*phal_mock_, Shutdown()).WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_switch_->Shutdown(), DerivedFromStatus(DefaultError()));
}

// PushForwardingPipelineConfig() should verify and propagate the config.
TEST_F(BcmSwitchTest, PushForwardingPipelineConfigSuccess) {
  PushChassisConfigSuccess();

  ::p4::ForwardingPipelineConfig config;
  {
    InSequence sequence;
    // Verify should always be called before push.
    EXPECT_CALL(*bcm_node_mock_,
                VerifyForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_node_mock_,
                PushForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_switch_->PushForwardingPipelineConfig(kNodeId, config));
}

// When BcmSwitchTest fails to verify a forwarding config during
// PushForwardingPipelineConfig(), it should not propagate the config and fail.
TEST_F(BcmSwitchTest, PushForwardingPipelineConfigFailureWhenVerifyFails) {
  PushChassisConfigSuccess();

  ::p4::ForwardingPipelineConfig config;
  EXPECT_CALL(*bcm_node_mock_,
              VerifyForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_node_mock_, PushForwardingPipelineConfig(_)).Times(0);
  EXPECT_THAT(bcm_switch_->PushForwardingPipelineConfig(kNodeId, config),
              DerivedFromStatus(DefaultError()));
}

// When BcmSwitchTest fails to push a forwarding config during
// PushForwardingPipelineConfig(), it should fail immediately.
TEST_F(BcmSwitchTest, PushForwardingPipelineConfigFailureWhenPushFails) {
  PushChassisConfigSuccess();

  ::p4::ForwardingPipelineConfig config;
  EXPECT_CALL(*bcm_node_mock_,
              VerifyForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_node_mock_,
              PushForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()));
  EXPECT_THAT(bcm_switch_->PushForwardingPipelineConfig(kNodeId, config),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, VerifyForwardingPipelineConfigSuccess) {
  PushChassisConfigSuccess();

  ::p4::ForwardingPipelineConfig config;
  {
    InSequence sequence;
    // Verify should always be called before push.
    EXPECT_CALL(*bcm_node_mock_,
                VerifyForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_switch_->VerifyForwardingPipelineConfig(kNodeId, config));
}

// Test registration of a writer for sending gNMI events.
TEST_F(BcmSwitchTest, RegisterEventNotifyWriterTest) {
  auto writer = std::shared_ptr<WriterInterface<GnmiEventPtr>>(
      new WriterMock<GnmiEventPtr>());

  EXPECT_CALL(*bcm_chassis_manager_mock_, RegisterEventNotifyWriter(writer))
      .WillOnce(Return(::util::OkStatus()))
      .WillOnce(Return(DefaultError()));

  // Successful BcmChassisManager registration.
  EXPECT_OK(bcm_switch_->RegisterEventNotifyWriter(writer));
  // Failed BcmChassisManager registration.
  EXPECT_THAT(bcm_switch_->RegisterEventNotifyWriter(writer),
              DerivedFromStatus(DefaultError()));
}

TEST_F(BcmSwitchTest, GetMemoryErrorAlarmStatePass) {
  WriterMock<DataResponse> writer;
  DataResponse resp;
  // Mock implementation of Write() that saves the response to local variable.
  EXPECT_CALL(writer, Write(_))
      .WillOnce(DoAll(WithArg<0>(Invoke([&resp](DataResponse r) {
                        // Copy the response.
                        resp = r;
                      })),
                      Return(true)));

  DataRequest req;
  *req.add_request()->mutable_memory_error_alarm() =
      DataRequest::SingleFieldRequest::FromChassis();
  std::vector<::util::Status> details;
  EXPECT_OK(bcm_switch_->RetrieveValue(
      /* node_id */ 0, req, &writer, &details));
  EXPECT_TRUE(resp.has_memory_error_alarm());
  ASSERT_EQ(details.size(), 1);
  EXPECT_THAT(details.at(0), ::util::OkStatus());
}

TEST_F(BcmSwitchTest, GetFlowProgrammingExceptionAlarmStatePass) {
  WriterMock<DataResponse> writer;
  DataResponse resp;
  // Mock implementation of Write() that saves the response to local variable.
  EXPECT_CALL(writer, Write(_))
      .WillOnce(DoAll(WithArg<0>(Invoke([&resp](DataResponse r) {
                        // Copy the response.
                        resp = r;
                      })),
                      Return(true)));

  DataRequest req;
  *req.add_request()->mutable_flow_programming_exception_alarm() =
      DataRequest::SingleFieldRequest::FromChassis();
  std::vector<::util::Status> details;
  EXPECT_OK(bcm_switch_->RetrieveValue(
      /* node_id */ 0, req, &writer, &details));
  EXPECT_TRUE(resp.has_flow_programming_exception_alarm());
  ASSERT_EQ(details.size(), 1);
  EXPECT_THAT(details.at(0), ::util::OkStatus());
}

TEST_F(BcmSwitchTest, GetpQosQueueCountersPass) {
  WriterMock<DataResponse> writer;
  DataResponse resp;
  // Mock implementation of Write() that saves the response to local variable.
  EXPECT_CALL(writer, Write(_))
      .WillOnce(DoAll(WithArg<0>(Invoke([&resp](DataResponse r) {
                        // Copy the response.
                        resp = r;
                      })),
                      Return(true)));

  DataRequest req;
  auto* request = req.add_request()->mutable_port_qos_counters();
  request->set_node_id(1);
  request->set_port_id(2);
  request->set_queue_id(4);

  std::vector<::util::Status> details;
  EXPECT_OK(bcm_switch_->RetrieveValue(
      /* node_id */ 0, req, &writer, &details));
  EXPECT_TRUE(resp.has_port_qos_counters());
  ASSERT_EQ(details.size(), 1);
  EXPECT_THAT(details.at(0), ::util::OkStatus());
}

// TODO: Complete unit test coverage.

}  // namespace
}  // namespace bcm
}  // namespace hal
}  // namespace stratum