/*
 * Copyright 2019 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"
#define LOG_NDEBUG 0

#include "Scheduler/TimeKeeper.h"
#include "Scheduler/VSyncDispatch.h"
#include "Scheduler/VSyncReactor.h"
#include "Scheduler/VSyncTracker.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <ui/Fence.h>
#include <ui/FenceTime.h>
#include <array>

using namespace testing;
using namespace std::literals;
namespace android::scheduler {

class MockVSyncTracker : public VSyncTracker {
public:
    MOCK_METHOD1(addVsyncTimestamp, void(nsecs_t));
    MOCK_CONST_METHOD1(nextAnticipatedVSyncTimeFrom, nsecs_t(nsecs_t));
    MOCK_CONST_METHOD0(currentPeriod, nsecs_t());
    MOCK_METHOD1(setPeriod, void(nsecs_t));
};

class VSyncTrackerWrapper : public VSyncTracker {
public:
    VSyncTrackerWrapper(std::shared_ptr<VSyncTracker> const& tracker) : mTracker(tracker) {}

    void addVsyncTimestamp(nsecs_t timestamp) final { mTracker->addVsyncTimestamp(timestamp); }
    nsecs_t nextAnticipatedVSyncTimeFrom(nsecs_t timePoint) const final {
        return mTracker->nextAnticipatedVSyncTimeFrom(timePoint);
    }
    nsecs_t currentPeriod() const final { return mTracker->currentPeriod(); }
    void setPeriod(nsecs_t period) { mTracker->setPeriod(period); }

private:
    std::shared_ptr<VSyncTracker> const mTracker;
};

class MockClock : public Clock {
public:
    MOCK_CONST_METHOD0(now, nsecs_t());
};

class ClockWrapper : public Clock {
public:
    ClockWrapper(std::shared_ptr<Clock> const& clock) : mClock(clock) {}

    nsecs_t now() const { return mClock->now(); }

private:
    std::shared_ptr<Clock> const mClock;
};

class MockVSyncDispatch : public VSyncDispatch {
public:
    MOCK_METHOD2(registerCallback, CallbackToken(std::function<void(nsecs_t)> const&, std::string));
    MOCK_METHOD1(unregisterCallback, void(CallbackToken));
    MOCK_METHOD3(schedule, ScheduleResult(CallbackToken, nsecs_t, nsecs_t));
    MOCK_METHOD1(cancel, CancelResult(CallbackToken token));
};

class VSyncDispatchWrapper : public VSyncDispatch {
public:
    VSyncDispatchWrapper(std::shared_ptr<VSyncDispatch> const& dispatch) : mDispatch(dispatch) {}
    CallbackToken registerCallback(std::function<void(nsecs_t)> const& callbackFn,
                                   std::string callbackName) final {
        return mDispatch->registerCallback(callbackFn, callbackName);
    }

    void unregisterCallback(CallbackToken token) final { mDispatch->unregisterCallback(token); }

    ScheduleResult schedule(CallbackToken token, nsecs_t workDuration,
                            nsecs_t earliestVsync) final {
        return mDispatch->schedule(token, workDuration, earliestVsync);
    }

    CancelResult cancel(CallbackToken token) final { return mDispatch->cancel(token); }

private:
    std::shared_ptr<VSyncDispatch> const mDispatch;
};

std::shared_ptr<FenceTime> generateInvalidFence() {
    sp<Fence> fence = new Fence();
    return std::make_shared<FenceTime>(fence);
}

std::shared_ptr<FenceTime> generatePendingFence() {
    sp<Fence> fence = new Fence(dup(fileno(tmpfile())));
    return std::make_shared<FenceTime>(fence);
}

void signalFenceWithTime(std::shared_ptr<FenceTime> const& fence, nsecs_t time) {
    FenceTime::Snapshot snap(time);
    fence->applyTrustedSnapshot(snap);
}

std::shared_ptr<FenceTime> generateSignalledFenceWithTime(nsecs_t time) {
    sp<Fence> fence = new Fence(dup(fileno(tmpfile())));
    std::shared_ptr<FenceTime> ft = std::make_shared<FenceTime>(fence);
    signalFenceWithTime(ft, time);
    return ft;
}

class StubCallback : public DispSync::Callback {
public:
    void onDispSyncEvent(nsecs_t when) final {
        std::lock_guard<std::mutex> lk(mMutex);
        mLastCallTime = when;
    }
    std::optional<nsecs_t> lastCallTime() const {
        std::lock_guard<std::mutex> lk(mMutex);
        return mLastCallTime;
    }

private:
    std::mutex mutable mMutex;
    std::optional<nsecs_t> mLastCallTime GUARDED_BY(mMutex);
};

class VSyncReactorTest : public testing::Test {
protected:
    VSyncReactorTest()
          : mMockDispatch(std::make_shared<NiceMock<MockVSyncDispatch>>()),
            mMockTracker(std::make_shared<NiceMock<MockVSyncTracker>>()),
            mMockClock(std::make_shared<NiceMock<MockClock>>()),
            mReactor(std::make_unique<ClockWrapper>(mMockClock),
                     std::make_unique<VSyncDispatchWrapper>(mMockDispatch),
                     std::make_unique<VSyncTrackerWrapper>(mMockTracker), kPendingLimit) {
        ON_CALL(*mMockClock, now()).WillByDefault(Return(mFakeNow));
        ON_CALL(*mMockTracker, currentPeriod()).WillByDefault(Return(period));
    }

    std::shared_ptr<MockVSyncDispatch> mMockDispatch;
    std::shared_ptr<MockVSyncTracker> mMockTracker;
    std::shared_ptr<MockClock> mMockClock;
    static constexpr size_t kPendingLimit = 3;
    static constexpr nsecs_t mDummyTime = 47;
    static constexpr nsecs_t mPhase = 3000;
    static constexpr nsecs_t mAnotherPhase = 5200;
    static constexpr nsecs_t period = 10000;
    static constexpr nsecs_t mAnotherPeriod = 23333;
    static constexpr nsecs_t mFakeCbTime = 2093;
    static constexpr nsecs_t mFakeNow = 2214;
    static constexpr const char mName[] = "callbacky";
    VSyncDispatch::CallbackToken const mFakeToken{2398};

    nsecs_t lastCallbackTime = 0;
    StubCallback outerCb;
    std::function<void(nsecs_t)> innerCb;

    VSyncReactor mReactor;
};

TEST_F(VSyncReactorTest, addingNullFenceCheck) {
    EXPECT_FALSE(mReactor.addPresentFence(nullptr));
}

TEST_F(VSyncReactorTest, addingInvalidFenceSignalsNeedsMoreInfo) {
    EXPECT_TRUE(mReactor.addPresentFence(generateInvalidFence()));
}

TEST_F(VSyncReactorTest, addingSignalledFenceAddsToTracker) {
    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(mDummyTime));
    EXPECT_FALSE(mReactor.addPresentFence(generateSignalledFenceWithTime(mDummyTime)));
}

TEST_F(VSyncReactorTest, addingPendingFenceAddsSignalled) {
    nsecs_t anotherDummyTime = 2919019201;

    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(_)).Times(0);
    auto pendingFence = generatePendingFence();
    EXPECT_FALSE(mReactor.addPresentFence(pendingFence));
    Mock::VerifyAndClearExpectations(mMockTracker.get());

    signalFenceWithTime(pendingFence, mDummyTime);

    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(mDummyTime));
    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(anotherDummyTime));
    EXPECT_FALSE(mReactor.addPresentFence(generateSignalledFenceWithTime(anotherDummyTime)));
}

TEST_F(VSyncReactorTest, limitsPendingFences) {
    std::array<std::shared_ptr<FenceTime>, kPendingLimit * 2> fences;
    std::array<nsecs_t, fences.size()> fakeTimes;
    std::generate(fences.begin(), fences.end(), [] { return generatePendingFence(); });
    std::generate(fakeTimes.begin(), fakeTimes.end(), [i = 10]() mutable {
        i++;
        return i * i;
    });

    for (auto const& fence : fences) {
        mReactor.addPresentFence(fence);
    }

    for (auto i = fences.size() - kPendingLimit; i < fences.size(); i++) {
        EXPECT_CALL(*mMockTracker, addVsyncTimestamp(fakeTimes[i]));
    }

    for (auto i = 0u; i < fences.size(); i++) {
        signalFenceWithTime(fences[i], fakeTimes[i]);
    }
    mReactor.addPresentFence(generatePendingFence());
}

TEST_F(VSyncReactorTest, ignoresPresentFencesWhenToldTo) {
    static constexpr size_t aFewTimes = 8;
    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(mDummyTime)).Times(1);

    mReactor.setIgnorePresentFences(true);
    for (auto i = 0; i < aFewTimes; i++) {
        mReactor.addPresentFence(generateSignalledFenceWithTime(mDummyTime));
    }

    mReactor.setIgnorePresentFences(false);
    EXPECT_FALSE(mReactor.addPresentFence(generateSignalledFenceWithTime(mDummyTime)));
}

TEST_F(VSyncReactorTest, queriesTrackerForNextRefreshNow) {
    nsecs_t const fakeTimestamp = 4839;
    EXPECT_CALL(*mMockTracker, currentPeriod()).Times(0);
    EXPECT_CALL(*mMockTracker, nextAnticipatedVSyncTimeFrom(_))
            .Times(1)
            .WillOnce(Return(fakeTimestamp));

    EXPECT_THAT(mReactor.computeNextRefresh(0), Eq(fakeTimestamp));
}

TEST_F(VSyncReactorTest, queriesTrackerForExpectedPresentTime) {
    nsecs_t const fakeTimestamp = 4839;
    EXPECT_CALL(*mMockTracker, currentPeriod()).Times(0);
    EXPECT_CALL(*mMockTracker, nextAnticipatedVSyncTimeFrom(_))
            .Times(1)
            .WillOnce(Return(fakeTimestamp));

    EXPECT_THAT(mReactor.expectedPresentTime(), Eq(fakeTimestamp));
}

TEST_F(VSyncReactorTest, queriesTrackerForNextRefreshFuture) {
    nsecs_t const fakeTimestamp = 4839;
    nsecs_t const fakePeriod = 1010;
    nsecs_t const mFakeNow = 2214;
    int const numPeriodsOut = 3;
    EXPECT_CALL(*mMockClock, now()).WillOnce(Return(mFakeNow));
    EXPECT_CALL(*mMockTracker, currentPeriod()).WillOnce(Return(fakePeriod));
    EXPECT_CALL(*mMockTracker, nextAnticipatedVSyncTimeFrom(mFakeNow + numPeriodsOut * fakePeriod))
            .WillOnce(Return(fakeTimestamp));
    EXPECT_THAT(mReactor.computeNextRefresh(numPeriodsOut), Eq(fakeTimestamp));
}

TEST_F(VSyncReactorTest, getPeriod) {
    nsecs_t const fakePeriod = 1010;
    EXPECT_CALL(*mMockTracker, currentPeriod()).WillOnce(Return(fakePeriod));
    EXPECT_THAT(mReactor.getPeriod(), Eq(fakePeriod));
}

TEST_F(VSyncReactorTest, setPeriod) {
    nsecs_t const fakePeriod = 4098;
    EXPECT_CALL(*mMockTracker, setPeriod(fakePeriod));
    mReactor.setPeriod(fakePeriod);
}

TEST_F(VSyncReactorTest, addResyncSampleTypical) {
    nsecs_t const fakeTimestamp = 3032;
    bool periodFlushed = false;

    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(fakeTimestamp));
    EXPECT_FALSE(mReactor.addResyncSample(fakeTimestamp, &periodFlushed));
    EXPECT_FALSE(periodFlushed);
}

TEST_F(VSyncReactorTest, addResyncSamplePeriodChanges) {
    bool periodFlushed = false;
    nsecs_t const fakeTimestamp = 4398;
    nsecs_t const newPeriod = 3490;
    EXPECT_CALL(*mMockTracker, addVsyncTimestamp(fakeTimestamp));
    mReactor.setPeriod(newPeriod);
    EXPECT_FALSE(mReactor.addResyncSample(fakeTimestamp, &periodFlushed));
    EXPECT_TRUE(periodFlushed);
}

static nsecs_t computeWorkload(nsecs_t period, nsecs_t phase) {
    return period - phase;
}

TEST_F(VSyncReactorTest, addEventListener) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, cancel(mFakeToken)).Times(2).InSequence(seq);
    EXPECT_CALL(*mMockDispatch, unregisterCallback(mFakeToken)).InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.removeEventListener(&outerCb, &lastCallbackTime);
}

TEST_F(VSyncReactorTest, addEventListenerTwiceChangesPhase) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch,
                schedule(mFakeToken, computeWorkload(period, mAnotherPhase), _)) // mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, cancel(mFakeToken)).InSequence(seq);
    EXPECT_CALL(*mMockDispatch, unregisterCallback(mFakeToken)).InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.addEventListener(mName, mAnotherPhase, &outerCb, lastCallbackTime);
}

TEST_F(VSyncReactorTest, eventListenerGetsACallbackAndReschedules) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeCbTime))
            .Times(2)
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, cancel(mFakeToken)).InSequence(seq);
    EXPECT_CALL(*mMockDispatch, unregisterCallback(mFakeToken)).InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    innerCb(mFakeCbTime);
    innerCb(mFakeCbTime);
}

TEST_F(VSyncReactorTest, callbackTimestampReadapted) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, _))
            .InSequence(seq)
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeCbTime))
            .InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    innerCb(mFakeCbTime);
    EXPECT_THAT(outerCb.lastCallTime(), Optional(mFakeCbTime - period));
}

TEST_F(VSyncReactorTest, eventListenersRemovedOnDestruction) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, cancel(mFakeToken)).InSequence(seq);
    EXPECT_CALL(*mMockDispatch, unregisterCallback(mFakeToken)).InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
}

TEST_F(VSyncReactorTest, addEventListenerChangePeriod) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch,
                schedule(mFakeToken, computeWorkload(period, mAnotherPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockDispatch, cancel(mFakeToken)).InSequence(seq);
    EXPECT_CALL(*mMockDispatch, unregisterCallback(mFakeToken)).InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.addEventListener(mName, mAnotherPhase, &outerCb, lastCallbackTime);
}

TEST_F(VSyncReactorTest, changingPeriodChangesOfsetsOnNextCb) {
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch, schedule(mFakeToken, computeWorkload(period, mPhase), mFakeNow))
            .InSequence(seq);
    EXPECT_CALL(*mMockTracker, setPeriod(mAnotherPeriod));
    EXPECT_CALL(*mMockDispatch,
                schedule(mFakeToken, computeWorkload(mAnotherPeriod, mPhase), mFakeNow))
            .InSequence(seq);

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.setPeriod(mAnotherPeriod);
    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
}

TEST_F(VSyncReactorTest, negativeOffsetsApplied) {
    nsecs_t const negativePhase = -4000;
    Sequence seq;
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .InSequence(seq)
            .WillOnce(Return(mFakeToken));
    EXPECT_CALL(*mMockDispatch,
                schedule(mFakeToken, computeWorkload(period, negativePhase), mFakeNow))
            .InSequence(seq);
    mReactor.addEventListener(mName, negativePhase, &outerCb, lastCallbackTime);
}

using VSyncReactorDeathTest = VSyncReactorTest;
TEST_F(VSyncReactorDeathTest, invalidRemoval) {
    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.removeEventListener(&outerCb, &lastCallbackTime);
    EXPECT_DEATH(mReactor.removeEventListener(&outerCb, &lastCallbackTime), ".*");
}

TEST_F(VSyncReactorDeathTest, invalidChange) {
    EXPECT_DEATH(mReactor.changePhaseOffset(&outerCb, mPhase), ".*");

    // the current DispSync-interface usage pattern has evolved around an implementation quirk,
    // which is a callback is assumed to always exist, and it is valid api usage to change the
    // offset of an object that is in the removed state.
    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    mReactor.removeEventListener(&outerCb, &lastCallbackTime);
    mReactor.changePhaseOffset(&outerCb, mPhase);
}

} // namespace android::scheduler
