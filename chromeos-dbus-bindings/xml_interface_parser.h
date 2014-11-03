// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_DBUS_BINDINGS_XML_INTERFACE_PARSER_H_
#define CHROMEOS_DBUS_BINDINGS_XML_INTERFACE_PARSER_H_

#include <expat.h>

#include <map>
#include <string>
#include <vector>

#include <base/macros.h>

#include "chromeos-dbus-bindings/interface.h"

namespace base {

class FilePath;

}  // namespace base

namespace chromeos_dbus_bindings {

class XmlInterfaceParser {
 public:
  using XmlAttributeMap = std::map<std::string, std::string>;

  XmlInterfaceParser() = default;
  virtual ~XmlInterfaceParser() = default;

  virtual bool ParseXmlInterfaceFile(const base::FilePath& interface_file);
  const Interface& interface() const { return interface_; }

 private:
  friend class XmlInterfaceParserTest;

  // XML tag names.
  static const char kArgumentTag[];
  static const char kInterfaceTag[];
  static const char kMethodTag[];
  static const char kNodeTag[];
  static const char kSignalTag[];
  static const char kPropertyTag[];

  // XML attribute names.
  static const char kNameAttribute[];
  static const char kTypeAttribute[];
  static const char kDirectionAttribute[];
  static const char kAccessAttribute[];

  // XML argument directions.
  static const char kArgumentDirectionIn[];
  static const char kArgumentDirectionOut[];

  // Element callbacks on |this| called by HandleElementStart() and
  // HandleElementEnd(), respectively.
  void OnOpenElement(const std::string& element_name,
                     const XmlAttributeMap& attributes);
  void OnCloseElement(const std::string& element_name);

  // Methods for appending individual argument elements to the parser.
  void AddMethodArgument(const XmlAttributeMap& attributes);
  void AddSignalArgument(const XmlAttributeMap& attributes);

  // Finds the |element_key| element in |attributes|.  Returns true and sets
  // |element_value| on success.  Returns false otherwise.
  static bool GetElementAttribute(const XmlAttributeMap& attributes,
                                  const std::string& element_type,
                                  const std::string& element_key,
                                  std::string* element_value);

  // Asserts that a non-empty |element_key| attribute appears in |attributes|.
  // Returns the name on success, triggers a CHECK() otherwise.
  static std::string GetValidatedElementAttribute(
      const XmlAttributeMap& attributes,
      const std::string& element_type,
      const std::string& element_key);

  // Calls GetValidatedElementAttribute() for for the "name" property.
  static std::string GetValidatedElementName(
      const XmlAttributeMap& attributes,
      const std::string& element_type);

  // Method for extracting signal/method tag attributes to a struct.
  static Interface::Argument ParseArgument(const XmlAttributeMap& attributes,
                                           const std::string& element_type);

  // Method for extracting property tag attributes to a struct.
  static Interface::Property ParseProperty(const XmlAttributeMap& attributes);

  // Expat element callback functions.
  static void HandleElementStart(void* user_data,
                                 const XML_Char* element,
                                 const XML_Char** attr);
  static void HandleElementEnd(void* user_data, const XML_Char* element);

  // The output of the parse.
  Interface interface_;

  // Tracks where in the element traversal our parse has taken us.
  std::vector<std::string> element_path_;

  DISALLOW_COPY_AND_ASSIGN(XmlInterfaceParser);
};

}  // namespace chromeos_dbus_bindings

#endif  // CHROMEOS_DBUS_BINDINGS_XML_INTERFACE_PARSER_H_
