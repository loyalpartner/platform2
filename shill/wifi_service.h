// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_WIFI_SERVICE_
#define SHILL_WIFI_SERVICE_

#include <string>
#include <vector>

#include "shill/dbus_bindings/supplicant-interface.h"
#include "shill/event_dispatcher.h"
#include "shill/refptr_types.h"
#include "shill/service.h"

namespace shill {

class ControlInterface;
class EventDispatcher;
class Error;
class Manager;

class WiFiService : public Service {
 public:
  WiFiService(ControlInterface *control_interface,
              EventDispatcher *dispatcher,
              Manager *manager,
              const WiFiRefPtr &device,
              const std::vector<uint8_t> ssid,
              const std::string &mode,
              const std::string &security,
              bool hidden_ssid);
  ~WiFiService();

  // Inherited from Service.
  virtual void Connect(Error *error);
  virtual void Disconnect();

  virtual bool TechnologyIs(const Technology::Identifier type) const;

  // wifi_<MAC>_<BSSID>_<mode_string>_<security_string>
  std::string GetStorageIdentifier() const;

  const std::string &mode() const;
  const std::string &key_management() const;
  const std::vector<uint8_t> &ssid() const;

  void SetPassphrase(const std::string &passphrase, Error *error);

  // Overrride Load and Save from parent Service class.  We will call
  // the parent method.
  virtual bool IsLoadableFrom(StoreInterface *storage) const;
  virtual bool Load(StoreInterface *storage);
  virtual bool Save(StoreInterface *storage);

  bool IsSecurityMatch(const std::string &security) const;
  bool hidden_ssid() const { return hidden_ssid_; }

 private:
  friend class WiFiServiceSecurityTest;
  FRIEND_TEST(WiFiServiceTest, ConnectTaskRSN);
  FRIEND_TEST(WiFiServiceTest, ConnectTaskWPA);
  FRIEND_TEST(WiFiServiceTest, ConnectTaskPSK);
  FRIEND_TEST(WiFiServiceTest, LoadHidden);

  static const char kStorageHiddenSSID[];

  void ConnectTask();

  std::string GetDeviceRpcId();

  static std::string ParseWEPPassphrase(const std::string &passphrase,
                                        Error *error);
  static std::string ParseWPAPassphrase(const std::string &passphrase,
                                        Error *error);
  static bool CheckWEPIsHex(const std::string &passphrase, Error *error);
  static bool CheckWEPKeyIndex(const std::string &passphrase, Error *error);
  static bool CheckWEPPrefix(const std::string &passphrase, Error *error);

  // replace non-ASCII characters with '?'. return true if one or more
  // characters were changed
  static bool SanitizeSSID(std::string *ssid);

  // "wpa", "rsn" and "psk" are equivalent from a configuration perspective.
  // This function maps them all into "psk".
  static std::string GetSecurityClass(const std::string &security);

  // Profile data for a WPA/RSN service can be stored under a number of
  // different names.  These functions create different storage identifiers
  // based on whether they are referred to by their generic "psk" name or
  // if they use the (legacy) specific "wpa" or "rsn" names.
  std::string GetGenericStorageIdentifier() const;
  std::string GetSpecificStorageIdentifier() const;
  std::string GetStorageIdentifierForSecurity(
      const std::string &security) const;

  // Properties
  std::string passphrase_;
  bool need_passphrase_;
  std::string security_;
  uint8 strength_;
  // TODO(cmasone): see if the below can be pulled from the endpoint associated
  // with this service instead.
  const std::string mode_;
  std::string auth_mode_;
  bool hidden_ssid_;
  uint16 frequency_;
  uint16 physical_mode_;
  std::string hex_ssid_;
  std::string storage_identifier_;

  ScopedRunnableMethodFactory<WiFiService> task_factory_;
  WiFiRefPtr wifi_;
  const std::vector<uint8_t> ssid_;
  DISALLOW_COPY_AND_ASSIGN(WiFiService);
};

}  // namespace shill

#endif  // SHILL_WIFI_SERVICE_
