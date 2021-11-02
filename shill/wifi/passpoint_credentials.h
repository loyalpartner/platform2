// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_WIFI_PASSPOINT_CREDENTIALS_H_
#define SHILL_WIFI_PASSPOINT_CREDENTIALS_H_

#include <base/memory/ref_counted.h>
#include <string>
#include <vector>

#include "shill/eap_credentials.h"
#include "shill/refptr_types.h"

namespace shill {

class EapCredentials;
class Error;
class KeyValueStore;

// A PasspointCredentials contains a set of criteria used to match a Wi-Fi
// network without identifying it using its SSID. It also contains the EAP
// credentials required to successfully authenticate to that network.
class PasspointCredentials : public base::RefCounted<PasspointCredentials> {
 public:
  PasspointCredentials(const PasspointCredentials&) = delete;
  PasspointCredentials& operator=(const PasspointCredentials&) = delete;

  // Create a set of Passpoint credentials from a dictionary. The content of
  // the dictionary is validated (including EAP credentials) according to
  // the requirements of Passpoint specifications.
  // Errors are reported in |error|.
  static PasspointCredentialsRefPtr CreatePasspointCredentials(
      const KeyValueStore& args, Error* error);

  const std::vector<std::string>& domains() const { return domains_; }
  const std::string& realm() const { return realm_; }
  const std::vector<uint64_t>& home_ois() const { return home_ois_; }
  const std::vector<uint64_t>& required_home_ois() const {
    return required_home_ois_;
  }
  const std::vector<uint64_t>& roaming_consortia() const {
    return roaming_consortia_;
  }
  const EapCredentials& eap() const { return eap_; }
  bool metered_override() const { return metered_override_; }
  const std::string android_package_name() const {
    return android_package_name_;
  }

 private:
  PasspointCredentials(const std::vector<std::string>& domains,
                       const std::string& realm,
                       const std::vector<uint64_t>& home_ois,
                       const std::vector<uint64_t>& required_home_ois,
                       const std::vector<uint64_t>& rc,
                       bool metered_override,
                       const std::string& android_package_name);

  // Home service provider FQDNs.
  std::vector<std::string> domains_;
  // Home Realm for Interworking.
  std::string realm_;
  // Organizational identifiers identifying the home service provider of which
  // the provider is a member. When at least one of these OI matches an OI
  // advertised by a Passpoint operator, an authentication with that hotspot
  // is possible and it is identified as a "home" network.
  std::vector<uint64_t> home_ois_;
  // Organizational idendifiers for home networks that must be matched to
  // connect to a network.
  std::vector<uint64_t> required_home_ois_;
  // Roaming consortium OI(s) used to determine which access points support
  // authentication with this credential. When one of the following OIs matches
  // an OI advertised by the access point, an authentication is possible and
  // the hotspot is identified as a "roaming" network.
  std::vector<uint64_t> roaming_consortia_;
  // Set of EAP credentials (TLS or TTLS only) used to connect to a network
  // that matched these credentials.
  EapCredentials eap_;
  // Tells weither we should consider the network as metered and override
  // the service value.
  bool metered_override_;
  // Package name of the application that provided the credentials, if any.
  std::string android_package_name_;
};

}  // namespace shill

#endif  // SHILL_WIFI_PASSPOINT_CREDENTIALS_H_