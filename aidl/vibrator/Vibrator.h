/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "HwApi.h"

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
#define DURATION_AMPLITUDE_LIGHT 0.65
#define DURATION_AMPLITUDE_MEDIUM 0.8
#define DURATION_AMPLITUDE_STRONG 1
#endif

#define COMMON_MAX_COMPOSE_DELAY (1000)
#define COMMON_MAX_COMPOSE_EFFECT (32)

using ::aidl::android::hardware::vibrator::Braking;
using ::aidl::android::hardware::vibrator::CompositeEffect;
using ::aidl::android::hardware::vibrator::CompositePrimitive;
using ::aidl::android::hardware::vibrator::Effect;
using ::aidl::android::hardware::vibrator::EffectStrength;
using ::aidl::android::hardware::vibrator::IVibratorCallback;
using ::aidl::android::hardware::vibrator::PrimitivePwle;

struct HapticEffectSpec {
    int16_t id;
    uint32_t durationMs;
};

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class Vibrator : public BnVibrator {
  public:
    Vibrator(std::shared_ptr<HwApi> hwApi = nullptr);
    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedPrimitives(
            std::vector<CompositePrimitive>* _aidl_return) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* _aidl_return) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* _aidl_return) override;
    ndk::ScopedAStatus getQFactor(float* _aidl_return) override;
    ndk::ScopedAStatus getFrequencyResolution(float* _aidl_return) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* _aidl_return) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* _aidl_return) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;

    std::shared_ptr<HwApi> getHwApi() const { return mHwApi; }

  private:
    ndk::ScopedAStatus activate(uint32_t ms);
    ndk::ScopedAStatus uploadFFEffect(std::vector<int16_t> effectData, int timeoutMs);
    static float strengthToAmplitude(EffectStrength strength, ndk::ScopedAStatus* status);
    int cirrusEffectId(int id);

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
    static float durationAmplitude(float amplitude);
    float mDurationAmplitude;
#endif

    std::shared_ptr<HwApi> mHwApi;
    bool mEnabled{false};
    bool mExternalControl{false};
    std::mutex mMutex;

    bool mIsTimedOutVibrator;
    bool mIsForceFeedbackVibrator{false};
    bool mUsesCommonFFInterface{false};
    bool mSupportsPrimitives{false};
    bool mHasTimedOutIntensity;
    bool mHasTimedOutEffect;
    bool mHasCpTriggerQueue{false};
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
