// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is the boilerplate implementation of the IAllocator HAL interface,
// generated by the hidl-gen tool and then modified for use on Chrome OS.
// Modifications include:
// - Removal of non boiler plate client and server related code.
// - Reformatting to meet the Chrome OS coding standards.
//
// Originally generated with the command:
// $ hidl-gen -o output -L c++ -r android.hardware:hardware/interfaces \
//   android.hardware.neuralnetworks@1.3

#define LOG_TAG "android.hardware.neuralnetworks@1.3::PreparedModel"

#include <android/hardware/neuralnetworks/1.3/IPreparedModel.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/Status.h>

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace V1_3 {

const char* IPreparedModel::descriptor(
    "android.hardware.neuralnetworks@1.3::IPreparedModel");

::android::hardware::Return<void> IPreparedModel::interfaceChain(
    interfaceChain_cb _hidl_cb) {
  _hidl_cb({
      ::android::hardware::neuralnetworks::V1_3::IPreparedModel::descriptor,
      ::android::hardware::neuralnetworks::V1_2::IPreparedModel::descriptor,
      ::android::hardware::neuralnetworks::V1_0::IPreparedModel::descriptor,
      ::android::hidl::base::V1_0::IBase::descriptor,
  });
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::debug(
    const ::android::hardware::hidl_handle& fd,
    const ::android::hardware::hidl_vec<::android::hardware::hidl_string>&
        options) {
  (void)fd;
  (void)options;
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::interfaceDescriptor(
    interfaceDescriptor_cb _hidl_cb) {
  _hidl_cb(
      ::android::hardware::neuralnetworks::V1_3::IPreparedModel::descriptor);
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::getHashChain(
    getHashChain_cb _hidl_cb) {
  _hidl_cb({
        (uint8_t[32]){238, 157, 195, 75, 153, 37, 184, 54, 123, 17, 17, 199, 43,
        214, 217, 211, 117, 67, 39, 53, 228, 81, 87, 44, 165, 166, 101, 216, 81,
        106, 119, 68}
        /* ee9dc34b9925b8367b1111c72bd6d9d375432735e451572ca5a665d8516a7744 */,
        (uint8_t[32]){64, 231, 28, 214, 147, 222, 91, 131, 35, 37, 197, 216,
        240, 129, 242, 255, 32, 167, 186, 43, 137, 212, 1, 206, 229, 180, 179,
        235, 14, 36, 22, 129}
        /* 40e71cd693de5b832325c5d8f081f2ff20a7ba2b89d401cee5b4b3eb0e241681 */,
        (uint8_t[32]){235, 47, 160, 200, 131, 194, 24, 93, 81, 75, 224, 184,
        76, 23, 155, 40, 55, 83, 239, 12, 27, 119, 180, 91, 79, 53, 155, 210,
        59, 186, 139, 117}
        /* eb2fa0c883c2185d514be0b84c179b283753ef0c1b77b45b4f359bd23bba8b75 */,
        (uint8_t[32]){236, 127, 215, 158, 208, 45, 250, 133, 188, 73, 148, 38,
        173, 174, 62, 190, 35, 239, 5, 36, 243, 205, 105, 87, 19, 147, 36, 184,
        59, 24, 202, 76}
        /* ec7fd79ed02dfa85bc499426adae3ebe23ef0524f3cd6957139324b83b18ca4c */
        });
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::setHALInstrumentation() {
  return ::android::hardware::Void();
}

::android::hardware::Return<bool> IPreparedModel::linkToDeath(
    const ::android::sp<::android::hardware::hidl_death_recipient>& recipient,
    uint64_t cookie) {
  (void)cookie;
  return (recipient != nullptr);
}

::android::hardware::Return<void> IPreparedModel::ping() {
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::getDebugInfo(
    getDebugInfo_cb _hidl_cb) {
  ::android::hidl::base::V1_0::DebugInfo info = {};
  info.pid = -1;
  info.ptr = 0;
  info.arch =
#if defined(__LP64__)
      ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_64BIT;
#else
      ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_32BIT;
#endif
  _hidl_cb(info);
  return ::android::hardware::Void();
}

::android::hardware::Return<void> IPreparedModel::notifySyspropsChanged() {
  ::android::report_sysprop_change();
  return ::android::hardware::Void();
}

::android::hardware::Return<bool> IPreparedModel::unlinkToDeath(
    const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) {
  return (recipient != nullptr);
}

::android::hardware::Return<
    ::android::sp<::android::hardware::neuralnetworks::V1_3::IPreparedModel>>
IPreparedModel::castFrom(
    const ::android::sp<
        ::android::hardware::neuralnetworks::V1_0::IPreparedModel>& parent,
    bool emitError) {
  return ::android::hardware::details::castInterface<
      IPreparedModel,
      ::android::hardware::neuralnetworks::V1_0::IPreparedModel>(
      parent, "android.hardware.neuralnetworks@1.3::IPreparedModel", emitError);
}

::android::hardware::Return<
    ::android::sp<::android::hardware::neuralnetworks::V1_3::IPreparedModel>>
IPreparedModel::castFrom(
    const ::android::sp<::android::hidl::base::V1_0::IBase>& parent,
    bool emitError) {
  return ::android::hardware::details::castInterface<
      IPreparedModel, ::android::hidl::base::V1_0::IBase>(
      parent, "android.hardware.neuralnetworks@1.3::IPreparedModel", emitError);
}

}  // namespace V1_3
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
