// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
namespace t380 {
int brightnessForColor(uint32_t color, int maximum);
int readMaximum(const std::string& path, int* maximum);
int writeBrightness(const std::string& path, int brightness);
}
