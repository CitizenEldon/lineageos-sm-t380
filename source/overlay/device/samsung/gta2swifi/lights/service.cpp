// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "T380Lights"
#include "Backlight.h"
#include <aidl/android/hardware/light/BnLights.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <mutex>
#include <cstdio>

using aidl::android::hardware::light::BnLights;
using aidl::android::hardware::light::HwLight;
using aidl::android::hardware::light::HwLightState;
using aidl::android::hardware::light::LightType;
namespace {
constexpr char kDirectory[] = "/sys/class/leds/lcd-backlight/";
class Lights : public BnLights {
    std::mutex mutex;
    int maximum = 0;
    int requested = -1;
    int written = -1;
    int lastError = 0;
    uint64_t writes = 0;
public:
    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override {
        HwLight backlight;
        backlight.id = 0;
        backlight.ordinal = 0;
        backlight.type = LightType::BACKLIGHT;
        *lights = {backlight};
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override {
        if (id != 0) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        std::lock_guard lock(mutex);
        if (maximum == 0) {
            lastError = t380::readMaximum(std::string(kDirectory) + "max_brightness", &maximum);
            if (lastError) {
                LOG(ERROR) << "Cannot read backlight range: " << lastError;
                return ndk::ScopedAStatus::fromServiceSpecificError(lastError);
            }
        }
        requested = t380::brightnessForColor(static_cast<uint32_t>(state.color), maximum);
        lastError = t380::writeBrightness(std::string(kDirectory) + "brightness", requested);
        if (lastError) {
            LOG(ERROR) << "Cannot write backlight: " << lastError;
            return ndk::ScopedAStatus::fromServiceSpecificError(lastError);
        }
        written = requested;
        ++writes;
        return ndk::ScopedAStatus::ok();
    }
    binder_status_t dump(int fd, const char**, uint32_t) override {
        std::lock_guard lock(mutex);
        std::string actual;
        const bool readOk = android::base::ReadFileToString(
                std::string(kDirectory) + "brightness", &actual);
        dprintf(fd, "T380 backlight: max=%d requested=%d written=%d error=%d writes=%llu\n",
                maximum, requested, written, lastError, static_cast<unsigned long long>(writes));
        dprintf(fd, "driver_readback=%s\n", readOk ? actual.c_str() : "unavailable");
        return STATUS_OK;
    }
};
}
int main(int argc, char** argv) {
    (void)argc;
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto service = ndk::SharedRefBase::make<Lights>();
    const std::string name = std::string(Lights::descriptor) + "/default";
    CHECK_EQ(AServiceManager_addService(service->asBinder().get(), name.c_str()), STATUS_OK);
    ABinderProcess_joinThreadPool();
    return 1;
}
