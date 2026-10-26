/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "VibratorManager.h"

#include <android-base/logging.h>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

VibratorManager::VibratorManager(std::shared_ptr<IVibrator> vibrator)
    : mDefaultVibrator(std::move(vibrator)) {}

ndk::ScopedAStatus VibratorManager::getCapabilities(int32_t* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager getCapabilities";
    *_aidl_return = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibratorIds(std::vector<int32_t>* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager getVibratorIds";
    *_aidl_return = {0};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibrator(int32_t vibratorId,
                                                std::shared_ptr<IVibrator>* _aidl_return) {
    LOG(DEBUG) << "VibratorManager getVibrator id: " << vibratorId;
    if (vibratorId == 0) {
        *_aidl_return = mDefaultVibrator;
        return ndk::ScopedAStatus::ok();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus VibratorManager::prepareSynced(const std::vector<int32_t>& /*vibratorIds*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VibratorManager::triggerSynced(
        const std::shared_ptr<IVibratorCallback>& /*callback*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VibratorManager::cancelSynced() {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
