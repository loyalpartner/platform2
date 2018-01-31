// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AUTHPOLICY_SAMBA_INTERFACE_H_
#define AUTHPOLICY_SAMBA_INTERFACE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/macros.h>
#include <base/memory/ref_counted.h>
#include <dbus/authpolicy/dbus-constants.h>

#include "authpolicy/authpolicy_flags.h"
#include "authpolicy/authpolicy_metrics.h"
#include "authpolicy/constants.h"
#include "authpolicy/jail_helper.h"
#include "authpolicy/path_service.h"
#include "authpolicy/proto_bindings/active_directory_info.pb.h"
#include "authpolicy/samba_helper.h"
#include "authpolicy/tgt_manager.h"
#include "authpolicy/windows_policy_manager.h"
#include "bindings/authpolicy_containers.pb.h"

// Helper methods for Samba Active Directory authentication, machine (device)
// joining and policy fetching. Note: "Device" and "machine" can be used
// interchangably here.

namespace enterprise_management {
class ChromeDeviceSettingsProto;
}

namespace policy {
class DevicePolicyImpl;
}

namespace authpolicy {

class Anonymizer;
class AuthPolicyMetrics;
class PathService;
class ProcessExecutor;

class SambaInterface {
 public:
  SambaInterface(scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                 AuthPolicyMetrics* metrics,
                 const PathService* path_service,
                 const base::Closure& user_kerberos_files_changed);

  ~SambaInterface();

  // Creates directories required by Samba code. If |expect_config| is true,
  // loads configuration and device policy and initializes dependent stuff like
  // |encryption_types_|.
  // Returns an error
  // - if a directory failed to create or
  // - if |expect_config| is true and the config file fails to load.
  ErrorType Initialize(bool expect_config);

  // Cleans all persistent state files. Returns true if all files were cleared.
  static bool CleanState(const PathService* path_service);

  // Calls kinit to get a Kerberos ticket-granting-ticket (TGT) for the given
  // |user_principal_name| (format: user_name@workgroup.domain). If a TGT
  // already exists, it is renewed. The password must be readable from the pipe
  // referenced by the file descriptor |password_fd|. On success, the user's
  // account information is returned in |account_info|. If |account_id| is
  // non-empty, the |account_info| is queried by |account_id| instead of by
  // user name. This is safer since the account id is invariant, whereas the
  // user name can change. The updated user name (or rather the sAMAccountName)
  // is returned in the |account_info|. Thus, |account_id| should be set if
  // known and left empty if unknown.
  ErrorType AuthenticateUser(const std::string& user_principal_name,
                             const std::string& account_id,
                             int password_fd,
                             ActiveDirectoryAccountInfo* account_info);

  // Retrieves the status of the user account given by |account_id| (aka
  // objectGUID). |user_principal_name| is used to derive the user's realm.
  // The returned |user_status| contains general ActiveDirectoryAccountInfo as
  // well as the status of the user's ticket-granting-ticket (TGT). Does not
  // fill |user_status| on error.
  ErrorType GetUserStatus(const std::string& user_principal_name,
                          const std::string& account_id,
                          ActiveDirectoryUserStatus* user_status);

  // Gets the user Kerberos credential cache (krb5cc) and configuration
  // (krb5.conf) files if they exist. Does not set |files| on error.
  ErrorType GetUserKerberosFiles(const std::string& account_id,
                                 KerberosFiles* files);

  // Joins the local device with name |machine_name| to an Active Directory
  // domain. The credentials for joining (usually admin level) are given by
  // |user_principal_name| and |password_fd|, see AuthenticateUser() for
  // details. |machine_domain| is the domain where the machine is joined to. If
  // empty, it is derived from |user_principal_name|. |machine_ou| is a vector
  // of organizational units where the machine is placed into, ordered
  // leaf-to-root. If empty, the machine is placed in the default location (e.g.
  // Computers OU). |encryption_types| specifies the allowed encryption types
  // for Kerberos authentication. On success, |joined_domain| is set to the
  // domain that was joined.
  ErrorType JoinMachine(const std::string& machine_name,
                        const std::string& machine_domain,
                        const std::vector<std::string>& machine_ou,
                        const std::string& user_principal_name,
                        KerberosEncryptionTypes encryption_types,
                        int password_fd,
                        std::string* joined_domain);

  // Downloads user and extension policy from the Active Directory server and
  // stores it in |gpo_policy_data|. |account_id| is the unique user objectGUID
  // returned from |AuthenticateUser| in |account_info|. The user's Kerberos
  // authentication ticket must still be valid. If this operation fails, call
  // |AuthenticateUser| and try again.
  ErrorType FetchUserGpos(const std::string& account_id,
                          protos::GpoPolicyData* gpo_policy_data);

  // Downloads device and extension policy from the Active Directory server and
  // stores it in |gpo_policy_data|. The device must be joined to the Active
  // Directory domain already (see |JoinMachine|). During join, a machine
  // password is stored in a keytab file, which is used for authentication for
  // policy fetch.
  ErrorType FetchDeviceGpos(protos::GpoPolicyData* gpo_policy_data);

  // Sets the default log level, see AuthPolicyFlags::DefaultLevel for details.
  // The level persists between restarts of authpolicyd, but gets reset on
  // reboot.
  void SetDefaultLogLevel(AuthPolicyFlags::DefaultLevel level);

  // Returns the user's principal name (sAMAccountName @ realm).
  std::string GetUserPrincipal() const { return user_account_.GetPrincipal(); }

  const std::string& user_account_id() const { return user_account_id_; }

  const std::string& machine_name() const {
    return device_account_.netbios_name;
  }

  // Disable retry sleep for unit tests.
  void DisableRetrySleepForTesting() {
    smbclient_retry_sleep_enabled_ = false;
    device_tgt_manager_.DisableRetrySleepForTesting();
  }

  // Returns the anonymizer.
  const Anonymizer* GetAnonymizerForTesting() const {
    return anonymizer_.get();
  }

  // Returns the Windows policy manager.
  WindowsPolicyManager& GetWindowsPolicyManagerForTesting() {
    return windows_policy_manager_;
  }

  // Renew the user ticket-granting-ticket.
  ErrorType RenewUserTgtForTesting() { return user_tgt_manager_.RenewTgt(); }

  // Returns the ticket-granting-ticket manager for the user account.
  TgtManager& GetUserTgtManagerForTesting() { return user_tgt_manager_; }

  // Sets the container used to load device policy during Initialize(). Can be
  // used to load device policy from a different location and without key check.
  void SetDevicePolicyImplForTesting(
      std::unique_ptr<policy::DevicePolicyImpl> policy_impl);

  // Resets internal state (useful for doing multiple domain joins).
  void ResetForTesting() { Reset(); }

 private:
  // User or device specific information. The user might be logging on to a
  // different realm than the machine was joined to.
  struct AccountData {
    std::string realm;         // Active Directory realm.
    std::string workgroup;     // Active Directory workgroup name.
    std::string netbios_name;  // Netbios name is empty for user.
    std::string kdc_ip;        // IPv4/IPv6 address of key distribution center.
    std::string dc_name;       // DNS name of the domain controller
    std::string user_name;     // User sAMAccountName or device netbios_name+$.
    Path smb_conf_path;        // Path of the Samba configuration file.

    explicit AccountData(Path _smb_conf_path) : smb_conf_path(_smb_conf_path) {}

    // Returns user_name @ realm.
    std::string GetPrincipal() const { return user_name + "@" + realm; }
  };

  // Actual implementation of AuthenticateUser() (see above). The method is
  // wrapped in order to catch and memorize the returned error.
  ErrorType AuthenticateUserInternal(const std::string& user_principal_name,
                                     const std::string& account_id,
                                     int password_fd,
                                     ActiveDirectoryAccountInfo* account_info);

  // Gets the status of the user's ticket-granting-ticket (TGT). Uses klist
  // internally to check whether the ticket is valid, expired or not present.
  // Does not perform any server-side checks.
  ErrorType GetUserTgtStatus(ActiveDirectoryUserStatus::TgtStatus* tgt_status);

  // Determines the password status by comparing the old |user_pwd_last_set_|
  // timestamp to the new timestamp in |account_info|.
  ActiveDirectoryUserStatus::PasswordStatus GetUserPasswordStatus(
      const ActiveDirectoryAccountInfo& account_info);

  // Writes the Samba configuration file using the given |account|.
  ErrorType WriteSmbConf(const AccountData& account) const;

  // Queries the name of the workgroup for the given |account| and stores it in
  // |account|->workgroup.
  ErrorType UpdateWorkgroup(AccountData* account);

  // Queries the IP of the key distribution center (KDC) for the given |account|
  // and stores it in |account|->kdc_ip. The KDC address is required to speed up
  // network communication and to get rid of waiting for the machine account
  // propagation after Active Directory domain join.
  ErrorType UpdateKdcIp(AccountData* account) const;

  // Queries the DNS domain name of the domain controller (DC) for the given
  // |account| and stores it in |account|->dc_name. The DC name is required as
  // host name in smbclient. With an IP address only, Samba wouldn't be able to
  // use the Kerberos ticket.
  ErrorType UpdateDcName(AccountData* account) const;

  // Writes the Samba configuration file for the given |account| and updates
  // the account's kdc_ip, dc_name and workgroup.
  ErrorType UpdateAccountData(AccountData* account);

  // Writes the file with configuration information.
  ErrorType WriteConfiguration() const;

  // Reads the file with configuration information.
  ErrorType ReadConfiguration();

  // Copies the machine keytab file to the state directory. The copy is owned by
  // authpolicyd, so that authpolicyd_exec cannot modify it anymore.
  ErrorType SecureMachineKeyTab() const;

  // Gets user account info. If |account_id| is not empty, searches by
  // objectGUID = |account_id| only. Otherwise, searches by sAMAccountName =
  // |user_name| and - if that fails - by userPrincipalName = |normalized_upn|.
  // Note that sAMAccountName can be different from the name-part of the
  // userPrincipalName and that kinit/Windows prefer sAMAccountName over
  // userPrincipalName. Assumes that the account is up-to-date and the user's
  // TGT is valid.
  ErrorType GetAccountInfo(const std::string& user_name,
                           const std::string& normalized_upn,
                           const std::string& account_id,
                           ActiveDirectoryAccountInfo* account_info);

  // Calls net ads search with given |search_string| to retrieve |account_info|.
  // Authenticates with the device TGT.
  ErrorType SearchAccountInfo(const std::string& search_string,
                              ActiveDirectoryAccountInfo* account_info);

  // Downloads GPOs and returns the |gpo_file_paths|. |source| determines
  // whether to get GPOs that apply to the user or the device. |scope|
  // determines whether user or device policy is to be loaded from the GPOs.
  // Note that some use cases like user policy loopback processing require
  // reading user policy from device GPOs. Calls GetGpoList() and DownloadGpos()
  // internally.
  ErrorType GetGpos(GpoSource source,
                    PolicyScope scope,
                    std::vector<base::FilePath>* gpo_file_paths);

  // Calls net ads gpo list to retrieve a list of GPOs in |gpo_list|. See
  // GetGpos() for an explanation of |source| and |scope|.
  ErrorType GetGpoList(GpoSource source,
                       PolicyScope scope,
                       protos::GpoList* gpo_list) const;

  // Downloads user or device GPOs in the given |gpo_list|. See GetGpos() for an
  // explanation of |source| and |scope|. Returns the downloaded GPO file paths
  // in |gpo_file_paths|.
  ErrorType DownloadGpos(const protos::GpoList& gpo_list,
                         GpoSource source,
                         PolicyScope scope,
                         std::vector<base::FilePath>* gpo_file_paths) const;

  // Parses GPOs and stores them in user/device policy protobufs.
  ErrorType ParseGposIntoProtobuf(
      const std::vector<base::FilePath>& gpo_file_paths,
      const char* parser_cmd_string,
      std::string* policy_blob) const;

  // Update stuff that depends on device policy like |encryption_types_|. Should
  // be called whenever new device policy is available.
  void UpdateDevicePolicyDependencies(
      const enterprise_management::ChromeDeviceSettingsProto& device_policy);

  // Get user or device AccountData. Depends on GpoSource, not on PolicyScope,
  // since that determines what account to download GPOs for.
  const AccountData& GetAccount(GpoSource source) const {
    return source == GpoSource::USER ? user_account_ : device_account_;
  }

  // Get user or device TGT manager. Depends on GpoSource, not on PolicyScope,
  // since that determines what account to download GPOs for and the TGT is tied
  // to the account.
  const TgtManager& GetTgtManager(GpoSource source) const {
    return source == GpoSource::USER ? user_tgt_manager_ : device_tgt_manager_;
  }

  // Sets and fixes the current user by account id key. Only one account id is
  // allowed per user. Calling this multiple times with different account ids
  // crashes the daemon.
  void SetUser(const std::string& account_id_key);

  // Similar to SetUser, but sets user_account_.realm.
  void SetUserRealm(const std::string& user_realm);

  // Sets encryption types used by Kerberos tickets.
  void SetKerberosEncryptionTypes(KerberosEncryptionTypes encryption_types);

  // Anonymizes |realm| in different capitalizations as well as all parts. For
  // instance, if realm is SOME.EXAMPLE.COM, anonymizes SOME, EXAMPLE and COM.
  void AnonymizeRealm(const std::string& realm, const char* placeholder);

  // Returns true if the device is not in a 'joined' state.
  bool IsDeviceJoined() const;

  // Resets internal state to an 'unenrolled' state by wiping configuration and
  // user data.
  void Reset();

  // Loads |flags_default_level_| from Path::FLAGS_DEFAULT_LEVEL. Logs an
  // error if the file exists, but the level cannot be loaded. Fails silently if
  // the file does not exist.
  void LoadFlagsDefaultLevel();

  // Saves |flags_default_level_| to Path::FLAGS_DEFAULT_LEVEL. Logs on error.
  void SaveFlagsDefaultLevel();

  // Reloads debug flags. Should be done on every public method called from
  // D-Bus, so that authpolicyd doesn't have to be restarted if the flags
  // change. Note that this is cheap in a production environment where the flags
  // file does not exist, so this is no performance concern.
  void ReloadDebugFlags();

  // User account_id (aka objectGUID).
  std::string user_account_id_;
  // User logon name.
  std::string user_sam_account_name_;
  // Timestamp of last password change on server.
  uint64_t user_pwd_last_set_ = 0;
  // Is the user logged in?
  bool user_logged_in_ = false;
  // Last AuthenticateUser() error.
  ErrorType last_auth_error_ = ERROR_NONE;

  AccountData user_account_;
  AccountData device_account_;

  // The order of members is carefully chosen to match initialization order, so
  // don't mess with it unless you have a reason.

  // UMA statistics, not owned.
  AuthPolicyMetrics* metrics_;

  // Lookup for file paths, not owned.
  const PathService* paths_;

  // Removes sensitive data from logs.
  std::unique_ptr<Anonymizer> anonymizer_;

  // Debug flags, loaded from Path::DEBUG_FLAGS.
  protos::DebugFlags flags_;
  AuthPolicyFlags::DefaultLevel flags_default_level_ = AuthPolicyFlags::kQuiet;

  // Helper to setup and run minijailed processes.
  JailHelper jail_helper_;

  // User and device ticket-granting-ticket managers.
  TgtManager user_tgt_manager_;
  TgtManager device_tgt_manager_;

  // Encryption types to use for kinit and Samba commands. Don't set directly,
  // always set through SetKerberosEncryptionTypes().
  KerberosEncryptionTypes encryption_types_ = ENC_TYPES_STRONG;

  // Manager for interesting Windows policy.
  WindowsPolicyManager windows_policy_manager_;

  // For testing only. Used/consumed during Initialize().
  std::unique_ptr<policy::DevicePolicyImpl> device_policy_impl_for_testing;

  // Whether kinit calls may return false negatives and must be retried.
  bool retry_machine_kinit_ = false;

  // Whether to sleep when retrying smbclient (disable for testing).
  bool smbclient_retry_sleep_enabled_ = true;

  DISALLOW_COPY_AND_ASSIGN(SambaInterface);
};

}  // namespace authpolicy

#endif  // AUTHPOLICY_SAMBA_INTERFACE_H_
