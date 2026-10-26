/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibratorManager.h>
#include <aidl/android/hardware/vibrator/IVibrator.h>

#include <memory>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class VibratorManager : public BnVibratorManager {
  public:
    explicit VibratorManager(std::shared_ptr<IVibrator> vibrator);
    virtual ~VibratorManager() = default;

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getVibratorIds(std::vector<int32_t>* _aidl_return) override;
    ndk::ScopedAStatus getVibrator(int32_t vibratorId,
                                   std::shared_ptr<IVibrator>* _aidl_return) override;
    ndk::ScopedAStatus prepareSynced(const std::vector<int32_t>& vibratorIds) override;
    ndk::ScopedAStatus triggerSynced(const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus cancelSynced() override;

  private:
    std::shared_ptr<IVibrator> mDefaultVibrator;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
