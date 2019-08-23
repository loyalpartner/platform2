// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_MOBILE_OPERATOR_DB_TEST_PROTOS_INIT_TEST_OVERRIDE_DB_INIT_1_H_
#define SHILL_MOBILE_OPERATOR_DB_TEST_PROTOS_INIT_TEST_OVERRIDE_DB_INIT_1_H_

#ifndef IN_MOBILE_OPERATOR_INFO_UNITTEST_CC
#error "Must be included only from mobile_operator_info_test.cc."
#endif

// Following is the binary protobuf for the following (text representation)
// protobuf:
// mno {
//   data {
//     uuid : "hahaha"
//     country : "us"
//     localized_name {
//       name : "Faulker"
//     }
//     mccmnc : "00"
//     mccmnc : "01"
//     mobile_apn {
//       apn : "zeros_default"
//       localized_name {
//         name : "Def0"
//       }
//     }
//     mobile_apn {
//       apn : "onesies_default"
//       localized_name {
//         name : "Def1"
//       }
//     }
//     sid : "0000"
//   }
// }
// The binary data for the protobuf in this file was generated by writing the
// prototxt file init_test_override_db_init_1.prototxt and then:
//   protoc --proto_path .. --encode "shill.mobile_operator_db.MobileOperatorDB"
//     ../mobile_operator_db.proto < init_test_override_db_init_1.prototxt
//     > init_test_override_db_init_1.h.pbf
//   cat init_test_override_db_init_1.h.pbf | xxd -i

namespace shill {
namespace mobile_operator_db {
static const unsigned char init_test_override_db_init_1[]{
    0x0a, 0x60, 0x0a, 0x5e, 0x0a, 0x06, 0x68, 0x61, 0x68, 0x61, 0x68,
    0x61, 0x1a, 0x02, 0x75, 0x73, 0x22, 0x09, 0x0a, 0x07, 0x46, 0x61,
    0x75, 0x6c, 0x6b, 0x65, 0x72, 0xaa, 0x01, 0x02, 0x30, 0x30, 0xaa,
    0x01, 0x02, 0x30, 0x31, 0xb2, 0x01, 0x17, 0x0a, 0x0d, 0x7a, 0x65,
    0x72, 0x6f, 0x73, 0x5f, 0x64, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74,
    0x1a, 0x06, 0x0a, 0x04, 0x44, 0x65, 0x66, 0x30, 0xb2, 0x01, 0x19,
    0x0a, 0x0f, 0x6f, 0x6e, 0x65, 0x73, 0x69, 0x65, 0x73, 0x5f, 0x64,
    0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0x1a, 0x06, 0x0a, 0x04, 0x44,
    0x65, 0x66, 0x31, 0xca, 0x02, 0x04, 0x30, 0x30, 0x30, 0x30};
}  // namespace mobile_operator_db
}  // namespace shill

#endif  // SHILL_MOBILE_OPERATOR_DB_TEST_PROTOS_INIT_TEST_OVERRIDE_DB_INIT_1_H_
