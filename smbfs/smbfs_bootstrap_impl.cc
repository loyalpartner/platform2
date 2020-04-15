// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "smbfs/smbfs_bootstrap_impl.h"

#include <utility>

#include <base/bind.h>
#include <base/logging.h>
#include <mojo/public/cpp/system/platform_handle.h>

#include "smbfs/smb_credential.h"
#include "smbfs/smb_filesystem.h"

namespace smbfs {
namespace {

mojom::MountError ConnectErrorToMountError(SmbFilesystem::ConnectError error) {
  switch (error) {
    case SmbFilesystem::ConnectError::kNotFound:
      return mojom::MountError::kNotFound;
    case SmbFilesystem::ConnectError::kAccessDenied:
      return mojom::MountError::kAccessDenied;
    case SmbFilesystem::ConnectError::kSmb1Unsupported:
      return mojom::MountError::kInvalidProtocol;
    default:
      return mojom::MountError::kUnknown;
  }
}

}  // namespace

SmbFsBootstrapImpl::SmbFsBootstrapImpl(
    mojom::SmbFsBootstrapRequest request,
    SmbFilesystemFactory smb_filesystem_factory,
    Delegate* delegate)
    : binding_(this, std::move(request)),
      smb_filesystem_factory_(smb_filesystem_factory),
      delegate_(delegate) {
  DCHECK(smb_filesystem_factory_);
  DCHECK(delegate_);
  binding_.set_connection_error_handler(base::Bind(
      &SmbFsBootstrapImpl::OnMojoConnectionError, base::Unretained(this)));
}

SmbFsBootstrapImpl::~SmbFsBootstrapImpl() = default;

void SmbFsBootstrapImpl::Start(BootstrapCompleteCallback callback) {
  DCHECK(!completion_callback_);
  completion_callback_ = std::move(callback);
}

void SmbFsBootstrapImpl::MountShare(mojom::MountOptionsPtr options,
                                    mojom::SmbFsDelegatePtr smbfs_delegate,
                                    MountShareCallback callback) {
  if (!completion_callback_) {
    LOG(ERROR) << "Mojo bootstrap not active";
    std::move(callback).Run(mojom::MountError::kUnknown, nullptr);
    return;
  }

  if (options->share_path.find("smb://") != 0) {
    // TODO(amistry): More extensive URL validation.
    LOG(ERROR) << "Invalid share path: " << options->share_path;
    std::move(callback).Run(mojom::MountError::kInvalidUrl, nullptr);
    return;
  }

  std::unique_ptr<SmbCredential> credential = std::make_unique<SmbCredential>(
      options->workgroup, options->username, nullptr);
  if (options->kerberos_config) {
    delegate_->SetupKerberos(
        std::move(options->kerberos_config),
        base::BindOnce(&SmbFsBootstrapImpl::OnCredentialsSetup,
                       base::Unretained(this), std::move(options),
                       std::move(smbfs_delegate), std::move(callback),
                       std::move(credential), true /* use_kerberos */));
    return;
  }

  if (options->password) {
    credential->password = std::move(options->password.value());
  }

  OnCredentialsSetup(std::move(options), std::move(smbfs_delegate),
                     std::move(callback), std::move(credential),
                     false /* use_kerberos */, true /* setup_success */);
}

void SmbFsBootstrapImpl::OnCredentialsSetup(
    mojom::MountOptionsPtr options,
    mojom::SmbFsDelegatePtr smbfs_delegate,
    MountShareCallback callback,
    std::unique_ptr<SmbCredential> credential,
    bool use_kerberos,
    bool setup_success) {
  if (!setup_success) {
    std::move(callback).Run(mojom::MountError::kUnknown, nullptr);
    return;
  }

  SmbFilesystem::Options smb_options;
  smb_options.share_path = options->share_path;
  smb_options.credentials = std::move(credential);
  smb_options.allow_ntlm = options->allow_ntlm;
  auto fs = smb_filesystem_factory_.Run(std::move(smb_options));
  // Don't use the resolved address if Kerberos is set up. Kerberos requires the
  // full hostname to obtain auth tickets.
  if (options->resolved_host && !use_kerberos) {
    if (options->resolved_host->address_bytes.size() != 4) {
      LOG(ERROR) << "Invalid IP address size: "
                 << options->resolved_host->address_bytes.size();
      std::move(callback).Run(mojom::MountError::kInvalidOptions, nullptr);
      return;
    }
    fs->SetResolvedAddress(options->resolved_host->address_bytes);
  }
  if (!options->skip_connect) {
    SmbFilesystem::ConnectError error = fs->EnsureConnected();
    if (error != SmbFilesystem::ConnectError::kOk) {
      LOG(ERROR) << "Unable to connect to SMB share " << options->share_path
                 << ": " << error;
      std::move(callback).Run(ConnectErrorToMountError(error), nullptr);
      return;
    }
  }

  mojom::SmbFsPtr smbfs_ptr;
  std::move(completion_callback_)
      .Run(std::move(fs), mojo::MakeRequest(&smbfs_ptr),
           std::move(smbfs_delegate));

  std::move(callback).Run(mojom::MountError::kOk, std::move(smbfs_ptr));
}

void SmbFsBootstrapImpl::OnMojoConnectionError() {
  if (completion_callback_) {
    std::move(completion_callback_).Run(nullptr, nullptr, nullptr);
  }
}

}  // namespace smbfs
