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


// This file contains P4ConfigVerifier's implementation.

#include "third_party/stratum/hal/lib/p4/p4_config_verifier.h"

#include "base/commandlineflags.h"
#include "third_party/stratum/hal/lib/p4/p4_write_request_differ.h"
#include "third_party/stratum/lib/macros.h"
#include "third_party/stratum/public/lib/error.h"
#include "third_party/stratum/public/proto/p4_annotation.pb.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/substitute.h"
#include "third_party/sandblaze/p4lang/p4/p4runtime.pb.h"
#include "util/gtl/map_util.h"

// These flags control the strictness of error reporting for certain
// anomalies in the pipeline config.  Each flag has one of the following values:
//  "error" - treat the condition as an error that causes the verify to fail.
//  "warn" - log the condition as a warning, but do not fail to verify.
//  "vlog" - report the condition if VLOG(1) is enabled; verify succeeds.
// Any other flag value causes P4ConfigVerifier to silently ignore the
// condition.  The flags are intended to set the error strictness according to
// the environment in which the P4ConfigVerifier is running.  The "error" level
// is appropriate for all conditions that indicate an inconsistency in the
// P4PipelineConfig that prohibits successful execution in the Hercules switch
// stack.  The "warn" level is appropriate for conditions that need to be
// addressed before production, but which do not block ongoing Hercules
// development.  The "vlog" level's typical use is to suppress the warning
// level messages in some environments.  For example, "warn" may be the
// choice for unit tests and presubmits, but "vlog" will suppress log spam
// for those errors on the switch.  The default values are currently set for
// the needs of the Hercules switch stack environment.

DEFINE_string(match_field_error_level, "vlog", "Controls errors for table "
              "match fields that do not have a known field descriptor type");
DEFINE_string(action_field_error_level, "vlog", "Controls errors for action "
              "references to header fields without a known field descriptor "
              "type");

namespace stratum {
namespace hal {

std::unique_ptr<P4ConfigVerifier> P4ConfigVerifier::CreateInstance(
    const ::p4::config::P4Info& p4_info,
    const P4PipelineConfig& p4_pipeline_config) {
  return absl::WrapUnique(new P4ConfigVerifier(p4_info, p4_pipeline_config));
}

::util::Status P4ConfigVerifier::Verify() {
  // If the P4PipelineConfig is empty, further verification is pointless.
  if (p4_pipeline_config_.table_map().empty()) {
    ::util::Status status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4 table map is missing object mapping descriptors";
    return status;
  }

  ::util::Status verify_status = ::util::OkStatus();
  for (const auto& p4_table : p4_info_.tables()) {
    ::util::Status table_status = VerifyTable(p4_table);
    APPEND_STATUS_IF_ERROR(verify_status, table_status);
  }

  for (const auto& p4_action : p4_info_.actions()) {
    ::util::Status action_status = VerifyAction(p4_action);
    APPEND_STATUS_IF_ERROR(verify_status, action_status);
  }

  for (const auto& static_entry :
       p4_pipeline_config_.static_table_entries().updates()) {
    ::util::Status entry_status = VerifyStaticTableEntry(static_entry);
    APPEND_STATUS_IF_ERROR(verify_status, entry_status);
  }

  return verify_status;
}

::util::Status P4ConfigVerifier::VerifyAndCompare(
    const ::p4::config::P4Info& old_p4_info,
    const P4PipelineConfig& old_p4_pipeline_config) {
  RETURN_IF_ERROR(Verify());

  // VerifyAndCompare accepts unchanged static entries or addition of new
  // static entries.  Static entry deletions and modifications require reboot.
  p4::WriteRequest delete_request;
  p4::WriteRequest modify_request;
  P4WriteRequestDiffer static_entry_differ(
      old_p4_pipeline_config.static_table_entries(),
      p4_pipeline_config_.static_table_entries());
  RETURN_IF_ERROR(static_entry_differ.Compare(
      &delete_request, nullptr, &modify_request, nullptr));
  ::util::Status status = ::util::OkStatus();
  if (delete_request.updates_size()) {
    ::util::Status static_delete_status =
        MAKE_ERROR(ERR_REBOOT_REQUIRED)
        << "P4PipelineConfig has " << delete_request.updates_size()
        << " static table entry deletions that require a reboot: "
        << delete_request.ShortDebugString();
    APPEND_STATUS_IF_ERROR(status, static_delete_status);
  }
  if (modify_request.updates_size()) {
    ::util::Status static_modify_status =
        MAKE_ERROR(ERR_REBOOT_REQUIRED)
        << "P4PipelineConfig has " << modify_request.updates_size()
        << " static table entry modifications that require a reboot: "
        << modify_request.ShortDebugString();
    APPEND_STATUS_IF_ERROR(status, static_modify_status);
  }

  // TODO: Add comparisons for more reboot-required deltas.

  return status;
}

::util::Status P4ConfigVerifier::VerifyTable(
    const ::p4::config::Table& p4_table) {
  ::util::Status table_status = ::util::OkStatus();

  // Every P4 table needs a p4_pipeline_config_ table descriptor.
  const std::string& table_name = p4_table.preamble().name();
  auto* map_value =
      gtl::FindOrNull(p4_pipeline_config_.table_map(), table_name);
  if (map_value != nullptr) {
    if (map_value->has_table_descriptor()) {
      // The pipeline stage must be known for all tables.
      if (map_value->table_descriptor().pipeline_stage() ==
          P4Annotation::DEFAULT_STAGE) {
        ::util::Status bad_stage_status =
            MAKE_ERROR(ERR_INTERNAL)
            << "P4PipelineConfig table map descriptor for P4 table "
            << table_name << " does not specify a pipeline stage";
        APPEND_STATUS_IF_ERROR(table_status, bad_stage_status);
      }
    } else {
      ::util::Status bad_descriptor_status =
          MAKE_ERROR(ERR_INTERNAL)
          << "P4PipelineConfig table map descriptor for P4 table " << table_name
          << " is not a table descriptor: " << map_value->ShortDebugString();
      APPEND_STATUS_IF_ERROR(table_status, bad_descriptor_status);
    }
  } else {
    ::util::Status no_descriptor_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig table map has no descriptor for P4 table "
        << table_name;
    APPEND_STATUS_IF_ERROR(table_status, no_descriptor_status);
  }

  // All of the table's match fields need to be verified.
  for (const auto& match_field : p4_table.match_fields()) {
    ::util::Status field_status = VerifyMatchField(match_field, table_name);
    APPEND_STATUS_IF_ERROR(table_status, field_status);
  }

  VLOG(1) << "P4 table " << table_name << " verification "
          << (table_status.ok() ? "succeeds" : "fails");

  return table_status;
}

::util::Status P4ConfigVerifier::VerifyAction(
    const ::p4::config::Action& p4_action) {
  ::util::Status action_status = ::util::OkStatus();

  // Every P4 action needs a valid p4_pipeline_config_ action descriptor.
  const std::string& action_name = p4_action.preamble().name();
  auto* map_value =
      gtl::FindOrNull(p4_pipeline_config_.table_map(), action_name);
  if (map_value != nullptr) {
    if (map_value->has_action_descriptor()) {
      const auto& action_descriptor = map_value->action_descriptor();
      for (const auto& assignment : action_descriptor.assignments()) {
        ::util::Status assign_status =
            VerifyActionInstructions(assignment, action_name);
        APPEND_STATUS_IF_ERROR(action_status, assign_status);
      }
    } else {
      ::util::Status bad_descriptor_status =
          MAKE_ERROR(ERR_INTERNAL)
          << "P4PipelineConfig table map descriptor for P4 action "
          << action_name << " is not an action descriptor: "
          << map_value->ShortDebugString();
      APPEND_STATUS_IF_ERROR(action_status, bad_descriptor_status);
    }
  } else {
    ::util::Status no_descriptor_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig table map has no descriptor for P4 action "
        << action_name;
    APPEND_STATUS_IF_ERROR(action_status, no_descriptor_status);
  }

  VLOG(1) << "P4 action " << action_name << " verification "
          << (action_status.ok() ? "succeeds" : "fails");

  return action_status;
}

::util::Status P4ConfigVerifier::VerifyStaticTableEntry(
    const p4::Update& static_entry) {
  ::util::Status entry_status = ::util::OkStatus();
  if (static_entry.type() != p4::Update::INSERT) {
    ::util::Status bad_type_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig static table entry has unexpected type: "
        << static_entry.ShortDebugString();
    APPEND_STATUS_IF_ERROR(entry_status, bad_type_status);
  }

  if (!static_entry.entity().has_table_entry()) {
    ::util::Status no_table_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig static table entry entity has no TableEntry: "
        << static_entry.ShortDebugString();
    APPEND_STATUS_IF_ERROR(entry_status, no_table_status);
    return entry_status;  // Nothing more to do if TableEntry is missing.
  }

  const p4::TableEntry& table_entry = static_entry.entity().table_entry();
  bool table_found = false;
  for (const auto& p4_table : p4_info_.tables()) {
    if (table_entry.table_id() == p4_table.preamble().id()) {
      table_found = true;

      // Although table_entry.match_size() == 0 is generally valid to update
      // the table's default action, that should not be happening with static
      // table entries.
      if (table_entry.match_size() != p4_table.match_fields_size()) {
        ::util::Status match_size_status =
            MAKE_ERROR(ERR_INTERNAL)
            << "P4PipelineConfig static table entry has "
            << table_entry.match_size() << " match fields.  P4Info expects "
            << p4_table.match_fields_size() << " match fields: "
            << table_entry.ShortDebugString();
        APPEND_STATUS_IF_ERROR(entry_status, match_size_status);
      }
      // TODO: More things that could be verified:
      //  1) The field IDs in table_entry.match() could be checked.
      //  2) The table_entry.action() value can be verified.
      // Since both of these items have many possible valid combinations, the
      // easiest way to do them both would be to create a new
      // P4PerDeviceTableManager and call MapFlowEntry to see if it succeeds.
      // Since P4PerDeviceTableManager uses a P4ConfigVerifier to assist with
      // VerifyForwardingPipelineConfig, any attempt to verify with MapFlowEntry
      // needs to be careful to avoid potential infinite recursion.
      break;
    }
  }

  if (!table_found) {
    ::util::Status no_table_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig static table entry table_id is not in P4Info: "
        << table_entry.ShortDebugString();
    APPEND_STATUS_IF_ERROR(entry_status, no_table_status);
  }

  return entry_status;
}

::util::Status P4ConfigVerifier::VerifyMatchField(
    const ::p4::config::MatchField& match_field,
    const std::string& table_name) {
  // Every P4 table match_field needs a p4_pipeline_config_ field descriptor.
  const std::string& field_name = match_field.name();
  auto field_descriptor_status = GetFieldDescriptor(field_name, table_name);
  RETURN_IF_ERROR(field_descriptor_status.status());

  // The field descriptor should contain a known field type.
  auto field_descriptor = field_descriptor_status.ValueOrDie();
  if (!VerifyKnownFieldType(*field_descriptor)) {
    const std::string message = absl::Substitute(
        "P4 match field $0 in table $1 has an unspecified field type",
        field_name.c_str(), table_name.c_str());
    RETURN_IF_ERROR(FilterError(message, FLAGS_match_field_error_level));
  }

  // The field's match type should have a corresponding field descriptor
  // conversion.
  if (match_field.match_type() != ::p4::config::MatchField::UNSPECIFIED) {
    bool match_ok = false;
    for (const auto& conversion : field_descriptor->valid_conversions()) {
      if (match_field.match_type() == conversion.match_type()) {
        match_ok = true;
        break;
      }
    }
    if (!match_ok) {
      return MAKE_ERROR(ERR_INTERNAL)
             << "P4PipelineConfig descriptor for match field " << field_name
             << " in P4 table " << table_name << " has no conversion entry for"
             << " match type "
             << ::p4::config::MatchField_MatchType_Name(
                 match_field.match_type());
    }
  }

  return ::util::OkStatus();
}

::util::Status P4ConfigVerifier::VerifyActionInstructions(
    const P4ActionDescriptor::P4ActionInstructions& instructions,
    const std::string& action_name) {
  ::util::Status action_status = ::util::OkStatus();

  // Instructions with assignments to headers and header fields are verified.
  // Simple action primitives are ignored.
  for (const auto& field_name : instructions.destination_field_names()) {
    if (instructions.assigned_value().source_value_case() ==
        P4AssignSourceValue::kSourceHeaderName) {
      auto header_assign_status = VerifyHeaderAssignment();
      APPEND_STATUS_IF_ERROR(action_status, header_assign_status);
      continue;
    }
    auto field_assign_status = VerifyFieldAssignment(
        field_name, instructions.assigned_value(), action_name);
    APPEND_STATUS_IF_ERROR(action_status, field_assign_status);
  }

  return action_status;
}

::util::Status P4ConfigVerifier::VerifyHeaderAssignment() {
  // TODO:  Implement this function.  The table map now has header
  // descriptors, so one should exist for the source and the destination.
  return ::util::OkStatus();
}

::util::Status P4ConfigVerifier::VerifyFieldAssignment(
    const std::string& destination_field,
    const P4AssignSourceValue& source_value, const std::string& action_name) {
  ::util::Status assignment_status = ::util::OkStatus();

  // The destination field should always have a p4_pipeline_config_ field
  // descriptor.  The descriptor does not need a known field type.  Destination
  // fields are sometimes unused in the P4 program, so field types only need
  // to be enforced when a field is used on the right side of an assignment
  // or as a match key.
  auto destination_field_status =
      GetFieldDescriptor(destination_field, action_name);
  APPEND_STATUS_IF_ERROR(assignment_status, destination_field_status.status());

  // When the assignment source is another field, the field descriptor must
  // exist, and it must contain a known field type.  Constants and action
  // parameters do not need any extra verification when used as an assignment
  // source.
  // TODO: One possible exception is "local_metadata.l3_class_id",
  // which is assigned in a P4 action but never referenced elsewhere in the
  // P4 program.  It is, however, needed by the switch stack, so a way is
  // needed to handle metadata that communicates data to the switch stack
  // vs. metadata that is simply unused in a particular role.
  if (source_value.source_value_case() !=
      P4AssignSourceValue::kSourceFieldName) {
    return assignment_status;
  }

  // Source header fields must always refer to a valid field descriptor
  // with a known field type.
  const std::string& source_field = source_value.source_field_name();
  auto source_field_status = GetFieldDescriptor(source_field, action_name);
  if (source_field_status.ok()) {
    auto field_descriptor = source_field_status.ValueOrDie();
    if (!VerifyKnownFieldType(*field_descriptor)) {
      const std::string message = absl::Substitute(
        "P4 field $0 in action $1 has an unspecified field type",
        source_field.c_str(), action_name.c_str());
      APPEND_STATUS_IF_ERROR(assignment_status, FilterError(
          message, FLAGS_action_field_error_level));
    }
  } else {
    APPEND_STATUS_IF_ERROR(assignment_status, source_field_status.status());
  }

  return assignment_status;
}

bool P4ConfigVerifier::VerifyKnownFieldType(
    const P4FieldDescriptor& descriptor) {
  return !(descriptor.type() == P4_FIELD_TYPE_UNKNOWN ||
           descriptor.type() == P4_FIELD_TYPE_ANNOTATED);
}

::util::StatusOr<const P4FieldDescriptor*> P4ConfigVerifier::GetFieldDescriptor(
    const std::string& field_name, const std::string& log_object) {
  auto* map_value =
      gtl::FindOrNull(p4_pipeline_config_.table_map(), field_name);
  if (map_value != nullptr) {
    if (!map_value->has_field_descriptor()) {
      ::util::Status bad_descriptor_status =
          MAKE_ERROR(ERR_INTERNAL)
          << "P4PipelineConfig descriptor for field " << field_name
          << " referenced by P4 object " << log_object
          << " is not a field descriptor: " << map_value->ShortDebugString();
      return bad_descriptor_status;
    }
  } else {
    ::util::Status no_descriptor_status =
        MAKE_ERROR(ERR_INTERNAL)
        << "P4PipelineConfig table map has no descriptor for field "
        << field_name << " referenced by P4 object " << log_object;
    return no_descriptor_status;
  }

  return &map_value->field_descriptor();
}

::util::Status P4ConfigVerifier::FilterError(
    const std::string& message, const std::string& filter_level) {
  if (filter_level == "error") {
    return MAKE_ERROR(ERR_INTERNAL) << message;
  } else if (filter_level == "warn") {
    LOG(WARNING) << message;
  } else if (filter_level == "vlog") {
    VLOG(1) << message;
  }
  return ::util::OkStatus();
}

}  // namespace hal
}  // namespace stratum