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


#include "third_party/stratum/hal/lib/bcm/bcm_node.h"

#include "third_party/stratum/glue/status/canonical_errors.h"
#include "third_party/stratum/glue/status/status_test_util.h"
#include "third_party/stratum/hal/lib/bcm/bcm_acl_manager_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_l2_manager_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_l3_manager_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_packetio_manager_mock.h"
#include "third_party/stratum/hal/lib/bcm/bcm_table_manager_mock.h"
#include "third_party/stratum/hal/lib/common/writer_mock.h"
#include "third_party/stratum/hal/lib/p4/p4_table_mapper_mock.h"
#include "third_party/stratum/lib/utils.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/synchronization/mutex.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArgs;

namespace stratum {
namespace hal {
namespace bcm {

MATCHER_P(EqualsProto, proto, "") { return ProtoEqual(arg, proto); }

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

class BcmNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bcm_acl_manager_mock_ = absl::make_unique<BcmAclManagerMock>();
    bcm_l2_manager_mock_ = absl::make_unique<BcmL2ManagerMock>();
    bcm_l3_manager_mock_ = absl::make_unique<BcmL3ManagerMock>();
    bcm_packetio_manager_mock_ = absl::make_unique<BcmPacketioManagerMock>();
    bcm_table_manager_mock_ = absl::make_unique<BcmTableManagerMock>();
    p4_table_mapper_mock_ = absl::make_unique<P4TableMapperMock>();
    bcm_node_ = BcmNode::CreateInstance(
        bcm_acl_manager_mock_.get(), bcm_l2_manager_mock_.get(),
        bcm_l3_manager_mock_.get(), bcm_packetio_manager_mock_.get(),
        bcm_table_manager_mock_.get(), p4_table_mapper_mock_.get(), kUnit);
  }

  void PushChassisConfigWithCheck() {
    ChassisConfig config;
    config.add_nodes()->set_id(kNodeId);
    {
      InSequence sequence;  // The order of the calls are important. Enforce it.
      EXPECT_CALL(*p4_table_mapper_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_table_manager_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_l2_manager_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_l3_manager_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_acl_manager_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
      EXPECT_CALL(*bcm_packetio_manager_mock_,
                  PushChassisConfig(EqualsProto(config), kNodeId))
          .WillOnce(Return(::util::OkStatus()));
    }
    ASSERT_OK(bcm_node_->PushChassisConfig(config, kNodeId));
    ASSERT_TRUE(IsInitialized());
  }

  bool IsInitialized() {
    absl::WriterMutexLock l(&bcm_node_->lock_);
    return bcm_node_->initialized_;
  }

  ::util::Status DefaultError() {
    return ::util::Status(StratumErrorSpace(), ERR_UNKNOWN, kErrorMsg);
  }

  static constexpr uint64 kNodeId = 13579;
  static constexpr int kUnit = 2;
  static constexpr char kErrorMsg[] = "Test error message";
  static constexpr uint32 kMemberId = 841;
  static constexpr uint32 kGroupId = 111;
  static constexpr int kEgressIntfId = 10001;

  std::unique_ptr<BcmAclManagerMock> bcm_acl_manager_mock_;
  std::unique_ptr<BcmL2ManagerMock> bcm_l2_manager_mock_;
  std::unique_ptr<BcmL3ManagerMock> bcm_l3_manager_mock_;
  std::unique_ptr<BcmPacketioManagerMock> bcm_packetio_manager_mock_;
  std::unique_ptr<BcmTableManagerMock> bcm_table_manager_mock_;
  std::unique_ptr<P4TableMapperMock> p4_table_mapper_mock_;
  std::unique_ptr<BcmNode> bcm_node_;
};

constexpr uint64 BcmNodeTest::kNodeId;
constexpr int BcmNodeTest::kUnit;
constexpr char BcmNodeTest::kErrorMsg[];
constexpr uint32 BcmNodeTest::kMemberId;
constexpr uint32 BcmNodeTest::kGroupId;
constexpr int BcmNodeTest::kEgressIntfId;

TEST_F(BcmNodeTest, PushChassisConfigSuccess) { PushChassisConfigWithCheck(); }

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenTableMapperPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenTableManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenL2ManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenL3ManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenAclManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, PushChassisConfigFailureWhenPacketioManagerPushFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              PushChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->PushChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigSuccess) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  {
    InSequence sequence;  // The order of the calls are important. Enforce it.
    EXPECT_CALL(*p4_table_mapper_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_table_manager_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_l2_manager_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_l3_manager_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_acl_manager_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_packetio_manager_mock_,
                VerifyChassisConfig(EqualsProto(config), kNodeId))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_node_->VerifyChassisConfig(config, kNodeId));
  EXPECT_FALSE(IsInitialized());  // Should be false even of verify passes
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenTableMapperVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenTableManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenL2ManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenL3ManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenAclManagerrVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenPacketioManagerVerifyFails) {
  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureWhenMultiManagerVerifyFails) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId))
      .WillOnce(Return(
          ::util::Status(StratumErrorSpace(), ERR_INTERNAL, kErrorMsg)));

  EXPECT_THAT(bcm_node_->VerifyChassisConfig(config, kNodeId),
              DerivedFromStatus(DefaultError()));
  EXPECT_TRUE(IsInitialized());  // Initialized as we pushed config before
}

TEST_F(BcmNodeTest, VerifyChassisConfigFailureForInvalidNodeId) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), 0))
      .WillOnce(Return(::util::OkStatus()));

  ::util::Status status = bcm_node_->VerifyChassisConfig(config, 0);
  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status.error_message(), HasSubstr("Invalid node ID"));
  EXPECT_EQ(ERR_INVALID_PARAM, status.error_code());
  EXPECT_TRUE(IsInitialized());  // Initialized as we pushed config before
}

TEST_F(BcmNodeTest, VerifyChassisConfigReportsRebootRequired) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ChassisConfig config;
  config.add_nodes()->set_id(kNodeId);
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l2_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              VerifyChassisConfig(EqualsProto(config), kNodeId + 1))
      .WillOnce(Return(::util::OkStatus()));

  ::util::Status status = bcm_node_->VerifyChassisConfig(config, kNodeId + 1);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(ERR_REBOOT_REQUIRED, status.error_code());
}

TEST_F(BcmNodeTest, ShutdownSuccess) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  {
    InSequence sequence;  // The order of the calls are important. Enforce it.
    EXPECT_CALL(*bcm_packetio_manager_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_acl_manager_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_l3_manager_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_l2_manager_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_table_manager_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*p4_table_mapper_mock_, Shutdown())
        .WillOnce(Return(::util::OkStatus()));
  }

  EXPECT_OK(bcm_node_->Shutdown());
  EXPECT_FALSE(IsInitialized());
}

TEST_F(BcmNodeTest, ShutdownFailureWhenSomeManagerShutdownFails) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  EXPECT_CALL(*bcm_packetio_manager_mock_, Shutdown())
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_, Shutdown())
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_l3_manager_mock_, Shutdown())
      .WillOnce(Return(DefaultError()));
  EXPECT_CALL(*bcm_l2_manager_mock_, Shutdown())
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_, Shutdown())
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*p4_table_mapper_mock_, Shutdown())
      .WillOnce(Return(DefaultError()));

  EXPECT_THAT(bcm_node_->Shutdown(), DerivedFromStatus(DefaultError()));
}

// PushForwardingPipelineConfig() should verify and propagate the config.
TEST_F(BcmNodeTest, PushForwardingPipelineConfigSuccess) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::ForwardingPipelineConfig config;
  {
    InSequence sequence;
    // P4TableMapper should check for static entry pre-push before other pushes.
    EXPECT_CALL(*p4_table_mapper_mock_, HandlePrePushStaticEntryChanges(_, _))
        .WillOnce(Return(::util::OkStatus()));
    // P4TableMapper should always be setup before flow managers.
    EXPECT_CALL(*p4_table_mapper_mock_,
                PushForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_acl_manager_mock_,
                PushForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    // P4TableMapper should check for static entry post-push after other pushes.
    EXPECT_CALL(*p4_table_mapper_mock_, HandlePostPushStaticEntryChanges(_, _))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_node_->PushForwardingPipelineConfig(config));
}

// PushForwardingPipelineConfig() should fail immediately on any push failures.
TEST_F(BcmNodeTest, PushForwardingPipelineConfigFailueOnAnyManagerPushFailure) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::ForwardingPipelineConfig config;
  // Order matters here as if an earlier push fails, following pushes must not
  // be attempted.
  EXPECT_CALL(*p4_table_mapper_mock_, HandlePrePushStaticEntryChanges(_, _))
      .WillOnce(Return(DefaultError()))
      .WillRepeatedly(Return(::util::OkStatus()));
  EXPECT_CALL(*p4_table_mapper_mock_,
              PushForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()))
      .WillRepeatedly(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              PushForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()))
      .WillRepeatedly(Return(::util::OkStatus()));
  EXPECT_CALL(*p4_table_mapper_mock_, HandlePostPushStaticEntryChanges(_, _))
      .WillOnce(Return(DefaultError()))
      .WillRepeatedly(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->PushForwardingPipelineConfig(config),
              DerivedFromStatus(DefaultError()));
  EXPECT_THAT(bcm_node_->PushForwardingPipelineConfig(config),
              DerivedFromStatus(DefaultError()));
  EXPECT_THAT(bcm_node_->PushForwardingPipelineConfig(config),
              DerivedFromStatus(DefaultError()));
  EXPECT_THAT(bcm_node_->PushForwardingPipelineConfig(config),
              DerivedFromStatus(DefaultError()));
}

// VerifyForwardingPipelineConfig() should verify the config.
TEST_F(BcmNodeTest, VerifyForwardingPipelineConfigSuccess) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::ForwardingPipelineConfig config;
  {
    InSequence sequence;
    EXPECT_CALL(*p4_table_mapper_mock_,
                VerifyForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
    EXPECT_CALL(*bcm_acl_manager_mock_,
                VerifyForwardingPipelineConfig(EqualsProto(config)))
        .WillOnce(Return(::util::OkStatus()));
  }
  EXPECT_OK(bcm_node_->VerifyForwardingPipelineConfig(config));
}

// VerifyForwardingPipelineConfig() should fail immediately on any verify
// failures.
TEST_F(BcmNodeTest,
       VerifyForwardingPipelineConfigFailueOnAnyManagerVerifyFailure) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::ForwardingPipelineConfig config;
  EXPECT_CALL(*p4_table_mapper_mock_,
              VerifyForwardingPipelineConfig(EqualsProto(config)))
      .WillOnce(Return(DefaultError()))
      .WillRepeatedly(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_acl_manager_mock_,
              VerifyForwardingPipelineConfig(EqualsProto(config)))
      .WillRepeatedly(Return(::util::OkStatus()));

  EXPECT_THAT(bcm_node_->VerifyForwardingPipelineConfig(config),
              DerivedFromStatus(DefaultError()));
  EXPECT_OK(bcm_node_->VerifyForwardingPipelineConfig(config));
  EXPECT_OK(bcm_node_->VerifyForwardingPipelineConfig(config));
}

namespace {

::p4::TableEntry* SetupTableEntryToInsert(::p4::WriteRequest* req,
                                          uint64 node_id) {
  req->set_device_id(node_id);
  auto* update = req->add_updates();
  update->set_type(::p4::Update::INSERT);
  auto* entity = update->mutable_entity();
  return entity->mutable_table_entry();
}

::p4::TableEntry* SetupTableEntryToModify(::p4::WriteRequest* req,
                                          uint64 node_id) {
  req->set_device_id(node_id);
  auto* update = req->add_updates();
  update->set_type(::p4::Update::MODIFY);
  auto* entity = update->mutable_entity();
  return entity->mutable_table_entry();
}

::p4::TableEntry* SetupTableEntryToDelete(::p4::WriteRequest* req,
                                          uint64 node_id) {
  req->set_device_id(node_id);
  auto* update = req->add_updates();
  update->set_type(::p4::Update::DELETE);
  auto* entity = update->mutable_entity();
  return entity->mutable_table_entry();
}

}  // namespace

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_Ipv4Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV4_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, InsertLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_Ipv4Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV4_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, InsertLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_Ipv6Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV6_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, InsertLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_Ipv6Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV6_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, InsertLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_L2Multicat) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_L2_MULTICAST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l2_manager_mock_, InsertMulticastGroup(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_MyStation) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_MY_STATION);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l2_manager_mock_, InsertMyStationEntry(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertTableEntry_Acl) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToInsert(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::INSERT, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_ACL);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_acl_manager_mock_, InsertTableEntry(_))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyTableEntry_Ipv4Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToModify(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::MODIFY, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV4_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, ModifyLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              UpdateTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyTableEntry_Ipv4Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToModify(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::MODIFY, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV4_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, ModifyLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              UpdateTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyTableEntry_Ipv6Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToModify(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::MODIFY, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV6_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, ModifyLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              UpdateTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyTableEntry_Ipv6Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToModify(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::MODIFY, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV6_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, ModifyLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              UpdateTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyTableEntry_Acl) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToModify(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::MODIFY, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_ACL);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_acl_manager_mock_, ModifyTableEntry(_))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_Ipv4Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV4_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_Ipv4Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV4_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_Ipv6Lpm) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_IPV6_LPM);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_Ipv6Host) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_IPV6_HOST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteLpmOrHostFlow(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest,
       WriteForwardingEntriesSuccess_DeleteTableEntry_L2Multicast) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_L2_MULTICAST);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l2_manager_mock_, DeleteMulticastGroup(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_MyStation) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(
                            BcmFlowEntry::BCM_TABLE_MY_STATION);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l2_manager_mock_, DeleteMyStationEntry(_))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteTableEntry(EqualsProto(*table_entry)))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteTableEntry_Acl) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  auto* table_entry = SetupTableEntryToDelete(&req, kNodeId);

  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmFlowEntry(EqualsProto(*table_entry),
                               ::p4::Update::DELETE, _))
      .WillOnce(DoAll(WithArgs<2>(Invoke([](BcmFlowEntry* x) {
                        x->set_bcm_table_type(BcmFlowEntry::BCM_TABLE_ACL);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_acl_manager_mock_, DeleteTableEntry(_))
      .WillOnce(Return(::util::OkStatus()));

  std::vector<::util::Status> results = {};
  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertActionProfileMember) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::INSERT);
  auto* entity = update->mutable_entity();
  auto* member = entity->mutable_action_profile_member();
  member->set_member_id(kMemberId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_, ActionProfileMemberExists(kMemberId))
      .WillOnce(Return(false));
  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmNonMultipathNexthop(EqualsProto(*member), _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmNonMultipathNexthop* x) {
                        x->set_type(BcmNonMultipathNexthop::NEXTHOP_TYPE_PORT);
                        x->set_unit(kUnit);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, FindOrCreateNonMultipathNexthop(_))
      .WillOnce(Return(kEgressIntfId));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddActionProfileMember(EqualsProto(*member),
                                     BcmNonMultipathNexthop::NEXTHOP_TYPE_PORT,
                                     kEgressIntfId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyActionProfileMember) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::MODIFY);
  auto* entity = update->mutable_entity();
  auto* member = entity->mutable_action_profile_member();
  member->set_member_id(kMemberId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_,
              GetBcmNonMultipathNexthopInfo(kMemberId, _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmNonMultipathNexthopInfo* x) {
                        x->egress_intf_id = kEgressIntfId;
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmNonMultipathNexthop(EqualsProto(*member), _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmNonMultipathNexthop* x) {
                        x->set_type(BcmNonMultipathNexthop::NEXTHOP_TYPE_PORT);
                        x->set_unit(kUnit);
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_,
              ModifyNonMultipathNexthop(kEgressIntfId, _))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(
      *bcm_table_manager_mock_,
      UpdateActionProfileMember(EqualsProto(*member),
                                BcmNonMultipathNexthop::NEXTHOP_TYPE_PORT))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteActionProfileMember) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::DELETE);
  auto* entity = update->mutable_entity();
  auto* member = entity->mutable_action_profile_member();
  member->set_member_id(kMemberId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_,
              GetBcmNonMultipathNexthopInfo(kMemberId, _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmNonMultipathNexthopInfo* x) {
                        x->egress_intf_id = kEgressIntfId;
                        x->group_ref_count = 0;
                        x->flow_ref_count = 0;
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteNonMultipathNexthop(kEgressIntfId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteActionProfileMember(EqualsProto(*member)))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_InsertActionProfileGroup) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::INSERT);
  auto* entity = update->mutable_entity();
  auto* group = entity->mutable_action_profile_group();
  group->set_group_id(kGroupId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_, ActionProfileGroupExists(kGroupId))
      .WillOnce(Return(false));
  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmMultipathNexthop(EqualsProto(*group), _))
      .WillOnce(DoAll(WithArgs<1>(Invoke(
                          [](BcmMultipathNexthop* x) { x->set_unit(kUnit); })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, FindOrCreateMultipathNexthop(_))
      .WillOnce(Return(kEgressIntfId));
  EXPECT_CALL(*bcm_table_manager_mock_,
              AddActionProfileGroup(EqualsProto(*group), kEgressIntfId))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_ModifyActionProfileGroup) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::MODIFY);
  auto* entity = update->mutable_entity();
  auto* group = entity->mutable_action_profile_group();
  group->set_group_id(kGroupId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_, GetBcmMultipathNexthopInfo(kGroupId, _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmMultipathNexthopInfo* x) {
                        x->egress_intf_id = kEgressIntfId;
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_table_manager_mock_,
              FillBcmMultipathNexthop(EqualsProto(*group), _))
      .WillOnce(DoAll(WithArgs<1>(Invoke(
                          [](BcmMultipathNexthop* x) { x->set_unit(kUnit); })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, ModifyMultipathNexthop(kEgressIntfId, _))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              UpdateActionProfileGroup(EqualsProto(*group)))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

TEST_F(BcmNodeTest, WriteForwardingEntriesSuccess_DeleteActionProfileGroup) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  ::p4::WriteRequest req;
  req.set_device_id(kNodeId);
  auto* update = req.add_updates();
  update->set_type(::p4::Update::DELETE);
  auto* entity = update->mutable_entity();
  auto* group = entity->mutable_action_profile_group();
  group->set_group_id(kGroupId);
  std::vector<::util::Status> results = {};

  EXPECT_CALL(*bcm_table_manager_mock_, GetBcmMultipathNexthopInfo(kGroupId, _))
      .WillOnce(DoAll(WithArgs<1>(Invoke([](BcmMultipathNexthopInfo* x) {
                        x->egress_intf_id = kEgressIntfId;
                        x->flow_ref_count = 0;
                      })),
                      Return(::util::OkStatus())));
  EXPECT_CALL(*bcm_l3_manager_mock_, DeleteMultipathNexthop(kEgressIntfId))
      .WillOnce(Return(::util::OkStatus()));
  EXPECT_CALL(*bcm_table_manager_mock_,
              DeleteActionProfileGroup(EqualsProto(*group)))
      .WillOnce(Return(::util::OkStatus()));

  EXPECT_OK(bcm_node_->WriteForwardingEntries(req, &results));
  EXPECT_EQ(1U, results.size());
}

// RegisterPacketReceiveWriter() should forward the call to BcmPacketioManager
// and return success or error based on the returned result.
TEST_F(BcmNodeTest, RegisterPacketReceiveWriter) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  auto writer = std::make_shared<WriterMock<::p4::PacketIn>>();
  EXPECT_CALL(*bcm_packetio_manager_mock_,
              RegisterPacketReceiveWriter(
                  GoogleConfig::BCM_KNET_INTF_PURPOSE_CONTROLLER, Eq(writer)))
      .WillOnce(Return(::util::OkStatus()))
      .WillOnce(Return(DefaultError()));

  EXPECT_OK(bcm_node_->RegisterPacketReceiveWriter(writer));
  EXPECT_THAT(bcm_node_->RegisterPacketReceiveWriter(writer),
              DerivedFromStatus(DefaultError()));
}

// UnregisterPacketReceiveWriter() should forward the call to BcmPacketioManager
// and return success or error based on the returned result.
TEST_F(BcmNodeTest, UnregisterPacketReceiveWriter) {
  ASSERT_NO_FATAL_FAILURE(PushChassisConfigWithCheck());

  EXPECT_CALL(*bcm_packetio_manager_mock_,
              UnregisterPacketReceiveWriter(
                  GoogleConfig::BCM_KNET_INTF_PURPOSE_CONTROLLER))
      .WillOnce(Return(::util::OkStatus()))
      .WillOnce(Return(DefaultError()));

  EXPECT_OK(bcm_node_->UnregisterPacketReceiveWriter());
  EXPECT_THAT(bcm_node_->UnregisterPacketReceiveWriter(),
              DerivedFromStatus(DefaultError()));
}

// TODO: Complete unit test coverage.

}  // namespace bcm
}  // namespace hal
}  // namespace stratum