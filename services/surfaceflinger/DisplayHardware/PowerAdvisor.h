/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <unordered_set>

#include <utils/Mutex.h>

#include <android/hardware/power/IPower.h>
#include <ui/DisplayIdentification.h>
#include "../Scheduler/OneShotTimer.h"

using namespace std::chrono_literals;

namespace android {

class SurfaceFlinger;

namespace Hwc2 {

class PowerAdvisor {
public:
    virtual ~PowerAdvisor();

    // Initializes resources that cannot be initialized on construction
    virtual void init() = 0;
    virtual void onBootFinished() = 0;
    virtual void setExpensiveRenderingExpected(DisplayId displayId, bool expected) = 0;
    virtual bool isUsingExpensiveRendering() = 0;
    virtual void notifyDisplayUpdateImminent() = 0;
    virtual bool usePowerHintSession() = 0;
    virtual bool supportsPowerHintSession() = 0;
    virtual bool isPowerHintSessionRunning() = 0;
    virtual void setTargetWorkDuration(int64_t targetDurationNanos) = 0;
    virtual void sendActualWorkDuration(int64_t actualDurationNanos, nsecs_t timestamp) = 0;
    virtual void enablePowerHint(bool enabled) = 0;
    virtual bool startPowerHintSession(const std::vector<int32_t>& threadIds) = 0;
    virtual bool canNotifyDisplayUpdateImminent() = 0;
};

namespace impl {

// PowerAdvisor is a wrapper around IPower HAL which takes into account the
// full state of the system when sending out power hints to things like the GPU.
class PowerAdvisor final : public Hwc2::PowerAdvisor {
public:
    class HalWrapper {
    public:
        virtual ~HalWrapper() = default;

        virtual bool setExpensiveRendering(bool enabled) = 0;
        virtual bool notifyDisplayUpdateImminent() = 0;
        virtual bool supportsPowerHintSession() = 0;
        virtual bool isPowerHintSessionRunning() = 0;
        virtual void restartPowerHintSession() = 0;
        virtual void setPowerHintSessionThreadIds(const std::vector<int32_t>& threadIds) = 0;
        virtual bool startPowerHintSession() = 0;
        virtual void setTargetWorkDuration(int64_t targetDurationNanos) = 0;
        virtual void sendActualWorkDuration(int64_t actualDurationNanos,
                                            nsecs_t timeStampNanos) = 0;
        virtual bool shouldReconnectHAL() = 0;
        virtual std::vector<int32_t> getPowerHintSessionThreadIds() = 0;
        virtual std::optional<int64_t> getTargetWorkDuration() = 0;
    };

    PowerAdvisor(SurfaceFlinger& flinger);
    ~PowerAdvisor() override;

    void init() override;
    void onBootFinished() override;
    void setExpensiveRenderingExpected(DisplayId displayId, bool expected) override;
    bool isUsingExpensiveRendering() override { return mNotifiedExpensiveRendering; };
    void notifyDisplayUpdateImminent() override;
    bool usePowerHintSession() override;
    bool supportsPowerHintSession() override;
    bool isPowerHintSessionRunning() override;
    void setTargetWorkDuration(int64_t targetDurationNanos) override;
    void sendActualWorkDuration(int64_t actualDurationNanos, nsecs_t timestamp) override;
    void enablePowerHint(bool enabled) override;
    bool startPowerHintSession(const std::vector<int32_t>& threadIds) override;
    bool canNotifyDisplayUpdateImminent() override;

private:
    HalWrapper* getPowerHal() REQUIRES(mPowerHalMutex);
    bool mReconnectPowerHal GUARDED_BY(mPowerHalMutex) = false;
    std::mutex mPowerHalMutex;

    std::atomic_bool mBootFinished = false;
    std::optional<bool> mPowerHintEnabled;
    std::optional<bool> mSupportsPowerHint;
    bool mPowerHintSessionRunning = false;

    // An adjustable safety margin which moves the "target" earlier to allow flinger to
    // go a bit over without dropping a frame, especially since we can't measure
    // the exact time HWC finishes composition so "actual" durations are measured
    // from the end of present() instead, which is a bit later.
    static constexpr const std::chrono::nanoseconds kTargetSafetyMargin = 2ms;

    std::unordered_set<DisplayId> mExpensiveDisplays;
    bool mNotifiedExpensiveRendering = false;

    SurfaceFlinger& mFlinger;
    std::atomic_bool mSendUpdateImminent = true;
    std::atomic<nsecs_t> mLastScreenUpdatedTime = 0;
    std::optional<scheduler::OneShotTimer> mScreenUpdateTimer;
};

class AidlPowerHalWrapper : public PowerAdvisor::HalWrapper {
public:
    explicit AidlPowerHalWrapper(sp<hardware::power::IPower> powerHal);
    ~AidlPowerHalWrapper() override;

    static std::unique_ptr<HalWrapper> connect();

    bool setExpensiveRendering(bool enabled) override;
    bool notifyDisplayUpdateImminent() override;
    bool supportsPowerHintSession() override;
    bool isPowerHintSessionRunning() override;
    void restartPowerHintSession() override;
    void setPowerHintSessionThreadIds(const std::vector<int32_t>& threadIds) override;
    bool startPowerHintSession() override;
    void setTargetWorkDuration(int64_t targetDurationNanos) override;
    void sendActualWorkDuration(int64_t actualDurationNanos, nsecs_t timeStampNanos) override;
    bool shouldReconnectHAL() override;
    std::vector<int32_t> getPowerHintSessionThreadIds() override;
    std::optional<int64_t> getTargetWorkDuration() override;

private:
    bool checkPowerHintSessionSupported();
    void closePowerHintSession();
    bool shouldReportActualDurationsNow();
    bool shouldSetTargetDuration(int64_t targetDurationNanos);

    const sp<hardware::power::IPower> mPowerHal = nullptr;
    bool mHasExpensiveRendering = false;
    bool mHasDisplayUpdateImminent = false;
    // Used to indicate an error state and need for reconstruction
    bool mShouldReconnectHal = false;
    // This is not thread safe, but is currently protected by mPowerHalMutex so it needs no lock
    sp<hardware::power::IPowerHintSession> mPowerHintSession = nullptr;
    // Queue of actual durations saved to report
    std::vector<hardware::power::WorkDuration> mPowerHintQueue;
    // The latest un-normalized values we have received for target and actual
    int64_t mTargetDuration = kDefaultTarget.count();
    std::optional<int64_t> mActualDuration;
    // The list of thread ids, stored so we can restart the session from this class if needed
    std::vector<int32_t> mPowerHintThreadIds;
    bool mSupportsPowerHint;
    // Keep track of the last messages sent for rate limiter change detection
    std::optional<int64_t> mLastActualDurationSent;
    // timestamp of the last report we sent, used to avoid stale sessions
    int64_t mLastActualReportTimestamp = 0;
    int64_t mLastTargetDurationSent = kDefaultTarget.count();
    // Whether to normalize all the actual values as error terms relative to a constant target
    // This saves a binder call by not setting the target, and should not affect the pid values
    static const bool sNormalizeTarget;
    // Whether we should emit ATRACE_INT data for hint sessions
    static const bool sTraceHintSessionData;

    // Max percent the actual duration can vary without causing a report (eg: 0.1 = 10%)
    static constexpr double kAllowedActualDeviationPercent = 0.1;
    // Max percent the target duration can vary without causing a report (eg: 0.1 = 10%)
    static constexpr double kAllowedTargetDeviationPercent = 0.1;
    // Target used for init and normalization, the actual value does not really matter
    static constexpr const std::chrono::nanoseconds kDefaultTarget = 50ms;
    // Amount of time after the last message was sent before the session goes stale
    // actually 100ms but we use 80 here to ideally avoid going stale
    static constexpr const std::chrono::nanoseconds kStaleTimeout = 80ms;
};

} // namespace impl
} // namespace Hwc2
} // namespace android
