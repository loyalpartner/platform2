// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/vpn/ipsec_connection.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/files/file_path_watcher.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/strcat.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <re2/re2.h>

#include "shill/metrics.h"
#include "shill/process_manager.h"
#include "shill/vpn/vpn_util.h"

namespace shill {

namespace {

constexpr char kBaseRunDir[] = "/run/ipsec";
constexpr char kStrongSwanConfFileName[] = "strongswan.conf";
constexpr char kSwanctlConfFileName[] = "swanctl.conf";
constexpr char kSwanctlPath[] = "/usr/sbin/swanctl";
constexpr char kCharonPath[] = "/usr/libexec/ipsec/charon";
constexpr char kViciSocketPath[] = "/run/ipsec/charon.vici";
constexpr char kSmartcardModuleName[] = "crypto_module";
// aes128-sha256-modp3072: new strongSwan default
// aes128-sha1-modp2048: old strongSwan default
// 3des-sha1-modp1536: strongSwan fallback
// 3des-sha1-modp1024: for compatibility with Windows RRAS, which requires
//                     using the modp1024 dh-group
constexpr char kDefaultIKEProposals[] =
    "aes128-sha256-modp3072,aes128-sha1-modp2048,3des-sha1-modp1536,3des-sha1-"
    "modp1024,default";
// Cisco ASA L2TP/IPsec setup instructions indicate using md5 for authentication
// for the IPsec SA. Default StrongS/WAN setup is to only propose SHA1.
constexpr char kDefaultESPProposals[] =
    "aes128gcm16,aes128-sha256,aes128-sha1,3des-sha1,3des-md5,default";

constexpr char kChildSAName[] = "managed";

// The default timeout value used in `swanctl --initiate`.
constexpr base::TimeDelta kIPsecTimeout = base::TimeDelta::FromSeconds(30);

// Represents a section in the format used by strongswan.conf and swanctl.conf.
// We use this class only for formatting swanctl.conf since the contents of
// strongswan.conf generated by this class are fixed. The basic syntax is:
//   section  := name { settings }
//   settings := (section|keyvalue)*
//   keyvalue := key = value\n
// Also see the following link for more details.
// https://wiki.strongswan.org/projects/strongswan/wiki/Strongswanconf
class StrongSwanConfSection {
 public:
  explicit StrongSwanConfSection(const std::string& name) : name_(name) {}

  StrongSwanConfSection* AddSection(const std::string& name) {
    auto section = new StrongSwanConfSection(name);
    sections_.emplace_back(section);
    return section;
  }

  void AddKeyValue(const std::string& key, const std::string& value) {
    key_values_[key] = value;
  }

  std::string Format(int indent_base = 0) const {
    std::vector<std::string> lines;
    const std::string indent_str(indent_base, ' ');

    lines.push_back(base::StrCat({indent_str, name_, " {"}));
    for (const auto& [k, v] : key_values_) {
      lines.push_back(
          base::StrCat({indent_str, "  ", k, " = ", FormatValue(v)}));
    }
    for (const auto& section : sections_) {
      lines.push_back(section->Format(indent_base + 2));
    }
    lines.push_back(base::StrCat({indent_str, "}"}));

    return base::JoinString(lines, "\n");
  }

 private:
  // Wraps the value in quotation marks and encodes control chars to make sure
  // the whole value will be read as a single string.
  static std::string FormatValue(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 2);
    output.append("\"");
    for (char c : input) {
      switch (c) {
        case '\b':
          output.append("\\b");
          break;
        case '\f':
          output.append("\\f");
          break;
        case '\n':
          output.append("\\n");
          break;
        case '\r':
          output.append("\\r");
          break;
        case '\t':
          output.append("\\t");
          break;
        case '"':
          output.append("\\\"");
          break;
        case '\\':
          output.append("\\\\");
          break;
        default:
          output.push_back(c);
          break;
      }
    }
    output.append("\"");
    return output;
  }

  std::string name_;
  std::vector<std::unique_ptr<StrongSwanConfSection>> sections_;
  std::map<std::string, std::string> key_values_;
};

// Parsing the encryption algorithm output by swanctl, which may contain two
// parts: the algorithm name and an optional key size. See the following src
// files in the strongswan project for how the name is output:
// - libstrongswan/crypto/crypters/crypter.c
// - swanctl/commands/list-sas.c
Metrics::VpnIpsecEncryptionAlgorithm ParseEncryptionAlgorithm(
    const std::string& input) {
  // The name and the key size is concated with "-". Changes them into "_" for
  // simplicity.
  std::string algo_str;
  base::ReplaceChars(input, "-", "_", &algo_str);
  static const std::map<std::string, Metrics::VpnIpsecEncryptionAlgorithm>
      str2enum = {
          {"AES_CBC_128", Metrics::kVpnIpsecEncryptionAlgorithm_AES_CBC_128},
          {"AES_CBC_192", Metrics::kVpnIpsecEncryptionAlgorithm_AES_CBC_192},
          {"AES_CBC_256", Metrics::kVpnIpsecEncryptionAlgorithm_AES_CBC_256},
          {"CAMELLIA_CBC_128",
           Metrics::kVpnIpsecEncryptionAlgorithm_CAMELLIA_CBC_128},
          {"CAMELLIA_CBC_192",
           Metrics::kVpnIpsecEncryptionAlgorithm_CAMELLIA_CBC_192},
          {"CAMELLIA_CBC_256",
           Metrics::kVpnIpsecEncryptionAlgorithm_CAMELLIA_CBC_256},
          {"3DES_CBC", Metrics::kVpnIpsecEncryptionAlgorithm_3DES_CBC},
          {"AES_GCM_16_128",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_16_128},
          {"AES_GCM_16_192",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_16_192},
          {"AES_GCM_16_256",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_16_256},
          {"AES_GCM_12_128",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_12_128},
          {"AES_GCM_12_192",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_12_192},
          {"AES_GCM_12_256",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_12_256},
          {"AES_GCM_8_128",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_8_128},
          {"AES_GCM_8_192",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_8_192},
          {"AES_GCM_8_256",
           Metrics::kVpnIpsecEncryptionAlgorithm_AES_GCM_8_256},
      };
  const auto it = str2enum.find(algo_str);
  if (it == str2enum.end()) {
    return Metrics::kVpnIpsecEncryptionAlgorithmUnknown;
  }
  return it->second;
}

// Parsing the integrity algorithm output by swanctl, which may contain two
// parts: the algorithm name and an optional key size. See the following src
// files in the strongswan project for how the name is output:
// - libstrongswan/crypto/signers/signer.c
// - swanctl/commands/list-sas.c
Metrics::VpnIpsecIntegrityAlgorithm ParseIntegrityAlgorithm(
    const std::string& input) {
  // The name and the key size is concated with "-". Changes them into "_" for
  // simplicity.
  std::string algo_str;
  base::ReplaceChars(input, "-", "_", &algo_str);
  static const std::map<std::string, Metrics::VpnIpsecIntegrityAlgorithm>
      str2enum = {
          {"HMAC_SHA2_256_128",
           Metrics::kVpnIpsecIntegrityAlgorithm_HMAC_SHA2_256_128},
          {"HMAC_SHA2_384_192",
           Metrics::kVpnIpsecIntegrityAlgorithm_HMAC_SHA2_384_192},
          {"HMAC_SHA2_512_256",
           Metrics::kVpnIpsecIntegrityAlgorithm_HMAC_SHA2_512_256},
          {"HMAC_SHA1_96", Metrics::kVpnIpsecIntegrityAlgorithm_HMAC_SHA1_96},
          {"AES_XCBC_96", Metrics::kVpnIpsecIntegrityAlgorithm_AES_XCBC_96},
          {"AES_CMAC_96", Metrics::kVpnIpsecIntegrityAlgorithm_AES_CMAC_96},
      };
  const auto it = str2enum.find(algo_str);
  if (it == str2enum.end()) {
    return Metrics::kVpnIpsecIntegrityAlgorithmUnknown;
  }
  return it->second;
}

// Parsing the DH group output by swanctl. See the following src files in the
// strongswan project for the names:
// - libstrongswan/crypto/diffie_hellman.c
Metrics::VpnIpsecDHGroup ParseDHGroup(const std::string& input) {
  static const std::map<std::string, Metrics::VpnIpsecDHGroup> str2enum = {
      {"ECP_256", Metrics::kVpnIpsecDHGroup_ECP_256},
      {"ECP_384", Metrics::kVpnIpsecDHGroup_ECP_384},
      {"ECP_521", Metrics::kVpnIpsecDHGroup_ECP_521},
      {"ECP_256_BP", Metrics::kVpnIpsecDHGroup_ECP_256_BP},
      {"ECP_384_BP", Metrics::kVpnIpsecDHGroup_ECP_384_BP},
      {"ECP_512_BP", Metrics::kVpnIpsecDHGroup_ECP_512_BP},
      {"CURVE_25519", Metrics::kVpnIpsecDHGroup_CURVE_25519},
      {"CURVE_448", Metrics::kVpnIpsecDHGroup_CURVE_448},
      {"MODP_1024", Metrics::kVpnIpsecDHGroup_MODP_1024},
      {"MODP_1536", Metrics::kVpnIpsecDHGroup_MODP_1536},
      {"MODP_2048", Metrics::kVpnIpsecDHGroup_MODP_2048},
      {"MODP_3072", Metrics::kVpnIpsecDHGroup_MODP_3072},
      {"MODP_4096", Metrics::kVpnIpsecDHGroup_MODP_4096},
      {"MODP_6144", Metrics::kVpnIpsecDHGroup_MODP_6144},
      {"MODP_8192", Metrics::kVpnIpsecDHGroup_MODP_8192},
  };
  const auto it = str2enum.find(input);
  if (it == str2enum.end()) {
    return Metrics::kVpnIpsecDHGroupUnknown;
  }
  return it->second;
}

}  // namespace

// static
std::tuple<Metrics::VpnIpsecEncryptionAlgorithm,
           Metrics::VpnIpsecIntegrityAlgorithm,
           Metrics::VpnIpsecDHGroup>
IPsecConnection::ParseCipherSuite(const std::string& input) {
  constexpr auto kInvalidResults =
      std::make_tuple(Metrics::kVpnIpsecEncryptionAlgorithmUnknown,
                      Metrics::kVpnIpsecIntegrityAlgorithmUnknown,
                      Metrics::kVpnIpsecDHGroupUnknown);
  auto [encryption_algo, integrity_algo, dh_group] = kInvalidResults;

  const std::vector<std::string> names = base::SplitString(
      input, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (const auto& name : names) {
    // Tries parsing the name as an encryption algorithm.
    auto parsed_encryption_algo = ParseEncryptionAlgorithm(name);
    if (parsed_encryption_algo !=
        Metrics::kVpnIpsecEncryptionAlgorithmUnknown) {
      if (encryption_algo != Metrics::kVpnIpsecEncryptionAlgorithmUnknown) {
        // This means |input| contains algorithm names with a certain type
        // multiple times. This is not expected, discards the results.
        LOG(ERROR) << "The input contains multiple encryption algorithm: "
                   << input;
        return kInvalidResults;
      }
      encryption_algo = parsed_encryption_algo;
      continue;
    }

    // Tries parsing the name as an integrity algorithm.
    auto parsed_integrity_algo = ParseIntegrityAlgorithm(name);
    if (parsed_integrity_algo != Metrics::kVpnIpsecIntegrityAlgorithmUnknown) {
      if (integrity_algo != Metrics::kVpnIpsecIntegrityAlgorithmUnknown) {
        LOG(ERROR) << "The input contains multiple integrity algorithm: "
                   << input;
        return kInvalidResults;
      }
      integrity_algo = parsed_integrity_algo;
      continue;
    }

    // Tries parsing the name as a DH group.
    auto parsed_dh_group = ParseDHGroup(name);
    if (parsed_dh_group != Metrics::kVpnIpsecDHGroupUnknown) {
      if (dh_group != Metrics::kVpnIpsecDHGroupUnknown) {
        LOG(ERROR) << "The input contains multiple DH group: " << input;
        return kInvalidResults;
      }
      dh_group = parsed_dh_group;
      continue;
    }
  }

  return {encryption_algo, integrity_algo, dh_group};
}

IPsecConnection::IPsecConnection(std::unique_ptr<Config> config,
                                 std::unique_ptr<Callbacks> callbacks,
                                 std::unique_ptr<VPNConnection> l2tp_connection,
                                 EventDispatcher* dispatcher,
                                 ProcessManager* process_manager)
    : VPNConnection(std::move(callbacks), dispatcher),
      config_(std::move(config)),
      l2tp_connection_(std::move(l2tp_connection)),
      vici_socket_path_(kViciSocketPath),
      process_manager_(process_manager),
      vpn_util_(VPNUtil::New()) {
  if (l2tp_connection_) {
    l2tp_connection_->ResetCallbacks(std::make_unique<VPNConnection::Callbacks>(
        base::BindRepeating(&IPsecConnection::OnL2TPConnected,
                            weak_factory_.GetWeakPtr()),
        base::BindOnce(&IPsecConnection::OnL2TPFailure,
                       weak_factory_.GetWeakPtr()),
        base::BindOnce(&IPsecConnection::OnL2TPStopped,
                       weak_factory_.GetWeakPtr())));
  } else {
    NOTREACHED();  // Reserved for IKEv2 VPN
  }
}

IPsecConnection::~IPsecConnection() {
  if (state() == State::kIdle || state() == State::kStopped) {
    return;
  }

  // This is unexpected but cannot be fully avoided. Call OnDisconnect() to make
  // sure resources are released.
  LOG(WARNING) << "Destructor called but the current state is " << state();
  OnDisconnect();
}

void IPsecConnection::OnConnect() {
  temp_dir_ = vpn_util_->CreateScopedTempDir(base::FilePath(kBaseRunDir));
  if (!temp_dir_.IsValid()) {
    NotifyFailure(Service::kFailureInternal,
                  "Failed to create temp dir for IPsec");
    return;
  }

  ScheduleConnectTask(ConnectStep::kStart);
}

void IPsecConnection::ScheduleConnectTask(ConnectStep step) {
  switch (step) {
    case ConnectStep::kStart:
      WriteStrongSwanConfig();
      return;
    case ConnectStep::kStrongSwanConfigWritten:
      StartCharon();
      return;
    case ConnectStep::kCharonStarted:
      WriteSwanctlConfig();
      return;
    case ConnectStep::kSwanctlConfigWritten:
      SwanctlLoadConfig();
      return;
    case ConnectStep::kSwanctlConfigLoaded:
      SwanctlInitiateConnection();
      return;
    case ConnectStep::kIPsecConnected:
      SwanctlListSAs();
      return;
    case ConnectStep::kIPsecStatusRead:
      if (l2tp_connection_) {
        l2tp_connection_->Connect();
      } else {
        NOTREACHED();  // Reserved for IKEv2 VPN
      }
      return;
    default:
      NOTREACHED();
  }
}

void IPsecConnection::WriteStrongSwanConfig() {
  strongswan_conf_path_ = temp_dir_.GetPath().Append(kStrongSwanConfFileName);

  // See the following link for the format and descriptions for each field:
  // https://wiki.strongswan.org/projects/strongswan/wiki/strongswanconf
  // TODO(b/165170125): Check if routing_table is still required.
  std::vector<std::string> lines = {
      "charon {",
      "  accept_unencrypted_mainmode_messages = yes",
      "  ignore_routing_tables = 0",
      "  install_routes = no",
      "  routing_table = 0",
      "  syslog {",
      "    daemon {",
      "      ike = 2",  // Logs some traffic selector info.
      "      cfg = 2",  // Logs algorithm proposals.
      "      knl = 2",  // Logs high-level xfrm crypto parameters.
      "    }",
      "  }",
      "  plugins {",
      "    pkcs11 {",
      "      modules {",
      base::StringPrintf("        %s {", kSmartcardModuleName),
      "          path = " + std::string{PKCS11_LIB},
      "        }",
      "      }",
      "    }",
      "  }",
      "}",
  };

  std::string contents = base::JoinString(lines, "\n");
  if (!vpn_util_->WriteConfigFile(strongswan_conf_path_, contents)) {
    NotifyFailure(Service::kFailureInternal,
                  base::StrCat({"Failed to write ", kStrongSwanConfFileName}));
    return;
  }
  ScheduleConnectTask(ConnectStep::kStrongSwanConfigWritten);
}

// The swanctl.conf which we generate here will look like:
// connections {
//   vpn { // A connection named "vpn".
//     ... // Parameters used in the IKE phase.
//     local-1 { ... } // First round of authentication in local or remote.
//     remote-1 { ... }
//     local-2 { ... } // Second round of authentication (if exists).
//     remote-2 { ... }
//     managed { // A CHILD_SA named "managed".
//       ... // Parameters for SA negotiation.
//     }
//   }
// }
// secrets {
//   ... // secrets used in IKE (e.g., PSK).
// }
// For the detailed meanings of each field, see
// https://wiki.strongswan.org/projects/strongswan/wiki/Swanctlconf
void IPsecConnection::WriteSwanctlConfig() {
  swanctl_conf_path_ = temp_dir_.GetPath().Append(kSwanctlConfFileName);

  using Section = StrongSwanConfSection;

  Section connections_section("connections");
  Section secrets_section("secrets");

  Section* vpn_section = connections_section.AddSection("vpn");
  vpn_section->AddKeyValue("local_addrs", "0.0.0.0/0,::/0");
  vpn_section->AddKeyValue("remote_addrs", config_->remote);
  vpn_section->AddKeyValue("proposals", kDefaultIKEProposals);
  vpn_section->AddKeyValue("version", "1");  // IKEv1

  // Fields for authentication.
  Section* local1 = vpn_section->AddSection("local-1");
  Section* remote1 = vpn_section->AddSection("remote-1");
  if (config_->psk.has_value()) {
    local1->AddKeyValue("auth", "psk");
    remote1->AddKeyValue("auth", "psk");
    auto* psk_section = secrets_section.AddSection("ike-1");
    psk_section->AddKeyValue("secret", config_->psk.value());
  } else {
    if (!config_->ca_cert_pem_strings.has_value() ||
        !config_->client_cert_id.has_value() ||
        !config_->client_cert_pin.has_value() ||
        !config_->client_cert_slot.has_value()) {
      NotifyFailure(Service::kFailureInternal,
                    "Expect cert auth but some required fields are empty");
      return;
    }

    local1->AddKeyValue("auth", "pubkey");
    remote1->AddKeyValue("auth", "pubkey");

    // Writes server CA to a file and references this file in the config.
    server_ca_.set_root_directory(temp_dir_.GetPath());
    server_ca_path_ =
        server_ca_.CreatePEMFromStrings(config_->ca_cert_pem_strings.value());
    remote1->AddKeyValue("cacerts", server_ca_path_.value());

    Section* cert = local1->AddSection("cert");
    cert->AddKeyValue("handle", config_->client_cert_id.value());
    cert->AddKeyValue("slot", config_->client_cert_slot.value());
    cert->AddKeyValue("module", kSmartcardModuleName);

    Section* token = secrets_section.AddSection("token-1");
    token->AddKeyValue("module", kSmartcardModuleName);
    token->AddKeyValue("handle", config_->client_cert_id.value());
    token->AddKeyValue("slot", config_->client_cert_slot.value());
    token->AddKeyValue("pin", config_->client_cert_pin.value());
  }

  if (config_->xauth_user.has_value() || config_->xauth_password.has_value()) {
    if (!config_->xauth_user.has_value()) {
      NotifyFailure(Service::kFailureInternal, "Only Xauth password is set");
      return;
    }
    if (!config_->xauth_password.has_value()) {
      NotifyFailure(Service::kFailureInternal, "Only Xauth user is set");
      return;
    }

    Section* local2 = vpn_section->AddSection("local-2");
    local2->AddKeyValue("auth", "xauth");
    local2->AddKeyValue("xauth_id", config_->xauth_user.value());
    Section* xauth_section = secrets_section.AddSection("xauth-1");
    xauth_section->AddKeyValue("id", config_->xauth_user.value());
    xauth_section->AddKeyValue("secret", config_->xauth_password.value());
  }

  // TODO(b/165170125): This part is untested.
  if (config_->tunnel_group.has_value()) {
    // Aggressive mode is insecure but required by the legacy Cisco VPN here.
    // See https://crbug.com/199004 .
    vpn_section->AddKeyValue("aggressive", "yes");

    // Sets local id.
    const std::string tunnel_group = config_->tunnel_group.value();
    const std::string hex_tunnel_id =
        base::HexEncode(tunnel_group.c_str(), tunnel_group.length());
    const std::string local_id =
        base::StringPrintf("@#%s", hex_tunnel_id.c_str());
    local1->AddKeyValue("id", local_id);
  }

  // Fields for CHILD_SA.
  Section* children_section = vpn_section->AddSection("children");
  Section* child_section = children_section->AddSection(kChildSAName);
  child_section->AddKeyValue(
      "local_ts", base::StrCat({"dynamic[", config_->local_proto_port, "]"}));
  child_section->AddKeyValue(
      "remote_ts", base::StrCat({"dynamic[", config_->remote_proto_port, "]"}));
  child_section->AddKeyValue("esp_proposals", kDefaultESPProposals);
  // L2TP/IPsec always uses transport mode.
  child_section->AddKeyValue("mode", "transport");

  // Writes to file.
  const std::string contents = base::StrCat(
      {connections_section.Format(), "\n", secrets_section.Format()});
  if (!vpn_util_->WriteConfigFile(swanctl_conf_path_, contents)) {
    NotifyFailure(
        Service::kFailureInternal,
        base::StrCat({"Failed to write swanctl.conf", kSwanctlConfFileName}));
    return;
  }

  ScheduleConnectTask(ConnectStep::kSwanctlConfigWritten);
}

void IPsecConnection::StartCharon() {
  // We should make sure there is no socket file before starting charon, since
  // we rely on its existence to know if charon is ready.
  if (base::PathExists(vici_socket_path_)) {
    // This could happen if something unexpected happened in the previous run,
    // e.g., shill crashed.
    LOG(WARNING) << "vici socket exists before starting charon";
    if (!base::DeleteFile(vici_socket_path_)) {
      const std::string reason = "Failed to delete vici socket file";
      PLOG(ERROR) << reason;
      NotifyFailure(Service::kFailureInternal, reason);
      return;
    }
  }

  // TODO(b/165170125): Check the behavior when shill crashes (if charon is
  // still running).
  std::vector<std::string> args = {};
  std::map<std::string, std::string> env = {
      {"STRONGSWAN_CONF", strongswan_conf_path_.value()},
  };
  // TODO(b/197199752): Consider removing CAP_SETGID.
  constexpr uint64_t kCapMask =
      CAP_TO_MASK(CAP_NET_ADMIN) | CAP_TO_MASK(CAP_NET_BIND_SERVICE) |
      CAP_TO_MASK(CAP_NET_RAW) | CAP_TO_MASK(CAP_SETGID);
  auto minijail_options = VPNUtil::BuildMinijailOptions(kCapMask);
  // Charon can have a quite large VmSize/VmPeak despite not using much resident
  // memory. This can be partially reduced by lowering charon.threads, but in
  // any case, Charon cannot rely on inheriting shill's RLIMIT_AS. See
  // crbug/961519.
  minijail_options.rlimit_as_soft = 750'000'000;  // 750MB
  charon_pid_ = process_manager_->StartProcessInMinijail(
      FROM_HERE, base::FilePath(kCharonPath), args, env, minijail_options,
      base::BindOnce(&IPsecConnection::OnCharonExitedUnexpectedly,
                     weak_factory_.GetWeakPtr()));

  if (charon_pid_ == -1) {
    NotifyFailure(Service::kFailureInternal, "Failed to start charon");
    return;
  }

  LOG(INFO) << "charon started";

  if (!base::PathExists(vici_socket_path_)) {
    vici_socket_watcher_ = std::make_unique<base::FilePathWatcher>();
    auto callback = base::BindRepeating(&IPsecConnection::OnViciSocketPathEvent,
                                        weak_factory_.GetWeakPtr());
    if (!vici_socket_watcher_->Watch(vici_socket_path_,
                                     base::FilePathWatcher::Type::kNonRecursive,
                                     callback)) {
      NotifyFailure(Service::kFailureInternal,
                    "Failed to set up FilePathWatcher for the vici socket");
      return;
    }
  } else {
    LOG(INFO) << "vici socket is already here";
    ScheduleConnectTask(ConnectStep::kCharonStarted);
  }
}

void IPsecConnection::SwanctlLoadConfig() {
  const std::vector<std::string> args = {"--load-all", "--file",
                                         swanctl_conf_path_.value()};
  RunSwanctl(args,
             base::BindOnce(&IPsecConnection::SwanctlNextStep,
                            weak_factory_.GetWeakPtr(),
                            ConnectStep::kSwanctlConfigLoaded),
             "Failed to load swanctl.conf");
}

void IPsecConnection::SwanctlInitiateConnection() {
  // This is a blocking call: if the execution returns with 0, then it means the
  // IPsec connection has been established.
  const std::string timeout_str =
      base::NumberToString(kIPsecTimeout.InSeconds());
  const std::vector<std::string> args = {"--initiate", "-c", kChildSAName,
                                         "--timeout", timeout_str};
  RunSwanctl(
      args,
      base::BindOnce(&IPsecConnection::SwanctlNextStep,
                     weak_factory_.GetWeakPtr(), ConnectStep::kIPsecConnected),
      "Failed to initiate IPsec connection");
}

void IPsecConnection::SwanctlListSAs() {
  const std::vector<std::string> args = {"--list-sas"};
  RunSwanctl(args,
             base::BindOnce(&IPsecConnection::OnSwanctlListSAsDone,
                            weak_factory_.GetWeakPtr()),
             "Failed to get SA information");
}

void IPsecConnection::OnViciSocketPathEvent(const base::FilePath& /*path*/,
                                            bool error) {
  if (state() != State::kConnecting) {
    LOG(WARNING) << "OnViciSocketPathEvent triggered on state " << state();
    return;
  }

  if (error) {
    NotifyFailure(Service::kFailureInternal,
                  "FilePathWatcher error for the vici socket");
    return;
  }

  if (!base::PathExists(vici_socket_path_)) {
    // This is kind of unexpected, since the first event should be the creation
    // of this file. Waits for the next event.
    LOG(WARNING) << "vici socket is still not ready";
    return;
  }

  LOG(INFO) << "vici socket is ready";

  vici_socket_watcher_ = nullptr;
  ScheduleConnectTask(ConnectStep::kCharonStarted);
}

void IPsecConnection::OnCharonExitedUnexpectedly(int exit_code) {
  charon_pid_ = -1;
  NotifyFailure(Service::kFailureInternal,
                base::StringPrintf(
                    "charon exited unexpectedly with exit code %d", exit_code));
  return;
}

void IPsecConnection::OnSwanctlListSAsDone(const std::string& stdout_str) {
  // Note that any failure in parsing the cipher suite is unexpected but will
  // not block the connection. We only leave a log for such failures.
  const std::vector<std::string> lines = base::SplitString(
      stdout_str, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);

  SetIKECipherSuite(lines);
  SetESPCipherSuite(lines);

  ScheduleConnectTask(ConnectStep::kIPsecStatusRead);
}

void IPsecConnection::RunSwanctl(const std::vector<std::string>& args,
                                 SwanctlCallback on_success,
                                 const std::string& message_on_failure) {
  std::map<std::string, std::string> env = {
      {"STRONGSWAN_CONF", strongswan_conf_path_.value()},
  };

  constexpr uint64_t kCapMask = 0;
  pid_t pid = process_manager_->StartProcessInMinijailWithStdout(
      FROM_HERE, base::FilePath(kSwanctlPath), args, env,
      VPNUtil::BuildMinijailOptions(kCapMask),
      base::BindOnce(&IPsecConnection::OnSwanctlExited,
                     weak_factory_.GetWeakPtr(), std::move(on_success),
                     message_on_failure));
  if (pid == -1) {
    NotifyFailure(Service::kFailureInternal, message_on_failure);
  }
}

void IPsecConnection::OnSwanctlExited(SwanctlCallback on_success,
                                      const std::string& message_on_failure,
                                      int exit_code,
                                      const std::string& stdout_str) {
  if (exit_code == 0) {
    std::move(on_success).Run(stdout_str);
  } else {
    NotifyFailure(Service::kFailureInternal,
                  base::StringPrintf("%s, exit_code=%d",
                                     message_on_failure.c_str(), exit_code));
  }
}

void IPsecConnection::SwanctlNextStep(ConnectStep step, const std::string&) {
  ScheduleConnectTask(step);
}

void IPsecConnection::SetIKECipherSuite(
    const std::vector<std::string>& swanctl_output) {
  ike_encryption_algo_ = Metrics::kVpnIpsecEncryptionAlgorithmUnknown;
  ike_integrity_algo_ = Metrics::kVpnIpsecIntegrityAlgorithmUnknown;
  ike_dh_group_ = Metrics::kVpnIpsecDHGroupUnknown;

  // The index of the line which contains the cipher suite information for IKE
  // in |swanctl_output|.
  constexpr int kIKECipherSuiteLineNumber = 3;
  if (swanctl_output.size() <= kIKECipherSuiteLineNumber) {
    LOG(ERROR) << "Failed to parse the IKE cipher suite, the number of line is "
               << swanctl_output.size();
    return;
  }

  // Example: AES_CBC-128/HMAC_SHA2_256_128/PRF_HMAC_SHA2_256/MODP_3072
  // See `swanctl/commands/list-sas.c:ike_sa()` in the strongswan project for
  // the format.
  static constexpr LazyRE2 kIKECipherSuiteLine = {
      R"(^\s*((?:[^/\s]+)(?:/[^/\s]+)*)\s*$)"};
  const std::string& line = swanctl_output[kIKECipherSuiteLineNumber];

  std::string matched_part;
  if (!RE2::FullMatch(line, *kIKECipherSuiteLine, &matched_part)) {
    LOG(ERROR) << "Failed to parse the IKE cipher suite, the line is: " << line;
    return;
  }

  std::tie(ike_encryption_algo_, ike_integrity_algo_, ike_dh_group_) =
      ParseCipherSuite(matched_part);
  if (ike_encryption_algo_ == Metrics::kVpnIpsecEncryptionAlgorithmUnknown ||
      ike_integrity_algo_ == Metrics::kVpnIpsecIntegrityAlgorithmUnknown ||
      ike_dh_group_ == Metrics::kVpnIpsecDHGroupUnknown) {
    LOG(ERROR) << "The output does not contain a valid cipher suite for IKE: "
               << matched_part;
  }
}

void IPsecConnection::SetESPCipherSuite(
    const std::vector<std::string>& swanctl_output) {
  esp_encryption_algo_ = Metrics::kVpnIpsecEncryptionAlgorithmUnknown;
  esp_integrity_algo_ = Metrics::kVpnIpsecIntegrityAlgorithmUnknown;

  // The index of the line which contains the cipher suite information for ESP
  // in |swanctl_output|.
  constexpr int kESPCipherSuiteLineNumber = 5;
  if (swanctl_output.size() <= kESPCipherSuiteLineNumber) {
    LOG(ERROR) << "Failed to parse the ESP cipher suite, the number of line is "
               << swanctl_output.size();
    return;
  }

  // This line does not only contains the cipher suite for ESP. Example:
  //  managed: #1, reqid 1, INSTALLED, TUNNEL, ESP:AES_CBC-128/HMAC_SHA2_256_128
  // See `swanctl/commands/list-sas.c:child_sas()` in the strongswan project
  // for the format.
  static constexpr LazyRE2 kESPCipherSuiteLine = {
      R"(^.*ESP:((?:[^/\s]+)(?:/[^/\s]+)*)\s*$)"};
  const std::string& line = swanctl_output[kESPCipherSuiteLineNumber];

  std::string matched_part;
  if (!RE2::FullMatch(line, *kESPCipherSuiteLine, &matched_part)) {
    LOG(ERROR) << "Failed to parse the ESP cipher suite, the line is: " << line;
    return;
  }

  const auto parsed_results = ParseCipherSuite(matched_part);
  esp_encryption_algo_ = std::get<0>(parsed_results);
  esp_integrity_algo_ = std::get<1>(parsed_results);
  if (esp_encryption_algo_ == Metrics::kVpnIpsecEncryptionAlgorithmUnknown ||
      esp_integrity_algo_ == Metrics::kVpnIpsecIntegrityAlgorithmUnknown) {
    LOG(ERROR) << "The output does not contain a valid cipher suite for ESP: "
               << matched_part;
  }
}

void IPsecConnection::OnL2TPConnected(const std::string& interface_name,
                                      int interface_index,
                                      const IPConfig::Properties& properties) {
  if (state() != State::kConnecting) {
    // This is possible, e.g., the upper layer called Disconnect() right before
    // this callback is triggered.
    LOG(WARNING) << "OnL2TPConnected() called but the IPsec layer is "
                 << state();
    return;
  }
  NotifyConnected(interface_name, interface_index, properties);
}

void IPsecConnection::OnDisconnect() {
  if (!l2tp_connection_) {
    StopCharon();
    return;
  }

  switch (l2tp_connection_->state()) {
    case State::kIdle:
      StopCharon();
      return;
    case State::kConnecting:
    case State::kConnected:
      l2tp_connection_->Disconnect();
      return;
    case State::kDisconnecting:
      // StopCharon() called in the stopped callback.
      return;
    case State::kStopped:
      // If |l2tp_connection_| is in stopped state but has not been destroyed,
      // the stopped callback must be in the queue, so StopCharon() will be
      // called later.
      return;
    default:
      NOTREACHED();
  }
}

void IPsecConnection::OnL2TPFailure(Service::ConnectFailure reason) {
  switch (state()) {
    case State::kDisconnecting:
      // If the IPsec layer is disconnecting, it could mean the failure happens
      // in the IPsec layer, and the failure must have been propagated to the
      // upper layer.
      return;
    case State::kConnecting:
    case State::kConnected:
      NotifyFailure(reason, "L2TP layer failure");
      return;
    default:
      // Other states are unexpected.
      LOG(DFATAL) << "OnL2TPFailure() called but the IPsec layer is "
                  << state();
  }
}

void IPsecConnection::OnL2TPStopped() {
  l2tp_connection_ = nullptr;
  if (state() != State::kDisconnecting) {
    LOG(DFATAL) << "OnL2TPStopped() called but the IPsec layer is " << state();
    // Does the cleanup anyway.
  }
  StopCharon();
}

void IPsecConnection::StopCharon() {
  if (charon_pid_ != -1) {
    process_manager_->StopProcess(charon_pid_);
    charon_pid_ = -1;
  }

  // Removes the vici socket file, since the charon process will not do that by
  // itself. Note that base::DeleteFile() will return true if the file does not
  // exist.
  if (!base::DeleteFile(vici_socket_path_)) {
    PLOG(ERROR) << "Failed to delete vici socket file";
  }

  // This function can be called directly from the destructor, and in that case
  // the state may not be kDisconnecting.
  if (state() == State::kDisconnecting) {
    // Currently we do not wait for charon fully stopped to send out this
    // signal.
    NotifyStopped();
  }
}

}  // namespace shill
