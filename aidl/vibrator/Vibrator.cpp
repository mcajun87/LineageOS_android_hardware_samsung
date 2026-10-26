/*
 * SPDX-FileCopyrightText: 2021-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"
extern "C" {
#include "owt.h"
}

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <cmath>
#include <format>
#include <map>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

const std::string kVibratorPropPrefix = "ro.vendor.vibrator_hal.";
const std::string kVibratorPropDuration = "_duration";

static std::map<Effect, HapticEffectSpec> CP_TRIGGER_EFFECTS{{Effect::CLICK, {10, 50}},
                                                             {Effect::DOUBLE_CLICK, {14, 250}},
                                                             {Effect::TICK, {50, 25}},
                                                             {Effect::HEAVY_CLICK, {23, 100}},
                                                             {Effect::TEXTURE_TICK, {50, 25}}};

static std::map<Effect, HapticEffectSpec> FF_EFFECT_IDS{{Effect::CLICK, {1, 50}},
                                                        {Effect::DOUBLE_CLICK, {5, 250}},
                                                        {Effect::TICK, {41, 25}},
                                                        {Effect::HEAVY_CLICK, {14, 100}},
                                                        {Effect::TEXTURE_TICK, {41, 25}}};

static std::map<CompositePrimitive, HapticEffectSpec> FF_PRIMITIVE_IDS{
        {CompositePrimitive::NOOP, {0, 0}},           {CompositePrimitive::CLICK, {1, 20}},
        {CompositePrimitive::THUD, {124, 300}},       {CompositePrimitive::SPIN, {123, 130}},
        {CompositePrimitive::QUICK_RISE, {121, 150}}, {CompositePrimitive::SLOW_RISE, {122, 500}},
        {CompositePrimitive::QUICK_FALL, {120, 100}}, {CompositePrimitive::LIGHT_TICK, {41, 20}},
        {CompositePrimitive::LOW_TICK, {119, 20}},
};

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
static std::map<EffectStrength, float> DURATION_AMPLITUDE = {
        {EffectStrength::LIGHT, DURATION_AMPLITUDE_LIGHT},
        {EffectStrength::MEDIUM, DURATION_AMPLITUDE_MEDIUM},
        {EffectStrength::STRONG, DURATION_AMPLITUDE_STRONG}};
#endif

static int getIntProperty(const std::string& key, int def) {
    return ::android::base::GetIntProperty(kVibratorPropPrefix + key, def);
}

Vibrator::Vibrator(std::shared_ptr<HwApi> hwApi)
    : mHwApi(hwApi ? std::move(hwApi) : std::make_shared<HwApi>()) {
    mIsForceFeedbackVibrator = mHwApi->hasForceFeedback();
    mIsTimedOutVibrator = mHwApi->isVibratorNodeExist() || mHwApi->isUsingLeds();
    mHasTimedOutIntensity = mHwApi->isAmplitudeControlExist();
    mHasTimedOutEffect = mHwApi->isNodeExist(mHwApi->getCpTriggerIndexNode());

    if (mIsTimedOutVibrator) {
        CP_TRIGGER_EFFECTS[Effect::CLICK].durationMs =
                getIntProperty("click" + kVibratorPropDuration, 10);
        CP_TRIGGER_EFFECTS[Effect::TICK].durationMs =
                getIntProperty("tick" + kVibratorPropDuration, 5);
        CP_TRIGGER_EFFECTS[Effect::TEXTURE_TICK].durationMs =
                getIntProperty("texture_tick" + kVibratorPropDuration, 5);
    }

    if (mIsForceFeedbackVibrator) {
        mUsesCommonFFInterface = mHwApi->hasFeature(COMMON_INPUTFF_INTERFACE);
        mSupportsPrimitives = mHwApi->hasFeature(PRIMITIVE_EFFECT_COMPOSE);
    }
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_EXTERNAL_CONTROL;

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
    *_aidl_return |= IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_EXTERNAL_AMPLITUDE_CONTROL;
#else
    if (mHasTimedOutIntensity)
        *_aidl_return |=
                IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_EXTERNAL_AMPLITUDE_CONTROL;
#endif

    if (mIsForceFeedbackVibrator) {
        *_aidl_return |= IVibrator::CAP_AMPLITUDE_CONTROL;
        if (mSupportsPrimitives) {
            *_aidl_return |= IVibrator::CAP_COMPOSE_EFFECTS;
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    return activate(0);
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    ndk::ScopedAStatus status;

    if (mHasTimedOutEffect) {
        mHwApi->writeNode(mHwApi->getCpTriggerIndexNode(), 0);  // Clear all effects
    }

    if (mIsForceFeedbackVibrator) {
        uploadFFEffect({0}, timeoutMs);
    }

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
    timeoutMs *= mDurationAmplitude;
#endif

    status = activate(timeoutMs);

    if (callback != nullptr) {
        std::thread([=] {
            LOG(DEBUG) << "Starting on on another thread";
            usleep(timeoutMs * 1000);
            LOG(DEBUG) << "Notifying on complete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Failed to call onComplete";
            }
        }).detach();
    }

    return status;
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    ndk::ScopedAStatus status;
    float amplitude = strengthToAmplitude(strength, &status);
    uint32_t ms = 1000;

    if (!status.isOk()) {
        return status;
    }

    if (mIsTimedOutVibrator) {
        activate(0);
    }

    setAmplitude(amplitude);

    if (mIsForceFeedbackVibrator) {
        HapticEffectSpec effectSpec = FF_EFFECT_IDS[effect];
        if (mUsesCommonFFInterface) {
            uploadFFEffect({effectSpec.id}, 0);
        } else {
            uploadFFEffect({0, effectSpec.id}, 0);
        }
        ms = effectSpec.durationMs;
    } else if (mHasTimedOutEffect && CP_TRIGGER_EFFECTS.contains(effect)) {
        mHwApi->writeNode(mHwApi->getCpTriggerIndexNode(), CP_TRIGGER_EFFECTS[effect].id);
        ms = CP_TRIGGER_EFFECTS[effect].durationMs;
    } else {
        if (effect == Effect::DOUBLE_CLICK) {
            ms = 134;
            std::thread([this, callback]() {
                activate(17);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                activate(17);
                if (callback != nullptr) {
                    callback->onComplete();
                }
            }).detach();
            *_aidl_return = ms;
            return ndk::ScopedAStatus::ok();
        }
        if (effect == Effect::HEAVY_CLICK) {
            ms = 100;
        } else {
            ms = CP_TRIGGER_EFFECTS[effect].durationMs;
        }
        *_aidl_return = ms;
#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
        *_aidl_return *= DURATION_AMPLITUDE[strength];
#endif
        return on(ms, callback);
    }

    status = activate(ms);

    if (callback != nullptr) {
        std::thread([=] {
            LOG(DEBUG) << "Starting perform on another thread";
            usleep(ms * 1000);
            LOG(DEBUG) << "Notifying perform complete";
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = ms;
    return status;
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK, Effect::HEAVY_CLICK,
                     Effect::TEXTURE_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    uint32_t intensity;

    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
    mDurationAmplitude = durationAmplitude(amplitude);
#endif

    LOG(DEBUG) << "Setting amplitude: " << amplitude;

    intensity = amplitude * INTENSITY_MAX;

#ifndef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
    LOG(DEBUG) << "Setting intensity: " << intensity;

    if (mHasTimedOutIntensity) {
        return mHwApi->setAmplitude(intensity)
                       ? ndk::ScopedAStatus::ok()
                       : ndk::ScopedAStatus::fromStatus(STATUS_UNKNOWN_ERROR);
    }

    if (mIsForceFeedbackVibrator) {
        if (!mHwApi->setForceFeedbackAmplitude(intensity))
            return ndk::ScopedAStatus::fromExceptionCode(STATUS_UNKNOWN_ERROR);
        return ndk::ScopedAStatus::ok();
    }
#endif

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled) {
    if (mEnabled) {
        LOG(WARNING) << "Setting external control while the vibrator is enabled is unsupported!";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    LOG(INFO) << "ExternalControl: " << mExternalControl << " -> " << enabled;
    mExternalControl = enabled;
    if (mHwApi->isNodeExist(mHwApi->getExtControlNode())) {
        mHwApi->writeNode(mHwApi->getExtControlNode(), enabled ? 1 : 0);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* _aidl_return) {
    if (!mSupportsPrimitives)
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    *_aidl_return = (mUsesCommonFFInterface ? COMMON_MAX_COMPOSE_DELAY : WT_MAX_DELAY);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* _aidl_return) {
    if (!mSupportsPrimitives)
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    *_aidl_return = (mUsesCommonFFInterface ? COMMON_MAX_COMPOSE_EFFECT / 2 : WT_MAX_SECTIONS - 1);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* _aidl_return) {
    if (!mSupportsPrimitives)
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    for (auto primitive : FF_PRIMITIVE_IDS) _aidl_return->push_back(primitive.first);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* _aidl_return) {
    if (!mSupportsPrimitives || (FF_PRIMITIVE_IDS.find(primitive) == FF_PRIMITIVE_IDS.end()))
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    *_aidl_return = FF_PRIMITIVE_IDS.at(primitive).durationMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (!mSupportsPrimitives)
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    int32_t maxCompositeSize;
    if (composite.empty() || !getCompositionSizeMax(&maxCompositeSize).isOk() ||
        composite.size() > static_cast<size_t>(maxCompositeSize))
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

    int totalDuration = 0;
    std::string owt_effect_str;
    struct common_inputff_effects common_composite = {};
    int32_t maxDelay;
    getCompositionDelayMax(&maxDelay);
    for (auto segment : composite) {
        if (segment.delayMs < 0 || segment.delayMs > maxDelay)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

        if (!FF_PRIMITIVE_IDS.contains(segment.primitive) || segment.scale < 0 || segment.scale > 1)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

        if (segment.primitive == CompositePrimitive::NOOP) {
            // No haptic effect. Used to generate extended delays between primitives.
            if (segment.delayMs > 0) {
                if (mUsesCommonFFInterface) {
                    common_composite.packets[common_composite.count++] = {0, 0, 0, segment.delayMs,
                                                                          0};
                } else {
                    owt_effect_str.append(std::format("{} ", segment.delayMs));
                }
            }
            totalDuration += segment.delayMs;
            continue;
        }

        // Consider 15% the lowest "feelable" amplitude
        int scale = round(segment.scale * 85 + 15);

        int id = FF_PRIMITIVE_IDS[segment.primitive].id;

        int primitiveDuration = 0;
        getPrimitiveDuration(segment.primitive, &primitiveDuration);
        totalDuration += primitiveDuration + segment.delayMs;

        if (mUsesCommonFFInterface) {
            if (segment.delayMs > 0) {
                common_composite.packets[common_composite.count++] = {0, 0, 0, segment.delayMs, 0};
            }
            common_composite.packets[common_composite.count++] = {FF_PERIODIC, id, scale,
                                                                  primitiveDuration, 0};
        } else {  // Cirrus OWT
            owt_effect_str.append(
                    segment.delayMs == 0
                            ? std::format("{}.{},", cirrusEffectId(id), scale)
                            : std::format("{} {}.{},", segment.delayMs, cirrusEffectId(id), scale));
        }
    }

    setAmplitude(1);

    std::vector<int16_t> effectData;
    if (mUsesCommonFFInterface) {
        auto data = (int16_t*)&common_composite;
        effectData.assign(data, data + sizeof(common_composite) / sizeof(int16_t));
    } else {
        int16_t data[WT_TYPE12_PWLE_SINGLE_PACKED_MAX / 2];
        int num_bytes = get_owt_data(owt_effect_str.data(), (uint8_t*)data);
        if (!num_bytes) return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
        effectData.assign(data, data + num_bytes);
    }
    uploadFFEffect(effectData, 0);
    activate(1);
    if (callback != nullptr) {
        std::thread([=] {
            usleep(totalDuration * 1000);
            callback->onComplete();
        }).detach();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t /*id*/, Effect /*effect*/,
                                            EffectStrength /*strength*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t /*id*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getQFactor(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& /*composite*/,
                                         const std::shared_ptr<IVibratorCallback>& /*callback*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::activate(uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock{mMutex};
    if (mIsTimedOutVibrator) {
        return (timeoutMs > 0 ? mHwApi->do_on(timeoutMs) : mHwApi->do_off())
                       ? ndk::ScopedAStatus::ok()
                       : ndk::ScopedAStatus::fromStatus(STATUS_UNKNOWN_ERROR);
    }
    if (mIsForceFeedbackVibrator) {
        if (!mHwApi->triggerForceFeedbackVibration(timeoutMs != 0))
            return ndk::ScopedAStatus::fromExceptionCode(STATUS_UNKNOWN_ERROR);
        return ndk::ScopedAStatus::ok();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::uploadFFEffect(std::vector<int16_t> effectData, int timeoutMs) {
    bool isConstant =
            (mUsesCommonFFInterface && effectData[0] == 0 && effectData.size() <= sizeof(int));
    if (!mHwApi->uploadForceFeedbackEffect(effectData, timeoutMs, isConstant)) {
        return ndk::ScopedAStatus::fromStatus(STATUS_UNKNOWN_ERROR);
    }
    return ndk::ScopedAStatus::ok();
}

float Vibrator::strengthToAmplitude(EffectStrength strength, ndk::ScopedAStatus* status) {
    *status = ndk::ScopedAStatus::ok();

    switch (strength) {
        case EffectStrength::LIGHT:
            return 0.25f;
        case EffectStrength::MEDIUM:
            return 0.5f;
        case EffectStrength::STRONG:
            return 1.0f;
    }

    *status = ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    return 0;
}

int Vibrator::cirrusEffectId(int id) {
    /* drivers/vibrator/cs/cs40l26/cs40l26.c - cs40l26_index_mapping */
    switch (id) {
        case 0:  // NOOP effect
        case 1:  // CLICK effect
        case 100:
            break;
        case 119 ... 124:
            id += 16;
            break;
        case 126 ... 127:
            id += 15;
            break;
        default:
            id += 9;
            break;
    }
    return id;
}

#ifdef VIBRATOR_SUPPORTS_DURATION_AMPLITUDE_CONTROL
float Vibrator::durationAmplitude(float amplitude) {
    if (amplitude == 1) {
        return DURATION_AMPLITUDE_STRONG;
    } else if (amplitude >= 0.5) {
        return DURATION_AMPLITUDE_MEDIUM;
    }

    return DURATION_AMPLITUDE_LIGHT;
}
#endif

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
