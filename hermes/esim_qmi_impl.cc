// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hermes/esim_qmi_impl.h"

#include <utility>
#include <vector>

#include <base/bind.h>

#include "hermes/qmi_constants.h"

namespace {
// This allows testing of EsimQmiImpl without actually needing to open a real
// QRTR socket to a QRTR modem.
bool CreateSocketPair(base::ScopedFD* one, base::ScopedFD* two) {
  int raw_socks[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, raw_socks) != 0) {
    PLOG(ERROR) << "Failed to create socket pair";
    return false;
  }
  one->reset(raw_socks[0]);
  two->reset(raw_socks[1]);
  return true;
}
}  // namespace

namespace hermes {

EsimQmiImpl::EsimQmiImpl(const uint8_t slot, base::ScopedFD fd)
    : slot_(slot), qrtr_socket_fd_(std::move(fd)), weak_factory_(this) {}

void EsimQmiImpl::Initialize(const DataCallback& data_callback,
                             const ErrorCallback& error_callback) {
  SendEsimMessage(
      QmiCommand::kOpenLogicalChannel, DataBlob(1, slot_),
      base::Bind(&EsimQmiImpl::OnOpenChannel, weak_factory_.GetWeakPtr(),
                 data_callback, error_callback),
      error_callback);
}

// static
std::unique_ptr<EsimQmiImpl> EsimQmiImpl::Create() {
  base::ScopedFD fd(qrtr_open(kQrtrPort));
  if (!fd.is_valid()) {
    return nullptr;
  }
  // qrtr_new_lookup(fd, service, version, instance)
  qrtr_new_lookup(fd.get(), kQrtrUimService, 1, 0);
  return std::unique_ptr<EsimQmiImpl>(
      new EsimQmiImpl(kEsimSlot, std::move(fd)));
}

// static
std::unique_ptr<EsimQmiImpl> EsimQmiImpl::CreateForTest(base::ScopedFD* sock) {
  base::ScopedFD fd;
  if (!CreateSocketPair(&fd, sock)) {
    return nullptr;
  }

  return std::unique_ptr<EsimQmiImpl>(
      new EsimQmiImpl(kEsimSlot, std::move(fd)));
}

// TODO(jruthe): pass |which| to EsimQmiImpl::SendEsimMessage to make the
// correct libqrtr call to the eSIM chip.
void EsimQmiImpl::GetInfo(int which,
                          const DataCallback& data_callback,
                          const ErrorCallback& error_callback) {
  if (!qrtr_socket_fd_.is_valid()) {
    error_callback.Run(EsimError::kEsimNotConnected);
    return;
  }

  if (which != kEsimInfo1) {
    error_callback.Run(EsimError::kEsimError);
    return;
  }

  SendEsimMessage(QmiCommand::kSendApdu, data_callback, error_callback);
}

void EsimQmiImpl::GetChallenge(const DataCallback& data_callback,
                               const ErrorCallback& error_callback) {
  if (!qrtr_socket_fd_.is_valid()) {
    error_callback.Run(EsimError::kEsimNotConnected);
    return;
  }

  SendEsimMessage(QmiCommand::kSendApdu, data_callback, error_callback);
}

// TODO(jruthe): pass |server_data| to EsimQmiImpl::SendEsimMessage to make
// correct libqrtr call to the eSIM chip.
void EsimQmiImpl::AuthenticateServer(const DataBlob& server_data,
                                     const DataCallback& data_callback,
                                     const ErrorCallback& error_callback) {
  if (!qrtr_socket_fd_.is_valid()) {
    error_callback.Run(EsimError::kEsimNotConnected);
    return;
  }

  SendEsimMessage(QmiCommand::kSendApdu, data_callback, error_callback);
}

void EsimQmiImpl::OnOpenChannel(const DataCallback& data_callback,
                                const ErrorCallback& error_callback,
                                const DataBlob& return_data) {
  // TODO(jruthe): need qmi packet parsing
  data_callback.Run(return_data);
}

void EsimQmiImpl::SendEsimMessage(const QmiCommand command,
                                  const DataBlob& data,
                                  const DataCallback& data_callback,
                                  const ErrorCallback& error_callback) const {
  DataBlob result_code_tlv;
  switch (command) {
    case QmiCommand::kOpenLogicalChannel:
      // std::vector<uint8_t> slot_tlv = {0x01, 0x01, 0x00, 0x01};
      // TODO(jruthe): insert actual PostTask for QMI call here to open logical
      // channel and populate result_code_tlv with return data from
      // SEND_APDU_IND QMI callback
      //
      result_code_tlv = {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
      data_callback.Run(result_code_tlv);
      break;
    case QmiCommand::kLogicalChannel:
      // TODO(jruthe) insert PostTask for closing logical channel
      result_code_tlv = {0x00};
      data_callback.Run(result_code_tlv);
      break;
    case QmiCommand::kSendApdu:
      // TODO(jruthe): implement some logic to construct different APDUs.

      // TODO(jruthe): insert actual PostTask for SEND_APDU QMI call
      result_code_tlv = {0x00};
      data_callback.Run(result_code_tlv);
      break;
  }
}

void EsimQmiImpl::SendEsimMessage(const QmiCommand command,
                                  const DataCallback& data_callback,
                                  const ErrorCallback& error_callback) const {
  SendEsimMessage(command, DataBlob(), data_callback, error_callback);
}

}  // namespace hermes
