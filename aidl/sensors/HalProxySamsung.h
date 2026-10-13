/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <HalProxyAidl.h>

#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace sensors {
namespace implementation {

class HalProxySamsung : public HalProxyAidl {
  public:
    HalProxySamsung();

  private:
    ndk::ScopedAStatus getSensorsList(
            std::vector<::aidl::android::hardware::sensors::SensorInfo>* _aidl_return) override;

    bool mHideDeviceOrientation = false;
};

}  // namespace implementation
}  // namespace sensors
}  // namespace hardware
}  // namespace android
}  // namespace aidl
