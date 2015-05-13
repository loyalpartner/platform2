// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBPROTOBINDER_IINTERFACE_H_
#define LIBPROTOBINDER_IINTERFACE_H_

#include "binder_export.h"                // NOLINT(build/include)
#include "binder_host.h"                  // NOLINT(build/include)
#include "binder_manager.h"               // NOLINT(build/include)
#include "binder_proxy_interface_base.h"  // NOLINT(build/include)
#include "ibinder.h"                      // NOLINT(build/include)

namespace protobinder {

// Abstract interface which BIDL services inherit from.
class BINDER_EXPORT IInterface {
 public:
  IInterface() {}
  ~IInterface() {}
};

template <typename INTERFACE>
class BINDER_EXPORT BinderHostInterface : public INTERFACE, public BinderHost {
 public:
 protected:
  virtual ~BinderHostInterface() {}
};

template <typename INTERFACE>
class BINDER_EXPORT BinderProxyInterface : public INTERFACE,
                                           public BinderProxyInterfaceBase {
 public:
  explicit BinderProxyInterface(BinderProxy* remote)
      : BinderProxyInterfaceBase(remote) {}

 protected:
  virtual ~BinderProxyInterface() {}
};

template <typename INTERFACE>
inline std::unique_ptr<INTERFACE> CreateInterface(BinderProxy* proxy) {
  std::unique_ptr<IInterface> test_interface =
      BinderManagerInterface::Get()->CreateTestInterface(proxy);
  if (test_interface) {
    return std::unique_ptr<INTERFACE>(
        static_cast<INTERFACE*>(test_interface.release()));
  }
  return INTERFACE::CreateInterface(proxy);
}

#define DECLARE_META_INTERFACE(INTERFACE)                                     \
    static std::unique_ptr<I##INTERFACE> CreateInterface(BinderProxy* proxy); \
    I##INTERFACE();                                                           \
    virtual ~I##INTERFACE();

#define IMPLEMENT_META_INTERFACE(INTERFACE, NAME)                           \
    std::unique_ptr<I##INTERFACE> I##INTERFACE::CreateInterface(            \
        BinderProxy* proxy) {                                               \
      return std::unique_ptr<I##INTERFACE>(new I##INTERFACE##Proxy(proxy)); \
    }                                                                       \
    I##INTERFACE::I##INTERFACE() {}                                         \
    I##INTERFACE::~I##INTERFACE() {}

}  // namespace protobinder

#endif  // LIBPROTOBINDER_IINTERFACE_H_
