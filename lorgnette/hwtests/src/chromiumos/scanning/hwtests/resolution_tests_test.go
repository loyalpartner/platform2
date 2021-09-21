// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package hwtests

import (
	"testing"

	"chromiumos/scanning/utils"
)

// TestHasSupportedResolutionTest tests that HasSupportedResolutionTest
// functions correctly.
func TestHasSupportedResolutionTest(t *testing.T) {
	tests := []struct {
		platenCaps     utils.SourceCapabilities
		adfSimplexCaps utils.SourceCapabilities
		adfDuplexCaps  utils.SourceCapabilities
		failures       []utils.FailureType
	}{
		{
			// Should pass: both resolutions ranges include 75.
			platenCaps: utils.SourceCapabilities{
				MaxWidth:       1200,
				MinWidth:       16,
				MaxHeight:      2800,
				MinHeight:      32,
				MaxScanRegions: 2,
				SettingProfile: utils.SettingProfile{
					Name:            "",
					Ref:             "",
					ColorModes:      []string{"RGB24"},
					DocumentFormats: []string{"application/octet-stream"},
					SupportedResolutions: utils.SupportedResolutions{
						XResolutionRange: utils.ResolutionRange{
							Min:    65,
							Max:    85,
							Normal: 75,
							Step:   10},
						YResolutionRange: utils.ResolutionRange{
							Min:    60,
							Max:    105,
							Normal: 90,
							Step:   15}}},
				MaxOpticalXResolution: 85,
				MaxOpticalYResolution: 105,
				MaxPhysicalWidth:      1200,
				MaxPhysicalHeight:     2800},
			// Should pass: [300, 300] is an allowed discrete resolution.
			adfSimplexCaps: utils.SourceCapabilities{
				MaxWidth:       1200,
				MinWidth:       16,
				MaxHeight:      2800,
				MinHeight:      32,
				MaxScanRegions: 2,
				SettingProfile: utils.SettingProfile{
					Name:            "",
					Ref:             "",
					ColorModes:      []string{"RGB24"},
					DocumentFormats: []string{"application/octet-stream"},
					SupportedResolutions: utils.SupportedResolutions{
						DiscreteResolutions: []utils.DiscreteResolution{
							utils.DiscreteResolution{
								XResolution: 100,
								YResolution: 200},
							utils.DiscreteResolution{
								XResolution: 300,
								YResolution: 300}}}},
				MaxOpticalXResolution: 800,
				MaxOpticalYResolution: 1200,
				MaxPhysicalWidth:      1200,
				MaxPhysicalHeight:     2800},
			// Should pass: zero-value SourceCapabilities aren't checked.
			adfDuplexCaps: utils.SourceCapabilities{},
			failures:      []utils.FailureType{},
		},
		{
			// Should fail: no resolutions specified for non-zero-value struct.
			platenCaps: utils.SourceCapabilities{
				MaxWidth:       1200,
				MinWidth:       16,
				MaxHeight:      2800,
				MinHeight:      32,
				MaxScanRegions: 2,
				SettingProfile: utils.SettingProfile{
					Name:            "",
					Ref:             "",
					ColorModes:      []string{"RGB24"},
					DocumentFormats: []string{"application/octet-stream"},
					SupportedResolutions: utils.SupportedResolutions{
						XResolutionRange: utils.ResolutionRange{},
						YResolutionRange: utils.ResolutionRange{}}},
				MaxOpticalXResolution: 85,
				MaxOpticalYResolution: 105,
				MaxPhysicalWidth:      1200,
				MaxPhysicalHeight:     2800},
			// Should fail: no matching allowable X and Y resolutions.
			adfSimplexCaps: utils.SourceCapabilities{
				MaxWidth:       1200,
				MinWidth:       16,
				MaxHeight:      2800,
				MinHeight:      32,
				MaxScanRegions: 2,
				SettingProfile: utils.SettingProfile{
					Name:            "",
					Ref:             "",
					ColorModes:      []string{"RGB24"},
					DocumentFormats: []string{"application/octet-stream"},
					SupportedResolutions: utils.SupportedResolutions{
						DiscreteResolutions: []utils.DiscreteResolution{
							utils.DiscreteResolution{
								XResolution: 100,
								YResolution: 200},
							utils.DiscreteResolution{
								XResolution: 1200,
								YResolution: 1200}}}},
				MaxOpticalXResolution: 800,
				MaxOpticalYResolution: 1200,
				MaxPhysicalWidth:      1200,
				MaxPhysicalHeight:     2800},
			// Should fail: X and Y resolution ranges do not overlap.
			adfDuplexCaps: utils.SourceCapabilities{
				MaxWidth:       1200,
				MinWidth:       16,
				MaxHeight:      2800,
				MinHeight:      32,
				MaxScanRegions: 2,
				SettingProfile: utils.SettingProfile{
					Name:            "",
					Ref:             "",
					ColorModes:      []string{"RGB24"},
					DocumentFormats: []string{"application/octet-stream"},
					SupportedResolutions: utils.SupportedResolutions{
						XResolutionRange: utils.ResolutionRange{
							Min:    65,
							Max:    85,
							Normal: 75,
							Step:   10},
						YResolutionRange: utils.ResolutionRange{
							Min:    200,
							Max:    600,
							Normal: 300,
							Step:   100}}},
				MaxOpticalXResolution: 85,
				MaxOpticalYResolution: 600,
				MaxPhysicalWidth:      1200,
				MaxPhysicalHeight:     2800},
			failures: []utils.FailureType{utils.CriticalFailure, utils.CriticalFailure, utils.CriticalFailure},
		},
	}

	for _, tc := range tests {
		got, err := HasSupportedResolutionTest(tc.platenCaps, tc.adfSimplexCaps, tc.adfDuplexCaps)()

		if err != nil {
			t.Errorf("Unexpected error: %v", err)
		}

		if len(got) != len(tc.failures) {
			t.Errorf("Number of failures: expected %d, got %d", len(tc.failures), len(got))
		}
		for i, failure := range got {
			if failure.Type != tc.failures[i] {
				t.Errorf("FailureType: expected %d, got %d", tc.failures[i], failure.Type)
			}
		}
	}
}