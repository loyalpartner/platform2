// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINIOS_DRAW_UTILS_H_
#define MINIOS_DRAW_UTILS_H_

#include <string>
#include <vector>

#include <base/files/file.h>
#include <base/files/file_util.h>
#include <base/strings/string_split.h>
#include <gtest/gtest_prod.h>

#include "minios/draw_interface.h"
#include "minios/process_manager.h"
#include "minios/screen_types.h"

namespace minios {

// Dropdown Menu Colors.
extern const char kMenuBlack[];
extern const char kMenuBlue[];
extern const char kMenuGrey[];
extern const char kMenuDropdownFrameNavy[];
extern const char kMenuDropdownBackgroundBlack[];
extern const char kMenuButtonFrameGrey[];

// Dimension Constants
extern const int kButtonHeight;
extern const int kButtonMargin;
extern const int kDefaultMessageWidth;
extern const int kMonospaceGlyphHeight;
extern const int kMonospaceGlyphWidth;

// Dimension Constants for Logging
extern const int kLogAreaHeight;
extern const int kLogAreaWidth;
extern const int kLogAreaY;
extern const int kLogCharPerLine;
extern const int kLogLinesPerPage;

// Frecon constants
extern const char kScreens[];
extern const int kFreconScalingFactor;
extern const int kCanvasSize;

// Key state parameters.
extern const int kFdsMax;
extern const int kKeyMax;

// `DrawUtils` contains all the different components needed to show MiniOS
// Screens.
class DrawUtils : public DrawInterface {
 public:
  explicit DrawUtils(ProcessManagerInterface* process_manager)
      : process_manager_(process_manager) {}
  ~DrawUtils() override = default;
  // Not copyable or movable.
  DrawUtils(const DrawUtils&) = delete;
  DrawUtils& operator=(const DrawUtils&) = delete;

  bool ShowText(const std::string& text,
                int glyph_offset_h,
                int glyph_offset_v,
                const std::string& color) override;

  bool ShowImage(const base::FilePath& image_name,
                 int offset_x,
                 int offset_y) override;

  bool ShowBox(int offset_x,
               int offset_y,
               int size_x,
               int size_y,
               const std::string& color) override;

  bool ShowMessage(const std::string& message_token,
                   int offset_x,
                   int offset_y) override;

  void ShowInstructions(const std::string& message_token) override;

  void ShowInstructionsWithTitle(const std::string& message_token) override;

  bool IsDetachable() override;

  void ShowButton(const std::string& message_token,
                  int offset_y,
                  bool is_selected,
                  int inner_width,
                  bool is_text) override;

  void ShowStepper(const std::vector<std::string>& steps) override;

  void MessageBaseScreen() override;

  void ShowCollapsedNetworkDropDown(bool is_selected) override;

  void ShowLanguageDropdown(int current_index) override;

  int FindLocaleIndex(int current_index) override;

  void ShowLanguageMenu(bool is_selected) override;

  void LocaleChange(int selected_locale) override;

  int GetSupportedLocalesSize() override { return supported_locales_.size(); }

  int GetDefaultButtonWidth() override { return default_button_width_; }

  // Override the root directory for testing. Default is '/'.
  void SetRootForTest(const std::string& test_root) {
    root_ = base::FilePath(test_root);
    screens_path_ = base::FilePath(root_).Append(kScreens);
  }

  // Override the current locale without using the language menu.
  void SetLanguageForTest(const std::string& test_locale) {
    locale_ = test_locale;
    // Reload locale dependent dimension constants.
    ReadDimensionConstants();
  }

  // Override whether current language is marked as being read from right to
  // left. Does not change language.
  void SetLocaleRtlForTest(bool is_rtl) { right_to_left_ = is_rtl; }

 protected:
  // Show progress bar at percentage given.
  void ShowProgressPercentage(double progress);

  // Clears full screen except the footer.
  void ClearMainArea();

  // Clears screen including the footer.
  void ClearScreen();

  // Shows footer with basic instructions and chromebook model.
  void ShowFooter();

  // Read dimension constants for current locale into memory. Must be updated
  // every time the language changes.
  void ReadDimensionConstants();

  // Sets the height or width of an image given the token. Returns false on
  // error.
  bool GetDimension(const std::string& token, int* token_dimension);

  // Read the language constants into memory. Does not change
  // based on the current locale. Returns false on failure.
  bool ReadLangConstants();

  // Sets the width of language token for a given locale. Returns false on
  // error.
  bool GetLangConstants(const std::string& locale, int* lang_width);

  // Gets frecon constants defined at initialization by Upstart job.
  void GetFreconConstants();

  // Checks whether the current language is read from right to left. Must be
  // updated every time the language changes.
  void CheckRightToLeft();

  // Get region from VPD. Set vpd_region_ to US as default.
  void GetVpdRegion();

  // Get hardware Id from crossystem. Set hwid to `CHROMEBOOK` as default.
  void ReadHardwareId();

  ProcessManagerInterface* process_manager_;

  int frecon_canvas_size_{1080};
  int frecon_scale_factor_{1};
  // Default button width. Changes for each locale.
  int default_button_width_{80};
  // Default root directory.
  base::FilePath root_{"/"};

  // Default screens path, set in init.
  base::FilePath screens_path_;

  // Default and fall back locale directory.
  std::string locale_{"en-US"};

  // Whether the locale is read from right to left.
  bool right_to_left_{false};

  // Key value pairs that store token name and measurements.
  base::StringPairs image_dimensions_;

  // Key value pairs that store language widths.
  base::StringPairs lang_constants_;

  // List of all supported locales.
  std::vector<std::string> supported_locales_;

  // Hardware Id read from crossystem.
  std::string hwid_;

  // Region code read from VPD. Used to determine keyboard layout. Does not
  // change based on selected locale.
  std::string vpd_region_;

  // Whether the device has a detachable keyboard.
  bool is_detachable_{false};
};

}  // namespace minios

#endif  // MINIOS_DRAW_UTILS_H_
