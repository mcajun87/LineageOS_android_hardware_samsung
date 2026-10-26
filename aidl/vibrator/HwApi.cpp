/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "HwApi.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static const std::vector<std::pair<std::string, sec_features>> kFeatureMap = {
        {"INTENSITY", INTENSITY},
        {"FORCE_TOUCH_INTENSITY", FORCE_TOUCH_INTENSITY},
        {"MULTI_FREQUENCY", MULTI_FREQUENCY},
        {"HAPTIC_ENGINE", HAPTIC_ENGINE},
        {"INDEX_HAPTIC_PATTERN", INDEX_HAPTIC_PATTERN},
        {"SEPINDEX_HAPTIC_PATTERN", SEPINDEX_HAPTIC_PATTERN},
        {"ENHANCED_HAPTIC_ENGINE", ENHANCED_HAPTIC_ENGINE},
        {"FIFO_HAPTIC_ENGINE", FIFO_HAPTIC_ENGINE},
        {"HYBRID_HAPTIC_ENGINE", HYBRID_HAPTIC_ENGINE},
        {"ENHANCED_HYBRID_HAPTIC_ENGINE", ENHANCED_HYBRID_HAPTIC_ENGINE},
        {"CAPABILITY_EXTERNAL_CONTROL", CAPABILITY_EXTERNAL_CONTROL},
        {"COMMON_INPUTFF_INTERFACE", COMMON_INPUTFF_INTERFACE},
        {"PRIMITIVE_EFFECT_COMPOSE", PRIMITIVE_EFFECT_COMPOSE},
        {"HYBRID_PATTERN_COMMON_INPUTFF", HYBRID_PATTERN_COMMON_INPUTFF},
};

static const std::vector<std::pair<std::string, int>> kMotorTypeMap = {
        {"NONE", MOTOR_NONE},
        {"LINEAR_INDEX", MOTOR_LINEAR_INDEX},
        {"COINDC_INDEX", MOTOR_COINDC_INDEX},
        {"COINDC_PMIC", MOTOR_COINDC_PMIC},
        {"LINEAR_0832RC", MOTOR_LINEAR_0832RC},
        {"LINEAR_1040", MOTOR_LINEAR_1040},
        {"LINEAR_RCVMOT", MOTOR_LINEAR_RCVMOT},
        {"LINEAR_1030", MOTOR_LINEAR_1030},
        {"LINEAR_INDEX_RC", MOTOR_LINEAR_INDEX_RC},
        {"LINEAR_WATCH", MOTOR_LINEAR_WATCH},
        {"LINEAR_ACTIVE", MOTOR_LINEAR_ACTIVE},
};

HwApi::HwApi()
    : mUseLeds(false),
      mMotorIndex(0),
      mCalDynamicIntensities(false),
      mAspEnable(0),
      mSupportFeatures(0),
      mInputFd(-1),
      mFfEffectId(-1) {
    if (access("/sys/class/timed_output/vibrator/enable", F_OK) == 0) {
        mUseLeds = false;
        mPrefix = "/sys/class/timed_output/vibrator/";
    } else if (access("/sys/class/leds/vibrator/activate", F_OK) == 0) {
        mUseLeds = true;
        mPrefix = "/sys/class/leds/vibrator/";
        LOG(INFO) << "Vibrator using LED trigger";
    } else {
        mUseLeds = false;
        mPrefix = "/sys/class/timed_output/vibrator/";
    }

    mIntensityNode = mPrefix + "intensity";
    mForceTouchIntensityNode = mPrefix + "force_touch_intensity";
    mMultiFreqNode = mPrefix + "multi_freq";
    mHapticEngineNode = mPrefix + "haptic_engine";
    mCpTriggerIndexNode = mPrefix + "cp_trigger_index";
    mCpTriggerQueueNode = mPrefix + "cp_trigger_queue";
    mNumWavesNode = mPrefix + "num_waves";
    mMotorTypeNode = mPrefix + "motor_type";
    mEventCmdNode = mPrefix + "event_cmd";
    mPatternLengthNode = mPrefix + "pattern_length";
    mExtControlNode = mPrefix + "ext_control";
    mIntensitiesNode = mPrefix + "intensities";
    mHapticIntensitiesNode = mPrefix + "haptic_intensities";
    mAspEnableNode = mPrefix + "asp_enable";
    mGpio1FallIndexNode = mPrefix + "gpio1_fall_index";
    mGpio1FallDigScaleNode = mPrefix + "gpio1_fall_dig_scale";
    mGpio1RiseIndexNode = mPrefix + "gpio1_rise_index";
    mGpio1RiseDigScaleNode = mPrefix + "gpio1_rise_dig_scale";
    mVibeStateNode = mPrefix + "vibe_state";
    mFifoNode = mPrefix + "fifo";
    mHybridHapticEngineNode = mPrefix + "hybrid_haptic_engine";
    mSupportFunctionsNode = "/sys/class/vib_info_class/vib_support_info/functions";
    mUseSepIndexNode = "/sys/class/sec_vib_inputff/control/use_sep_index";

    std::string funcs = readNode(mSupportFunctionsNode);
    for (const auto& [name, feat] : kFeatureMap) {
        if (funcs.find(name) != std::string::npos) {
            mSupportFeatures |= (1u << feat);
        }
    }

    mMotorIndex = getMotorIndex();
    LOG(INFO) << "Vibrator THE_DEVICE_MOTOR_TYPE : " << mMotorIndex;

    mCalDynamicIntensities = isCalDynamicIntensitiesNodeExist();
    if (hasAspEnable()) {
        mAspEnable = getAspEnable();
    }

    initForceFeedbackEffect();
}

HwApi::~HwApi() {
    if (mInputFd >= 0) {
        if (mFfEffectId != -1) {
            ioctl(mInputFd, EVIOCRMFF, mFfEffectId);
            mFfEffectId = -1;
        }
        close(mInputFd);
        mInputFd = -1;
    }
}

bool HwApi::checkClientUID(int uid) {
    if (uid == 1000 || uid == 0) {
        return true;
    }
    LOG(ERROR) << "Permission denied (" << uid << ")";
    return false;
}

bool HwApi::isNodeExist(const std::string& node) {
    return access(node.c_str(), F_OK) == 0;
}

bool HwApi::isNodeExistForRead(const std::string& node) {
    return access(node.c_str(), R_OK) == 0;
}

bool HwApi::isNodeExistForWrite(const std::string& node) {
    return access(node.c_str(), W_OK) == 0;
}

bool HwApi::writeNode(const std::string& node, int value) {
    std::lock_guard<std::mutex> lock(mLock);
    return ::android::base::WriteStringToFile(std::to_string(value), node);
}

bool HwApi::writeNodeByStringOnlyForWriting(const std::string& node, const std::string& value) {
    std::lock_guard<std::mutex> lock(mLock);
    return ::android::base::WriteStringToFile(value, node);
}

std::string HwApi::readNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mLock);
    std::string content;
    if (::android::base::ReadFileToString(node, &content)) {
        return ::android::base::Trim(content);
    }
    return "";
}

bool HwApi::do_on(int timeout) {
    if (!mUseLeds) {
        return writeNode(mPrefix + "enable", timeout);
    } else {
        writeNode(mPrefix + "state", 1);
        writeNode(mPrefix + "duration", timeout);
        return writeNode(mPrefix + "activate", 1);
    }
}

bool HwApi::do_off() {
    if (!mUseLeds) {
        return writeNode(mPrefix + "enable", 0);
    } else {
        return writeNode(mPrefix + "activate", 0);
    }
}

int HwApi::getAspEnable() {
    std::string val = readNode(mAspEnableNode);
    if (!val.empty()) {
        try {
            return std::stoi(val);
        } catch (...) {
        }
    }
    return 0;
}

bool HwApi::hasAspEnable() {
    return isNodeExist(mAspEnableNode);
}

bool HwApi::setAspEnable(int enable) {
    return writeNode(mAspEnableNode, enable);
}

int HwApi::getMotorIndex() {
    std::string type = readNode(mMotorTypeNode);
    if (!type.empty()) {
        for (const auto& [name, idx] : kMotorTypeMap) {
            if (type == name) {
                return idx;
            }
        }
    }
    return getMotorIndexbyMethod();
}

int HwApi::getMotorIndexbyMethod() {
    if (isNumWavesNodeExist()) {
        if (isHapticEngineNodeExist()) {
            return MOTOR_LINEAR_1040;
        } else {
            return MOTOR_LINEAR_WATCH;
        }
    } else {
        if (isHapticEngineNodeExist()) {
            if (isCalDynamicIntensitiesNodeExist()) {
                return MOTOR_LINEAR_RCVMOT;
            } else {
                return MOTOR_LINEAR_1030;
            }
        } else {
            if (isAmplitudeControlExist()) {
                return MOTOR_LINEAR_INDEX;
            } else {
                return MOTOR_COINDC_INDEX;
            }
        }
    }
}

int HwApi::getMotorPerformMethod() {
    if (isNodeExist(mCpTriggerIndexNode)) {
        return 1;
    } else if (isNodeExist(mHapticEngineNode)) {
        return 2;
    } else if (isNodeExist(mIntensityNode)) {
        return 3;
    }
    return 0;
}

bool HwApi::hasFeature(sec_features feature) {
    return (mSupportFeatures & (1u << feature)) != 0;
}

bool HwApi::hasFeature(const std::string& feature) {
    if (feature.empty() || !isNodeExist(mSupportFunctionsNode)) {
        return false;
    }
    std::string allFeatures = readNode(mSupportFunctionsNode);
    if (allFeatures.empty()) {
        return false;
    }
    std::istringstream stream(allFeatures);
    std::string token;
    while (std::getline(stream, token, ' ')) {
        if (token == feature) {
            return true;
        }
    }
    return false;
}

bool HwApi::supportsHasFeature() {
    return isNodeExist(mSupportFunctionsNode);
}

bool HwApi::isAmplitudeControlExist() {
    return isNodeExist(mIntensityNode);
}

bool HwApi::isCalDynamicIntensitiesNodeExist() {
    return isNodeExist(mHapticIntensitiesNode) || isNodeExist(mIntensitiesNode);
}

bool HwApi::isHapticEngineNodeExist() {
    return isNodeExist(mHapticEngineNode);
}

bool HwApi::isMotorTypeNodeExist() {
    return isNodeExist(mMotorTypeNode);
}

bool HwApi::isNumWavesNodeExist() {
    return isNodeExist(mNumWavesNode);
}

bool HwApi::isVibrating() {
    if (isNodeExist(mVibeStateNode)) {
        std::string state = readNode(mVibeStateNode);
        return state == "1";
    }
    return false;
}

bool HwApi::isVibratorActiveNodeExist() {
    return isNodeExist(mPrefix + "activate");
}

bool HwApi::isVibratorDurationNodeExist() {
    return isNodeExist(mPrefix + "duration");
}

bool HwApi::isVibratorNodeExist() {
    return isNodeExist(mPrefix + "enable");
}

bool HwApi::isVibratorStateNodeExist() {
    return isNodeExist(mPrefix + "state");
}

bool HwApi::pollVibeState() {
    int count = 0;
    while (isVibrating() && count < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        count++;
    }
    return !isVibrating();
}

bool HwApi::setAmplitude(int amplitude) {
    return writeNode(mIntensityNode, amplitude);
}

bool HwApi::setPerformAmplitude(int amplitude) {
    return writeNode(mIntensityNode, amplitude);
}

bool HwApi::supportsEnhancedSamsungHapticPattern() {
    return mMotorIndex == MOTOR_LINEAR_1040 || isHapticEngineNodeExist();
}

std::string HwApi::get_input_field_value() {
    FILE* fp = fopen("/proc/bus/input/devices", "r");
    if (!fp) {
        LOG(ERROR) << "Failed to open /proc/bus/input/devices: " << strerror(errno);
        return "";
    }

    char line[1024];
    bool foundDevice = false;
    std::string eventNode;

    while (fgets(line, sizeof(line), fp)) {
        if (!foundDevice) {
            if (strstr(line, "N: Name=\"sec_vibrator_inputff\"")) {
                foundDevice = true;
            }
        } else {
            if (line[0] == '\n') {
                foundDevice = false;
                continue;
            }
            if (line[0] == 'H') {
                char* p = strstr(line, "event");
                if (p) {
                    char eventName[64] = {0};
                    if (sscanf(p, "%63s", eventName) == 1) {
                        eventNode = std::string("/dev/input/") + eventName;
                        break;
                    }
                }
            }
        }
    }
    fclose(fp);
    return eventNode;
}

bool HwApi::initForceFeedbackEffect() {
    if (mInputFd >= 0) {
        return true;
    }

    mInputDevicePath = get_input_field_value();
    if (mInputDevicePath.empty()) {
        return false;
    }

    mInputFd = open(mInputDevicePath.c_str(), O_RDWR | O_CLOEXEC);
    if (mInputFd < 0) {
        LOG(ERROR) << "Failed to open " << mInputDevicePath << ": " << strerror(errno);
        return false;
    }

    LOG(INFO) << "Input Node File Open: " << mInputDevicePath;
    writeNodeByStringOnlyForWriting(mUseSepIndexNode, "1");

    return true;
}

bool HwApi::uploadForceFeedbackEffect(const std::vector<int16_t>& effectData, int timeoutMs,
                                      bool isConstant) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mInputFd < 0) {
        return false;
    }

    if (mFfEffectId != -1) {
        ioctl(mInputFd, EVIOCRMFF, mFfEffectId);
        mFfEffectId = -1;
    }

    struct ff_effect effect = {
            .id = -1,
            .replay.length = static_cast<uint16_t>(timeoutMs),
    };

    if (isConstant) {
        effect.type = FF_CONSTANT;
    } else {
        effect.type = FF_PERIODIC;
        effect.u.periodic.waveform = FF_CUSTOM;
        effect.u.periodic.custom_len = static_cast<uint32_t>(effectData.size());
        effect.u.periodic.custom_data = const_cast<int16_t*>(effectData.data());
    }

    int ret = ioctl(mInputFd, EVIOCSFF, &effect);
    if (ret < 0) {
        LOG(ERROR) << "Effect upload failed: " << strerror(errno);
        return false;
    }
    mFfEffectId = effect.id;
    return true;
}

bool HwApi::writeInputEvent(uint16_t type, uint16_t code, int32_t value) {
    if (mInputFd < 0) {
        return false;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, nullptr);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t ret = TEMP_FAILURE_RETRY(write(mInputFd, &ev, sizeof(ev)));
    return ret == sizeof(ev);
}

bool HwApi::triggerForceFeedbackVibration(int value) {
    return writeInputEvent(EV_FF, mFfEffectId >= 0 ? mFfEffectId : 0, value);
}

bool HwApi::setForceFeedbackAmplitude(int amplitude) {
    return writeInputEvent(EV_FF, FF_GAIN, amplitude);
}

bool HwApi::setAndTriggerCommonInputff(const common_inputff_effects* effects) {
    if (!effects) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mLock);
    if (mInputFd < 0) {
        if (!initForceFeedbackEffect()) {
            return false;
        }
    }

    if (mFfEffectId != -1) {
        ioctl(mInputFd, EVIOCRMFF, mFfEffectId);
        mFfEffectId = -1;
    }

    struct ff_effect effect = {
            .type = FF_PERIODIC,
            .id = -1,
            .replay.length = 0,
    };
    effect.u.periodic.waveform = FF_CUSTOM;
    effect.u.periodic.custom_len = sizeof(common_inputff_effects) / sizeof(int16_t);
    effect.u.periodic.custom_data =
            reinterpret_cast<__s16*>(const_cast<common_inputff_effects*>(effects));

    int ret = ioctl(mInputFd, EVIOCSFF, &effect);
    if (ret < 0) {
        LOG(ERROR) << "CommonInputff ioctl failed: " << strerror(errno);
        return false;
    }
    mFfEffectId = effect.id;

    return triggerForceFeedbackVibration(1);
}

void HwApi::debug(int fd) {
    dprintf(fd, "HwApi Debug Info:\n");
    dprintf(fd, "  Prefix: %s\n", mPrefix.c_str());
    dprintf(fd, "  Use LEDs: %s\n", mUseLeds ? "true" : "false");
    dprintf(fd, "  Motor Index: %d\n", mMotorIndex);
    dprintf(fd, "  Dynamic Intensities: %s\n", mCalDynamicIntensities ? "true" : "false");
    dprintf(fd, "  ASP Enable: %d\n", mAspEnable);
    dprintf(fd, "  Features bitmask: 0x%08x\n", mSupportFeatures);
    dprintf(fd, "  Input FD: %d\n", mInputFd);
    dprintf(fd, "  Input Device Path: %s\n", mInputDevicePath.c_str());
    dprintf(fd, "  FF Effect ID: %d\n", mFfEffectId);
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
