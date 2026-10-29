// SPDX-License-Identifier: Apache-2.0

#include "GpuInteractionPolicy.h"

#include <algorithm>
#include <utility>

namespace t380::power {

GpuInteractionPolicy::GpuInteractionPolicy(WriteFloor writeFloor,
                                           RestoreFailure onRestoreFailure)
    : GpuInteractionPolicy(std::move(writeFloor), std::move(onRestoreFailure), Config{}) {}

GpuInteractionPolicy::GpuInteractionPolicy(WriteFloor writeFloor,
                                           RestoreFailure onRestoreFailure, Config config)
    : mWriteFloor(std::move(writeFloor)),
      mOnRestoreFailure(std::move(onRestoreFailure)),
      mConfig(config) {}

GpuInteractionPolicy::~GpuInteractionPolicy() {
    shutdown();
}

bool GpuInteractionPolicy::start() {
    std::lock_guard lock(mMutex);
    if (mStarted) return mHealthy;
    if (!mWriteFloor || !mOnRestoreFailure || !mWriteFloor(mConfig.baselineFloor)) {
        mHealthy = false;
        return false;
    }
    mStarted = true;
    mWorker = std::thread(&GpuInteractionPolicy::workerLoop, this);
    return true;
}

bool GpuInteractionPolicy::setLaunch(bool enabled) {
    std::lock_guard lock(mMutex);
    if (!mStarted || !mHealthy) return false;
    const auto now = Clock::now();
    mLaunchActive = enabled;
    mLaunchDeadline = enabled ? now + mConfig.maxLaunch : Clock::time_point::min();
    const bool result = applyLocked(now);
    mCondition.notify_all();
    return result;
}

bool GpuInteractionPolicy::setInteraction(int32_t durationMs) {
    std::lock_guard lock(mMutex);
    if (!mStarted || !mHealthy) return false;
    const auto now = Clock::now();
    if (durationMs < 0) {
        mInteractionDeadline = Clock::time_point::min();
    } else {
        auto duration = durationMs == 0 ? mConfig.defaultInteraction
                                        : std::chrono::milliseconds(durationMs);
        duration = std::min(duration, mConfig.maxInteraction);
        mInteractionDeadline = std::max(mInteractionDeadline, now + duration);
    }
    const bool result = applyLocked(now);
    mCondition.notify_all();
    return result;
}

bool GpuInteractionPolicy::healthy() const {
    std::lock_guard lock(mMutex);
    return mStarted && mHealthy;
}

bool GpuInteractionPolicy::shutdown() {
    {
        std::lock_guard lock(mMutex);
        if (!mStarted) return mWriteFloor && mWriteFloor(mConfig.baselineFloor);
        mStop = true;
        mLaunchActive = false;
        mLaunchDeadline = Clock::time_point::min();
        mInteractionDeadline = Clock::time_point::min();
        mCondition.notify_all();
    }
    if (mWorker.joinable()) mWorker.join();
    const bool restored = mWriteFloor(mConfig.baselineFloor);
    std::lock_guard lock(mMutex);
    mStarted = false;
    mBoosted = false;
    mHealthy = mHealthy && restored;
    return restored;
}

void GpuInteractionPolicy::expireLocked(Clock::time_point now) {
    if (mLaunchActive && now >= mLaunchDeadline) {
        mLaunchActive = false;
        mLaunchDeadline = Clock::time_point::min();
    }
    if (now >= mInteractionDeadline) mInteractionDeadline = Clock::time_point::min();
}

bool GpuInteractionPolicy::applyLocked(Clock::time_point now) {
    expireLocked(now);
    const bool shouldBoost = mLaunchActive || now < mInteractionDeadline;
    if (shouldBoost == mBoosted) return true;
    if (mWriteFloor(shouldBoost ? mConfig.boostFloor : mConfig.baselineFloor)) {
        mBoosted = shouldBoost;
        return true;
    }

    // Abandon leases and retry the board default once. If both restoration
    // writes fail, the process must fail so init's synchronous onrestart
    // action gets its own restoration opportunity.
    mHealthy = false;
    mLaunchActive = false;
    mLaunchDeadline = Clock::time_point::min();
    mInteractionDeadline = Clock::time_point::min();
    if (mWriteFloor(mConfig.baselineFloor)) {
        mBoosted = false;
        return false;
    }
    mStop = true;
    mCondition.notify_all();
    mOnRestoreFailure();
    return false;
}

void GpuInteractionPolicy::workerLoop() {
    std::unique_lock lock(mMutex);
    while (!mStop) {
        const auto now = Clock::now();
        applyLocked(now);
        if (mStop) break;

        auto deadline = Clock::time_point::max();
        if (mLaunchActive) deadline = std::min(deadline, mLaunchDeadline);
        if (mInteractionDeadline != Clock::time_point::min()) {
            deadline = std::min(deadline, mInteractionDeadline);
        }
        if (deadline == Clock::time_point::max()) {
            mCondition.wait(lock);
        } else {
            mCondition.wait_until(lock, deadline);
        }
    }
}

}  // namespace t380::power
