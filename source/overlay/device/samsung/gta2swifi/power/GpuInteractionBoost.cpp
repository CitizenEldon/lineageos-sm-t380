// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "T380PowerPolicy"

#include "GpuInteractionPolicy.h"

#include <aidl/android/hardware/power/Boost.h>
#include <aidl/android/hardware/power/Mode.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <charconv>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

using aidl::android::hardware::power::Boost;
using aidl::android::hardware::power::Mode;

namespace aidl::android::hardware::power::impl {
namespace {

constexpr char kKgslPath[] = "/sys/class/kgsl/kgsl-3d0";
constexpr char kFloorPath[] = "/sys/class/kgsl/kgsl-3d0/min_pwrlevel";
constexpr int kBaselineFloor = 4;
constexpr int kBoostFloor = 0;

std::once_flag gInitializeOnce;
std::unique_ptr<t380::power::GpuInteractionPolicy> gPolicy;

bool readText(const std::string& path, std::string* value) {
    std::string raw;
    if (!::android::base::ReadFileToString(path, &raw, true)) return false;
    *value = ::android::base::Trim(raw);
    return true;
}

bool readInt(const std::string& path, int* value) {
    std::string text;
    if (!readText(path, &text)) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool writeFloor(int floor) {
    if (!::android::base::WriteStringToFile(std::to_string(floor) + "\n", kFloorPath, true)) {
        PLOG(ERROR) << "cannot write " << kFloorPath;
        return false;
    }
    int actual = -1;
    if (!readInt(kFloorPath, &actual) || actual != floor) {
        LOG(ERROR) << "GPU floor readback mismatch: requested=" << floor << " actual=" << actual;
        return false;
    }
    return true;
}

[[noreturn]] void restartAfterRestoreFailure() {
    LOG(ERROR) << "two GPU baseline restoration writes failed; terminating for init recovery";
    std::abort();
}

void initializeOnce() {
    std::string governor;
    int floor = -1;
    int maximum = -1;
    int levels = -1;
    if (!readText(std::string(kKgslPath) + "/devfreq/governor", &governor) ||
        !readInt(std::string(kKgslPath) + "/min_pwrlevel", &floor) ||
        !readInt(std::string(kKgslPath) + "/max_pwrlevel", &maximum) ||
        !readInt(std::string(kKgslPath) + "/num_pwrlevels", &levels) ||
        governor != "msm-adreno-tz" || floor != kBaselineFloor || maximum != 0 || levels != 5) {
        LOG(ERROR) << "unsupported GPU policy: governor=" << governor << " floor=" << floor
                   << " max=" << maximum << " levels=" << levels;
        return;
    }

    auto policy = std::make_unique<t380::power::GpuInteractionPolicy>(
            writeFloor, restartAfterRestoreFailure);
    if (!policy->start()) {
        LOG(ERROR) << "failed to initialize GPU interaction policy";
        return;
    }
    gPolicy = std::move(policy);
}

bool ready() {
    std::call_once(gInitializeOnce, initializeOnce);
    return gPolicy && gPolicy->healthy();
}

}  // namespace

bool initializeDeviceSpecificPower() {
    return ready();
}

void shutdownDeviceSpecificPower() {
    if (gPolicy && !gPolicy->shutdown()) LOG(ERROR) << "failed to restore GPU floor at shutdown";
}

bool isDeviceSpecificModeSupported(Mode type, bool* supported) {
    if (type != Mode::LAUNCH) return false;
    *supported = ready();
    return true;
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    if (type != Mode::LAUNCH) return false;
    if (!ready() || !gPolicy->setLaunch(enabled)) {
        LOG(ERROR) << "failed to apply LAUNCH=" << enabled;
    }
    return true;
}

bool isDeviceSpecificBoostSupported(Boost type, bool* supported) {
    if (type != Boost::INTERACTION) return false;
    *supported = ready();
    return true;
}

bool setDeviceSpecificBoost(Boost type, int32_t durationMs) {
    if (type != Boost::INTERACTION) return false;
    if (!ready() || !gPolicy->setInteraction(durationMs)) {
        LOG(ERROR) << "failed to apply INTERACTION duration=" << durationMs;
    }
    return true;
}

}  // namespace aidl::android::hardware::power::impl
