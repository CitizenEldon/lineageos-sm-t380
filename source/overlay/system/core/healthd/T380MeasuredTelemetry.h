// Scoped read-only SM-T380 telemetry contract. No charging authority.
#pragma once
#include <cstdio>
#include <cstdint>
#include <ctime>
namespace android {
struct T380MeasuredTelemetry {
    uint64_t sequence = 0, sampledMs = 0, temperatureMs = 0;
    unsigned valid = 0, temperatureValid = 0;
    int cause = 0, error = 0, capacity = -1, voltageUv = 0, currentUa = 0;
    int temperature = 0, status = 1;
    static constexpr const char* PATH =
        "/sys/devices/platform/i2c-fuelgauge/i2c-7/7-0071/android_telemetry";
    // Covers the kernel's30-second autonomous sleep poll and resume latency.
    bool parse(const char* text, uint64_t nowMs) {
        unsigned long long seq, sampled, tempMs;
        char extra;
        int count = std::sscanf(text,
            "t380_measured_v1=1 seq=%llu sampled_ms=%llu valid=%u cause=%d error=%d capacity=%d voltage_uv=%d current_ua=%d temp_valid=%u temp=%d temp_ms=%llu status=%d %c",
            &seq, &sampled, &valid, &cause, &error, &capacity, &voltageUv,
            &currentUa, &temperatureValid, &temperature, &tempMs, &status, &extra);
        if (count != 12 || valid > 1 || temperatureValid > 1 || cause < 0 || cause > 4 ||
            error > 0 || status < 0 || status > 4 ||
            (valid && (!seq || cause || error || capacity < 0 || capacity > 100))) return false;
        sequence = seq; sampledMs = sampled; temperatureMs = tempMs;
        valid = valid && nowMs >= sampledMs && nowMs - sampledMs <= 65000;
        temperatureValid = temperatureValid && nowMs >= temperatureMs &&
            nowMs - temperatureMs <= 65000;
        return true;
    }
};
}
