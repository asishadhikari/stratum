// Copyright 2019 Google LLC
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

// This is a .p4 file that ControlInspectorTest uses to verify table apply
// decoding with hit/miss conditions.
#include <core.p4>
#include <v1model.p4>

#include "control_stubs.p4"
#include "simple_headers.p4"

#define COLOR_GREEN  1

parser ParserImpl(packet_in packet, out headers hdr, inout test_metadata_t meta,
                  inout standard_metadata_t standard_metadata) {
  state start {
    transition accept;
  }
}

// The egress control contains an apply with a hit condition.
control egress(inout headers hdr, inout test_metadata_t meta,
               inout standard_metadata_t standard_metadata) {
  action nop() {
  }
  table test_table {
    actions = {
      nop;
    }
    key = {
      hdr.ethernet.dstAddr: exact;
      hdr.ethernet.etherType: exact;
    }
    default_action = nop();
  }
  apply {
    if (test_table.apply().hit) {
      meta.color = COLOR_GREEN;
    }
  }
}

// The ingress control contains an apply with a miss (!hit) condition.
control ingress(inout headers hdr, inout test_metadata_t meta,
                inout standard_metadata_t standard_metadata) {
  action nop() {
  }
  table test_table {
    actions = {
      nop;
    }
    key = {
      hdr.ethernet.dstAddr: exact;
      hdr.ethernet.etherType: exact;
    }
    default_action = nop();
  }
  apply {
    if (!test_table.apply().hit) {
      mark_to_drop(standard_metadata);
    }
  }
}

V1Switch(ParserImpl(), verify_checksum_stub(), ingress(), egress(),
         compute_checksum_stub(), deparser_stub()) main;
