// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace t380::power {

class GpuInteractionPolicy {
  public:
    using WriteFloor = std::function<bool(int)>;
    using RestoreFailure = std::function<void()>;

    struct Config {
        int baselineFloor = 4;
        int boostFloor = 0;
        std::chrono::milliseconds defaultInteraction{1000};
        std::chrono::milliseconds maxInteraction{5000};
        std::chrono::milliseconds maxLaunch{5000};
    };

    GpuInteractionPolicy(WriteFloor writeFloor, RestoreFailure onRestoreFailure);
    GpuInteractionPolicy(WriteFloor writeFloor, RestoreFailure onRestoreFailure, Config config);
    ~GpuInteractionPolicy();

    GpuInteractionPolicy(const GpuInteractionPolicy&) = delete;
    GpuInteractionPolicy& operator=(const GpuInteractionPolicy&) = delete;

    bool start();
    bool setLaunch(bool enabled);
    bool setInteraction(int32_t durationMs);
    bool healthy() const;
    bool shutdown();

  private:
    using Clock = std::chrono::steady_clock;

    bool applyLocked(Clock::time_point now);
    void expireLocked(Clock::time_point now);
    void workerLoop();

    const WriteFloor mWriteFloor;
    const RestoreFailure mOnRestoreFailure;
    const Config mConfig;
    mutable std::mutex mMutex;
    std::condition_variable mCondition;
    std::thread mWorker;
    bool mStarted = false;
    bool mStop = false;
    bool mHealthy = true;
    bool mBoosted = false;
    bool mLaunchActive = false;
    Clock::time_point mLaunchDeadline = Clock::time_point::min();
    Clock::time_point mInteractionDeadline = Clock::time_point::min();
};

}  // namespace t380::power
