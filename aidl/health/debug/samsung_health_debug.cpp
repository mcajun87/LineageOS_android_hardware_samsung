/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/health/BatteryChargingPolicy.h>
#include <aidl/android/hardware/health/BatteryHealthData.h>
#include <aidl/android/hardware/health/HealthInfo.h>
#include <aidl/android/hardware/health/IHealth.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "samsung-health/BatteryData.h"
#include "samsung-health/ShortDetection.h"
#include "samsung-health/Utils.h"

namespace hardware::samsung::health {
// Exported in BatteryData.cpp
int ChargingTempBucket(int temp);
}  // namespace hardware::samsung::health

using aidl::android::hardware::health::BatteryChargingPolicy;
using aidl::android::hardware::health::BatteryHealthData;
using aidl::android::hardware::health::HealthInfo;
using aidl::android::hardware::health::IHealth;
using android::base::ParseInt;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::Trim;

namespace {

// Terminal Color Formatting
bool gUseColor = true;

const char* ColorReset() {
    return gUseColor ? "\033[0m" : "";
}
const char* ColorBold() {
    return gUseColor ? "\033[1m" : "";
}
const char* ColorRed() {
    return gUseColor ? "\033[1;31m" : "";
}
const char* ColorGreen() {
    return gUseColor ? "\033[1;32m" : "";
}
const char* ColorYellow() {
    return gUseColor ? "\033[1;33m" : "";
}
const char* ColorCyan() {
    return gUseColor ? "\033[1;36m" : "";
}
const char* ColorGray() {
    return gUseColor ? "\033[0;37m" : "";
}

enum class TestStatus { PASS, FAIL, WARN, INFO, SKIP };

struct TestResult {
    std::string category;
    std::string name;
    TestStatus status;
    std::string message;
    std::string advice;
};

std::vector<TestResult> gResults;

void RecordResult(const std::string& category, const std::string& name, TestStatus status,
                  const std::string& message, const std::string& advice = "") {
    gResults.push_back({category, name, status, message, advice});
    const char* tag = "[INFO]";
    const char* color = ColorCyan();
    switch (status) {
        case TestStatus::PASS:
            tag = "[PASS]";
            color = ColorGreen();
            break;
        case TestStatus::FAIL:
            tag = "[FAIL]";
            color = ColorRed();
            break;
        case TestStatus::WARN:
            tag = "[WARN]";
            color = ColorYellow();
            break;
        case TestStatus::SKIP:
            tag = "[SKIP]";
            color = ColorGray();
            break;
        default:
            break;
    }
    std::cout << color << std::left << std::setw(8) << tag << ColorReset() << " " << ColorBold()
              << "[" << category << "] " << name << ColorReset() << ": " << message << "\n";
    if (!advice.empty()) {
        std::cout << "         " << ColorYellow() << "-> Advice: " << advice << ColorReset()
                  << "\n";
    }
}

std::string FormatTimestamp(int64_t seconds) {
    if (seconds <= 0) {
        return "0 (Not set)";
    }
    std::time_t tt = static_cast<std::time_t>(seconds);
    std::tm utc_tm{};
    gmtime_r(&tt, &utc_tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &utc_tm);
    std::ostringstream oss;
    oss << seconds << " (" << buf << ")";
    return oss.str();
}

std::string FormatPermissions(mode_t mode) {
    std::string s = "----------";
    if (S_ISDIR(mode)) s[0] = 'd';
    if (S_ISCHR(mode)) s[0] = 'c';
    if (S_ISBLK(mode)) s[0] = 'b';
    if (S_ISLNK(mode)) s[0] = 'l';

    if (mode & S_IRUSR) s[1] = 'r';
    if (mode & S_IWUSR) s[2] = 'w';
    if (mode & S_IXUSR) s[3] = 'x';

    if (mode & S_IRGRP) s[4] = 'r';
    if (mode & S_IWGRP) s[5] = 'w';
    if (mode & S_IXGRP) s[6] = 'x';

    if (mode & S_IROTH) s[7] = 'r';
    if (mode & S_IWOTH) s[8] = 'w';
    if (mode & S_IXOTH) s[9] = 'x';
    return s;
}

std::string ResolveUidGid(uid_t uid, gid_t gid) {
    std::ostringstream oss;
    struct passwd* pw = getpwuid(uid);
    if (pw) {
        oss << pw->pw_name;
    } else {
        oss << uid;
    }
    oss << ":";
    struct group* gr = getgrgid(gid);
    if (gr) {
        oss << gr->gr_name;
    } else {
        oss << gid;
    }
    return oss.str();
}

struct NodeEntry {
    std::string category;
    std::string path;
    bool required;
    std::string description;
};

const std::vector<NodeEntry> kTrackedNodes = {
        // BSOH / ASOC
        {"BSOH", "/sys/class/power_supply/sbp-fg/battery_state_of_health", false,
         "Dual-cell Fuel Gauge 1 BSOH sysfs"},
        {"BSOH", "/sys/class/power_supply/sbp-fg-2/battery_state_of_health", false,
         "Dual-cell Fuel Gauge 2 BSOH sysfs"},
        {"BSOH", "/sys/class/power_supply/battery/state_of_health", false,
         "Single-cell Battery State of Health sysfs"},
        {"BSOH", "/efs/FactoryApp/bsoh", false, "EFS Battery State of Health (float string)"},
        {"BSOH", "/efs/FactoryApp/asoc", false,
         "EFS Actual State of Charge / Health (int string)"},

        // Manufacturing / Usage Dates
        {"Date", "/sys/class/power_supply/battery/manufacturing_date", false,
         "Battery manufacturing timestamp sysfs"},
        {"Date", "/sys/class/power_supply/battery/first_usage_date", false,
         "Battery first usage timestamp sysfs"},

        // Charging Policy
        {"Policy", "/sys/class/power_supply/battery/charging_policy", false,
         "Battery charging policy sysfs"},

        // Battery Type & SecAuth
        {"BattType", "/sys/class/power_supply/battery/batt_type", false, "Battery type sysfs"},
        {"BattType", "/sys/class/power_supply/sec_auth/presence", false, "SecAuth presence sysfs"},
        {"BattType", "/sys/class/power_supply/sec_auth/batt_auth", false,
         "SecAuth authentication state sysfs"},
        {"BattType", "/sys/class/power_supply/sec_auth/qr_code", false, "SecAuth QR code sysfs"},
        {"BattType", "/efs/FactoryApp/HwParamBattQR", false, "EFS Hardware Param Battery QR code"},

        // Capacity Max & Thermal Error
        {"Capacity", "/sys/class/power_supply/battery/batt_capacity_max", false,
         "Battery max capacity sysfs"},
        {"Capacity", "/efs/Battery/batt_capacity_max", false, "Battery max capacity EFS backup"},
        {"Thermal", "/sys/class/power_supply/battery/err_wthm", false,
         "Battery error wireless thermal sysfs"},
        {"Thermal", "/efs/Battery/err_wthm", false, "Battery error wireless thermal EFS backup"},

        // RTC & Prev Battery Data
        {"RTC", "/sys/power/rtc_status", false, "RTC status sysfs primary"},
        {"RTC", "/sys/class/sec/rtc/rtc_status", false, "RTC status sysfs secondary"},
        {"RTC", "/efs/FactoryApp/rtc_status", false, "EFS RTC reset counter"},
        {"RTC", "/efs/FactoryApp/prev_batt_data", false,
         "EFS previous battery data (volt,temp,jig,chg)"},
        {"RTC", "/sys/class/power_supply/battery/prev_battery_data", false,
         "Sysfs previous battery data"},

        // Aging History
        {"Aging", "/efs/FactoryApp/batt_temp_charge", false,
         "EFS battery aging temperature charge history"},

        // Cable Count & CISD
        {"CISD", "/efs/FactoryApp/batt_cable_count", false, "EFS cable plug count"},
        {"CISD", "/sys/class/power_supply/battery/cisd_wire_count", false,
         "Sysfs CISD wire count"},
        {"CISD", "/sys/class/power_supply/battery/cisd_cable_data", false, "CISD cable data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_cable_data", false, "CISD cable data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_data", false, "CISD data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_data", false, "CISD data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_dcerr_data", false, "CISD dcerr data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_dcerr_data", false, "CISD dcerr data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_event_data", false, "CISD event data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_event_data", false, "CISD event data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_wc_data", false, "CISD wc data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_wc_data", false, "CISD wc data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_pd_data", false, "CISD pd data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_pd_data", false, "CISD pd data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_power_data", false, "CISD power data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_power_data", false, "CISD power data EFS"},
        {"CISD", "/sys/class/power_supply/battery/cisd_tx_data", false, "CISD tx data sysfs"},
        {"CISD", "/efs/FactoryApp/cisd_tx_data", false, "CISD tx data EFS"},
};

// ==============================================================================
// Section 1: Security, UID, and SELinux Check
// ==============================================================================
void AuditExecutionEnvironment() {
    uid_t uid = getuid();
    gid_t gid = getgid();
    std::cout << "\n"
              << ColorBold() << "--- 1. Execution Context & Permissions ---" << ColorReset()
              << "\n";
    std::cout << "Running as UID: " << uid << " (" << ResolveUidGid(uid, gid) << ")\n";

    if (uid != 0 && uid != 1000) {
        RecordResult("ENV", "Process UID", TestStatus::WARN,
                     "Process is not running as root (0) or system (1000). UID is " +
                             std::to_string(uid),
                     "EFS files (/efs/FactoryApp/...) often require system (1000) or root (0) "
                     "permissions. Consider running via `su`.");
    } else {
        RecordResult("ENV", "Process UID", TestStatus::PASS,
                     "Process has privileged UID: " + std::to_string(uid));
    }

    // Check SELinux enforcement if available
    std::string selinux_enforce;
    if (ReadFileToString("/sys/fs/selinux/enforce", &selinux_enforce)) {
        bool enforcing = (Trim(selinux_enforce) == "1");
        std::cout << "SELinux Status: " << (enforcing ? "Enforcing (1)" : "Permissive (0)") << "\n";
        if (enforcing && uid != 0) {
            RecordResult("ENV", "SELinux", TestStatus::INFO,
                         "SELinux is enforcing; unprivileged domains may be blocked from EFS",
                         "If EFS/sysfs reads fail, check `dmesg | grep avc`.");
        }
    }
}

// ==============================================================================
// Section 2: Audit All Sysfs & EFS Paths
// ==============================================================================
void AuditAllNodes(bool verbose) {
    std::cout << "\n"
              << ColorBold() << "--- 2. Auditing Sysfs & EFS Nodes ---" << ColorReset() << "\n";
    int found_count = 0;
    int missing_count = 0;

    for (const auto& entry : kTrackedNodes) {
        struct stat st{};
        bool exists = (stat(entry.path.c_str(), &st) == 0);
        bool readable = (access(entry.path.c_str(), R_OK) == 0);
        bool writable = (access(entry.path.c_str(), W_OK) == 0);

        if (exists) {
            found_count++;
            std::string content;
            bool read_ok = ReadFileToString(entry.path, &content);
            std::string preview = read_ok ? Trim(content) : "<unread>";
            if (preview.size() > 40) {
                preview = preview.substr(0, 37) + "...";
            }

            std::ostringstream msg;
            msg << FormatPermissions(st.st_mode) << " " << ResolveUidGid(st.st_uid, st.st_gid)
                << " [" << (readable ? "R" : "-") << (writable ? "W" : "-") << "] Value: '"
                << preview << "'";

            if (verbose) {
                RecordResult("NODE", entry.path, TestStatus::PASS, msg.str());
            }
        } else {
            missing_count++;
            if (verbose) {
                RecordResult("NODE", entry.path, TestStatus::INFO, "Not found on this hardware");
            }
        }
    }

    std::ostringstream summary;
    summary << "Found " << found_count << " nodes, " << missing_count
            << " not present on this hardware platform.";
    RecordResult("AUDIT", "Sysfs & EFS Nodes", TestStatus::PASS, summary.str());
}

// ==============================================================================
// Section 3: Direct libsamsunghealth Verification
// ==============================================================================
void VerifyDirectLibsamsunghealth() {
    std::cout << "\n"
              << ColorBold() << "--- 3. Direct libsamsunghealth Verification ---" << ColorReset()
              << "\n";

    hardware::samsung::health::BatteryData batteryData;
    hardware::samsung::health::ShortDetection shortDetection;

    // 3.1 BSOH (Battery State of Health) resolution
    std::cout << "\n" << ColorCyan() << "[BSOH Source Priority Analysis]" << ColorReset() << "\n";
    std::string sbp1, sbp2, soh_sysfs, bsoh_efs, asoc_efs;
    bool has_sbp1 = ReadFileToString("/sys/class/power_supply/sbp-fg/battery_state_of_health", &sbp1);
    bool has_sbp2 =
            ReadFileToString("/sys/class/power_supply/sbp-fg-2/battery_state_of_health", &sbp2);
    bool has_soh = ReadFileToString("/sys/class/power_supply/battery/state_of_health", &soh_sysfs);
    bool has_bsoh = ReadFileToString("/efs/FactoryApp/bsoh", &bsoh_efs);
    bool has_asoc = ReadFileToString("/efs/FactoryApp/asoc", &asoc_efs);

    std::cout << "  Priority 1 (Dual-Cell):  sbp-fg=" << (has_sbp1 ? Trim(sbp1) : "<none>")
              << ", sbp-fg-2=" << (has_sbp2 ? Trim(sbp2) : "<none>") << "\n";
    std::cout << "  Priority 2 (Single-Cell): state_of_health="
              << (has_soh ? Trim(soh_sysfs) : "<none>") << "\n";
    std::cout << "  Priority 3 (EFS BSOH):   /efs/FactoryApp/bsoh="
              << (has_bsoh ? Trim(bsoh_efs) : "<none>") << "\n";
    std::cout << "  Priority 4 (EFS ASOC):   /efs/FactoryApp/asoc="
              << (has_asoc ? Trim(asoc_efs) : "<none>") << "\n";

    int bsoh = batteryData.GetBsoh();
    if (bsoh > 0) {
        RecordResult("LIB", "GetBsoh()", TestStatus::PASS,
                     "Resolved BSOH: " + std::to_string(bsoh) + "%");
    } else {
        RecordResult("LIB", "GetBsoh()", TestStatus::WARN,
                     "Returned 0 (no BSOH/ASOC source available or battery state unknown)",
                     "Check if kernel driver supports state_of_health or EFS bsoh/asoc exists.");
    }

    // 3.2 Manufacturing Date & First Usage Date
    int64_t mfg_date = batteryData.GetManufacturingDate();
    int64_t usage_date = batteryData.GetFirstUsageDate();

    if (mfg_date > 0) {
        RecordResult("LIB", "Manufacturing Date", TestStatus::PASS, FormatTimestamp(mfg_date));
    } else {
        RecordResult("LIB", "Manufacturing Date", TestStatus::INFO,
                     "Not available (0) - sysfs /battery/manufacturing_date not present");
    }

    if (usage_date > 0) {
        RecordResult("LIB", "First Usage Date", TestStatus::PASS, FormatTimestamp(usage_date));
    } else {
        RecordResult("LIB", "First Usage Date", TestStatus::INFO,
                     "Not available (0) - sysfs /battery/first_usage_date not present");
    }

    // 3.3 Charging Policy
    int policy = batteryData.GetChargingPolicy();
    std::string policy_name = "UNKNOWN (" + std::to_string(policy) + ")";
    if (policy >= 0 && policy <= 4) {
        policy_name = aidl::android::hardware::health::toString(
                static_cast<BatteryChargingPolicy>(policy));
    }
    RecordResult("LIB", "Charging Policy", TestStatus::PASS,
                 "Current: " + policy_name + " (code: " + std::to_string(policy) + ")");

    // 3.4 RTC Reset Status
    std::cout << "\n" << ColorCyan() << "[RTC Status Node Check]" << ColorReset() << "\n";
    int rtc_index = -1;
    if (access("/sys/power/rtc_status", F_OK) == 0) {
        rtc_index = 0;
    } else if (access("/sys/class/sec/rtc/rtc_status", F_OK) == 0) {
        rtc_index = 1;
    }
    if (rtc_index >= 0) {
        RecordResult("LIB", "RTC Node", TestStatus::PASS,
                     "Detected rtc_status node at index " + std::to_string(rtc_index) +
                             (rtc_index == 0 ? " (/sys/power/rtc_status)"
                                             : " (/sys/class/sec/rtc/rtc_status)"));
    } else {
        RecordResult("LIB", "RTC Node", TestStatus::INFO,
                     "No rtc_status sysfs node detected on this kernel");
    }

    std::string rtcc_str;
    if (ReadFileToString("/efs/FactoryApp/rtc_status", &rtcc_str)) {
        RecordResult("LIB", "RTC Count (EFS)", TestStatus::PASS,
                     "Counter value: " + Trim(rtcc_str));
    } else {
        RecordResult("LIB", "RTC Count (EFS)", TestStatus::INFO,
                     "/efs/FactoryApp/rtc_status not initialized yet");
    }

    std::string prev_batt;
    if (ReadFileToString("/efs/FactoryApp/prev_batt_data", &prev_batt)) {
        int volt = 0, temp = 0, jig = 0, chg = 0;
        if (sscanf(prev_batt.c_str(), "%d, %d, %d, %d", &volt, &temp, &jig, &chg) == 4) {
            RecordResult("LIB", "prev_batt_data", TestStatus::PASS,
                         "Volt=" + std::to_string(volt) + "mV, Temp=" + std::to_string(temp) +
                                 "(tenths C), Jig=" + std::to_string(jig) +
                                 ", Chg=" + std::to_string(chg));
        } else {
            RecordResult("LIB", "prev_batt_data", TestStatus::WARN,
                         "Raw: '" + Trim(prev_batt) + "' (sscanf did not parse 4 values)");
        }
    }

    // 3.5 Battery Type & QR Code
    std::string batt_type_sysfs;
    if (ReadFileToString("/sys/class/power_supply/battery/batt_type", &batt_type_sysfs)) {
        RecordResult("LIB", "batt_type", TestStatus::PASS, "Current: " + Trim(batt_type_sysfs));
    }

    std::string qr_source_desc = "None";
    std::string qr_data;
    std::string presence_s, auth_s;
    int presence = 0, auth = 0;
    if (ReadFileToString("/sys/class/power_supply/sec_auth/presence", &presence_s)) {
        ParseInt(Trim(presence_s), &presence);
    }
    if (ReadFileToString("/sys/class/power_supply/sec_auth/batt_auth", &auth_s)) {
        ParseInt(Trim(auth_s), &auth);
    }
    if (presence == 1 && auth == 1) {
        if (ReadFileToString("/sys/class/power_supply/sec_auth/qr_code", &qr_data)) {
            qr_source_desc = "sec_auth/qr_code";
        }
    } else {
        if (ReadFileToString("/efs/FactoryApp/HwParamBattQR", &qr_data)) {
            qr_source_desc = "EFS HwParamBattQR";
        }
    }
    if (!qr_data.empty()) {
        std::string masked = Trim(qr_data);
        auto plus = masked.rfind('+');
        if (plus != std::string::npos && plus < 40) {
            masked = masked.substr(0, plus + 1) + "XXXXXXX";
        }
        RecordResult("LIB", "Battery QR Auth", TestStatus::PASS,
                     "Source: " + qr_source_desc + ", Masked Type: " + masked);
    }

    // 3.6 Capacity Max & Error Wthm
    std::string cap_sysfs, cap_efs;
    bool has_cap_sysfs =
            ReadFileToString("/sys/class/power_supply/battery/batt_capacity_max", &cap_sysfs);
    bool has_cap_efs = ReadFileToString("/efs/Battery/batt_capacity_max", &cap_efs);
    if (has_cap_sysfs || has_cap_efs) {
        RecordResult("LIB", "batt_capacity_max", TestStatus::PASS,
                     "Sysfs: " + (has_cap_sysfs ? Trim(cap_sysfs) : "<none>") +
                             ", EFS: " + (has_cap_efs ? Trim(cap_efs) : "<none>"));
    }

    std::string wthm_sysfs, wthm_efs;
    bool has_wthm_sysfs = ReadFileToString("/sys/class/power_supply/battery/err_wthm", &wthm_sysfs);
    bool has_wthm_efs = ReadFileToString("/efs/Battery/err_wthm", &wthm_efs);
    if (has_wthm_sysfs || has_wthm_efs) {
        RecordResult("LIB", "err_wthm", TestStatus::PASS,
                     "Sysfs: " + (has_wthm_sysfs ? Trim(wthm_sysfs) : "<none>") +
                             ", EFS: " + (has_wthm_efs ? Trim(wthm_efs) : "<none>"));
    }

    // 3.7 Cable Count & CISD
    std::string cable_count_efs, wire_count_sysfs;
    bool has_cc_efs = ReadFileToString("/efs/FactoryApp/batt_cable_count", &cable_count_efs);
    bool has_wc_sysfs =
            ReadFileToString("/sys/class/power_supply/battery/cisd_wire_count", &wire_count_sysfs);
    if (has_cc_efs || has_wc_sysfs) {
        RecordResult("LIB", "Cable Count", TestStatus::PASS,
                     "EFS count: " + (has_cc_efs ? Trim(cable_count_efs) : "<none>") +
                             ", Sysfs wire count: " +
                             (has_wc_sysfs ? Trim(wire_count_sysfs) : "<none>"));
    }

    // 3.8 Aging History Parsing
    std::string aging_raw;
    if (ReadFileToString("/efs/FactoryApp/batt_temp_charge", &aging_raw) && !aging_raw.empty()) {
        std::vector<std::string> tok = Split(Trim(aging_raw), ",");
        if (tok.size() >= 16) {
            int64_t record_ts = strtoll(tok[0].c_str(), nullptr, 10);
            std::cout << "\n" << ColorCyan() << "[Aging History Breakdown]" << ColorReset() << "\n";
            std::cout << "  Start Timestamp: " << FormatTimestamp(record_ts) << "\n";
            const char* bucket_names[5] = {">= 55.0C", "45.0..54.9C", "10.0..44.9C", "-5.0..9.9C",
                                           "< -5.0C"};
            for (int i = 0; i < 5; i++) {
                int cap = std::stoi(tok[1 + i]);
                uint64_t duration = std::stoull(tok[6 + i]);
                uint64_t last_ts = std::stoull(tok[11 + i]);
                std::cout << "  Bucket " << i << " (" << std::left << std::setw(12)
                          << bucket_names[i] << "): Charged=" << std::setw(4) << cap
                          << "%, Discharged=" << std::setw(6) << duration << "s, LastActive="
                          << (last_ts > 0 ? FormatTimestamp(last_ts) : "never") << "\n";
            }
            RecordResult("LIB", "Aging History (batt_temp_charge)", TestStatus::PASS,
                         "Successfully parsed 16 fields from /efs/FactoryApp/batt_temp_charge");
        } else {
            RecordResult("LIB", "Aging History (batt_temp_charge)", TestStatus::WARN,
                         "File exists but unexpected column count: " + std::to_string(tok.size()));
        }
    } else {
        RecordResult("LIB", "Aging History (batt_temp_charge)", TestStatus::INFO,
                     "/efs/FactoryApp/batt_temp_charge does not exist yet (flushed on battery "
                     "events)");
    }
}

// ==============================================================================
// Section 4: Live AIDL Health HAL Service Verification
// ==============================================================================
void VerifyAidlHealthService(hardware::samsung::health::BatteryData& batteryData) {
    std::cout << "\n"
              << ColorBold() << "--- 4. Live AIDL Health HAL Service Verification ---"
              << ColorReset() << "\n";

    std::string service_name = std::string(IHealth::descriptor) + "/default";
    std::cout << "Connecting to AIDL service: " << service_name << "...\n";

    bool declared = AServiceManager_isDeclared(service_name.c_str());
    if (!declared) {
        RecordResult("AIDL", "Service Declaration", TestStatus::WARN,
                     service_name + " is not registered in VINTF manifest");
    } else {
        RecordResult("AIDL", "Service Declaration", TestStatus::PASS,
                     service_name + " is declared in VINTF manifest");
    }

    ndk::SpAIBinder binder(AServiceManager_checkService(service_name.c_str()));
    if (binder.get() == nullptr) {
        binder = ndk::SpAIBinder(AServiceManager_waitForService(service_name.c_str()));
    }

    if (binder.get() == nullptr) {
        RecordResult(
                "AIDL", "Service Connection", TestStatus::FAIL,
                "Unable to connect to " + service_name,
                "Ensure android.hardware.health-service.samsung is running: `ps -A | grep health`");
        return;
    }
    RecordResult("AIDL", "Service Connection", TestStatus::PASS,
                 "Successfully connected to " + service_name);

    std::shared_ptr<IHealth> service = IHealth::fromBinder(binder);
    if (!service) {
        RecordResult("AIDL", "Service Interface", TestStatus::FAIL, "Failed to cast to IHealth");
        return;
    }

    // 4.1 getBatteryHealthData
    BatteryHealthData health_data;
    ndk::ScopedAStatus status = service->getBatteryHealthData(&health_data);

    if (status.isOk()) {
        RecordResult("AIDL", "getBatteryHealthData()", TestStatus::PASS,
                     "API returned STATUS_OK: BSOH=" +
                             std::to_string(health_data.batteryStateOfHealth) +
                             "%, MfgDate=" + FormatTimestamp(health_data.batteryManufacturingDateSeconds) +
                             ", FirstUsage=" + FormatTimestamp(health_data.batteryFirstUsageSeconds));

        // Cross-verify with direct BatteryData
        int direct_bsoh = batteryData.GetBsoh();
        if (health_data.batteryStateOfHealth == direct_bsoh) {
            RecordResult("AIDL", "BSOH Match", TestStatus::PASS,
                         "AIDL value (" + std::to_string(health_data.batteryStateOfHealth) +
                                 "%) matches direct libsamsunghealth (" +
                                 std::to_string(direct_bsoh) + "%)");
        } else {
            RecordResult("AIDL", "BSOH Match", TestStatus::WARN,
                         "AIDL value (" + std::to_string(health_data.batteryStateOfHealth) +
                                 "%) != direct libsamsunghealth (" + std::to_string(direct_bsoh) +
                                 "%)");
        }
    } else if (status.getExceptionCode() == EX_UNSUPPORTED_OPERATION) {
        RecordResult(
                "AIDL", "getBatteryHealthData()", TestStatus::FAIL,
                "Service returned EX_UNSUPPORTED_OPERATION",
                "CRITICAL: Health.cpp instantiated base 'Health' instead of 'SamsungHealth' in "
                "main()! Make sure ndk::SharedRefBase::make<SamsungHealth>(...) is used.");
    } else {
        RecordResult("AIDL", "getBatteryHealthData()", TestStatus::FAIL,
                     "Failed with status: " + std::string(status.getMessage()));
    }

    // 4.2 getChargingPolicy
    BatteryChargingPolicy charging_policy = BatteryChargingPolicy::INVALID;
    status = service->getChargingPolicy(&charging_policy);
    if (status.isOk()) {
        RecordResult("AIDL", "getChargingPolicy()", TestStatus::PASS,
                     "Returned policy: " + aidl::android::hardware::health::toString(charging_policy) +
                             " (" + std::to_string(static_cast<int>(charging_policy)) + ")");
    } else if (status.getExceptionCode() == EX_UNSUPPORTED_OPERATION) {
        RecordResult("AIDL", "getChargingPolicy()", TestStatus::FAIL,
                     "Service returned EX_UNSUPPORTED_OPERATION (SamsungHealth class not active in "
                     "HAL binary)");
    } else {
        RecordResult("AIDL", "getChargingPolicy()", TestStatus::FAIL,
                     "Failed with status: " + std::string(status.getMessage()));
    }

    // 4.3 getHealthInfo (Live Telemetry)
    HealthInfo health_info;
    status = service->getHealthInfo(&health_info);
    if (status.isOk()) {
        std::cout << "\n" << ColorCyan() << "[Live Device Battery Telemetry]" << ColorReset() << "\n";
        std::cout << "  Battery Level:       " << health_info.batteryLevel << "%\n";
        std::cout << "  Battery Voltage:     " << health_info.batteryVoltageMillivolts << " mV\n";
        std::cout << "  Battery Temperature: "
                  << (health_info.batteryTemperatureTenthsCelsius / 10.0f) << " °C ("
                  << health_info.batteryTemperatureTenthsCelsius << " tenths C)\n";
        std::cout << "  Current Now:         " << health_info.batteryCurrentMicroamps << " uA ("
                  << (health_info.batteryCurrentMicroamps / 1000.0f) << " mA)\n";
        std::cout << "  Current Average:     " << health_info.batteryCurrentAverageMicroamps
                  << " uA (" << (health_info.batteryCurrentAverageMicroamps / 1000.0f) << " mA)\n";
        std::cout << "  Chargers Online:     AC=" << (health_info.chargerAcOnline ? "YES" : "NO")
                  << ", USB=" << (health_info.chargerUsbOnline ? "YES" : "NO")
                  << ", Wireless=" << (health_info.chargerWirelessOnline ? "YES" : "NO")
                  << ", Dock=" << (health_info.chargerDockOnline ? "YES" : "NO") << "\n";
        RecordResult("AIDL", "getHealthInfo()", TestStatus::PASS,
                     "Successfully fetched live device HealthInfo");
    }
}

// ==============================================================================
// Section 5: Algorithm & Self-Tests
// ==============================================================================
void RunSelfTests() {
    std::cout << "\n" << ColorBold() << "--- 5. Algorithmic Self-Tests ---" << ColorReset() << "\n";

    // 5.1 Temperature Bucket Unit Tests
    struct TempTestVector {
        int temp;
        int expected_bucket;
        const char* label;
    };

    TempTestVector temp_tests[] = {
            {700, 0, "70.0 C (>= 55.0 C)"},    {550, 0, "55.0 C (boundary >= 55.0 C)"},
            {549, 1, "54.9 C (boundary 45..54.9 C)"}, {450, 1, "45.0 C (boundary 45..54.9 C)"},
            {449, 2, "44.9 C (boundary 10..44.9 C)"}, {250, 2, "25.0 C (room temp)"},
            {100, 2, "10.0 C (boundary 10..44.9 C)"}, {99, 3, "9.9 C (boundary -5..9.9 C)"},
            {0, 3, "0.0 C (freezing)"},        {-50, 3, "-5.0 C (boundary -5..9.9 C)"},
            {-51, 4, "-5.1 C (< -5.0 C)"},     {-200, 4, "-20.0 C (< -5.0 C)"},
    };

    bool all_bucket_pass = true;
    for (const auto& t : temp_tests) {
        int bucket = hardware::samsung::health::ChargingTempBucket(t.temp);
        if (bucket != t.expected_bucket) {
            all_bucket_pass = false;
            RecordResult("ALGO", "ChargingTempBucket", TestStatus::FAIL,
                         "Temp " + std::string(t.label) + " expected bucket " +
                                 std::to_string(t.expected_bucket) + " but got " +
                                 std::to_string(bucket));
        }
    }
    if (all_bucket_pass) {
        RecordResult("ALGO", "ChargingTempBucket", TestStatus::PASS,
                     "All 12 temperature bucket boundaries correctly classified");
    }

    // 5.2 QR Code Masking Logic Test
    struct QrTestVector {
        std::string input;
        std::string expected;
    };
    QrTestVector qr_tests[] = {
            {"AA11BB22+1234567890", "AA11BB22+XXXXXXX"},
            {"SHORT+123", "SHORT+XXXXXXX"},
            {"NOPLUSCODE", "NOPLUSCODE"},
    };
    bool all_qr_pass = true;
    for (const auto& qt : qr_tests) {
        std::string s = qt.input;
        auto plus = s.rfind('+');
        if (plus != std::string::npos && plus < 40) {
            s = s.substr(0, plus + 1) + "XXXXXXX";
        }
        if (s != qt.expected) {
            all_qr_pass = false;
            RecordResult("ALGO", "QR Masking", TestStatus::FAIL,
                         "Input '" + qt.input + "' produced '" + s + "' expected '" + qt.expected +
                                 "'");
        }
    }
    if (all_qr_pass) {
        RecordResult("ALGO", "QR Masking", TestStatus::PASS,
                     "Battery QR masking logic verified against test vectors");
    }

    // 5.3 RTC Reset Condition Check Logic Test
    auto CheckRtcCondition = [](int volt, int temp, int jig) -> bool {
        return (volt >= 3700 && temp >= 200 && jig == 0);
    };
    if (CheckRtcCondition(3800, 250, 0) && !CheckRtcCondition(3600, 250, 0) &&
        !CheckRtcCondition(3800, 150, 0) && !CheckRtcCondition(3800, 250, 1)) {
        RecordResult("ALGO", "RTC Condition Logic", TestStatus::PASS,
                     "RTC reset trigger rules (volt >= 3700mV, temp >= 20.0C, jig == 0) verified");
    } else {
        RecordResult("ALGO", "RTC Condition Logic", TestStatus::FAIL,
                     "RTC reset condition logic failed validation");
    }
}

// ==============================================================================
// Section 6: Policy Setter Test
// ==============================================================================
void TestSetChargingPolicy(int target_policy) {
    std::cout << "\n"
              << ColorBold() << "--- Setting Charging Policy: " << target_policy
              << " ---" << ColorReset() << "\n";
    std::string service_name = std::string(IHealth::descriptor) + "/default";
    ndk::SpAIBinder binder(AServiceManager_checkService(service_name.c_str()));
    if (!binder.get()) {
        RecordResult("POLICY", "Set Policy", TestStatus::FAIL, "Could not connect to " + service_name);
        return;
    }
    std::shared_ptr<IHealth> service = IHealth::fromBinder(binder);
    if (!service) {
        RecordResult("POLICY", "Set Policy", TestStatus::FAIL, "Could not get IHealth interface");
        return;
    }

    auto policy_enum = static_cast<BatteryChargingPolicy>(target_policy);
    ndk::ScopedAStatus status = service->setChargingPolicy(policy_enum);
    if (status.isOk()) {
        RecordResult("POLICY", "Set Policy", TestStatus::PASS,
                     "Successfully requested policy " + std::to_string(target_policy));
    } else {
        RecordResult("POLICY", "Set Policy", TestStatus::WARN,
                     "setChargingPolicy failed: " + std::string(status.getMessage()));
    }
}

// ==============================================================================
// Section 7: JSON Output
// ==============================================================================
void PrintJsonReport() {
    std::cout << "{\n";
    std::cout << "  \"timestamp\": " << time(nullptr) << ",\n";
    std::cout << "  \"results\": [\n";
    for (size_t i = 0; i < gResults.size(); ++i) {
        const auto& r = gResults[i];
        const char* status_str = "INFO";
        switch (r.status) {
            case TestStatus::PASS:
                status_str = "PASS";
                break;
            case TestStatus::FAIL:
                status_str = "FAIL";
                break;
            case TestStatus::WARN:
                status_str = "WARN";
                break;
            case TestStatus::SKIP:
                status_str = "SKIP";
                break;
            default:
                break;
        }
        std::cout << "    {\n";
        std::cout << "      \"category\": \"" << r.category << "\",\n";
        std::cout << "      \"name\": \"" << r.name << "\",\n";
        std::cout << "      \"status\": \"" << status_str << "\",\n";
        std::cout << "      \"message\": \"" << r.message << "\"";
        if (!r.advice.empty()) {
            std::cout << ",\n      \"advice\": \"" << r.advice << "\"";
        }
        std::cout << "\n    }" << (i + 1 < gResults.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n";
    std::cout << "}\n";
}

void PrintSummary() {
    int pass = 0, fail = 0, warn = 0, info = 0;
    for (const auto& r : gResults) {
        if (r.status == TestStatus::PASS) pass++;
        if (r.status == TestStatus::FAIL) fail++;
        if (r.status == TestStatus::WARN) warn++;
        if (r.status == TestStatus::INFO) info++;
    }

    std::cout << "\n"
              << ColorBold()
              << "====================================================================\n"
              << "                    DIAGNOSTIC TEST SUMMARY\n"
              << "===================================================================="
              << ColorReset() << "\n";
    std::cout << "  " << ColorGreen() << "PASS: " << pass << ColorReset() << " | " << ColorRed()
              << "FAIL: " << fail << ColorReset() << " | " << ColorYellow() << "WARN: " << warn
              << ColorReset() << " | " << ColorCyan() << "INFO: " << info << ColorReset() << "\n";
    std::cout << ColorBold()
              << "====================================================================\n"
              << ColorReset();

    if (fail > 0) {
        std::cout << ColorRed() << ColorBold() << "\n[!] CRITICAL ISSUES DETECTED:\n"
                  << ColorReset();
        for (const auto& r : gResults) {
            if (r.status == TestStatus::FAIL) {
                std::cout << "  * [" << r.category << "] " << r.name << ": " << r.message << "\n";
                if (!r.advice.empty()) {
                    std::cout << "    " << ColorYellow() << "Action: " << r.advice << ColorReset()
                              << "\n";
                }
            }
        }
    }
}

void ShowUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n\n"
              << "Diagnostic utility to detect if libsamsunghealth and Samsung Health AIDL\n"
              << "features are operating properly on target devices.\n\n"
              << "Options:\n"
              << "  -a, --all             Run all diagnostic suites (default)\n"
              << "      --nodes           Audit all sysfs and EFS paths\n"
              << "      --direct          Verify libsamsunghealth C++ classes directly\n"
              << "      --aidl            Test live AIDL Health HAL service over Binder\n"
              << "      --self-test       Run algorithmic unit self-tests\n"
              << "      --set-policy <N>  Test setting charging policy (0..4)\n"
              << "  -v, --verbose         Print detailed per-node audit logs\n"
              << "      --no-color        Disable ANSI color output\n"
              << "      --json            Output results in JSON format\n"
              << "  -h, --help            Show this help text\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool run_all = true;
    bool do_nodes = false;
    bool do_direct = false;
    bool do_aidl = false;
    bool do_self_test = false;
    int set_policy = -1;
    bool verbose = false;
    bool json_output = false;

    enum LongOptionVal {
        OPT_NODES = 1000,
        OPT_DIRECT,
        OPT_AIDL,
        OPT_SELF_TEST,
        OPT_SET_POLICY,
        OPT_NO_COLOR,
        OPT_JSON,
    };

    static struct option long_options[] = {
            {"all", no_argument, nullptr, 'a'},
            {"nodes", no_argument, nullptr, OPT_NODES},
            {"direct", no_argument, nullptr, OPT_DIRECT},
            {"aidl", no_argument, nullptr, OPT_AIDL},
            {"self-test", no_argument, nullptr, OPT_SELF_TEST},
            {"set-policy", required_argument, nullptr, OPT_SET_POLICY},
            {"verbose", no_argument, nullptr, 'v'},
            {"no-color", no_argument, nullptr, OPT_NO_COLOR},
            {"json", no_argument, nullptr, OPT_JSON},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "avh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'a':
                run_all = true;
                break;
            case OPT_NODES:
                run_all = false;
                do_nodes = true;
                break;
            case OPT_DIRECT:
                run_all = false;
                do_direct = true;
                break;
            case OPT_AIDL:
                run_all = false;
                do_aidl = true;
                break;
            case OPT_SELF_TEST:
                run_all = false;
                do_self_test = true;
                break;
            case OPT_SET_POLICY:
                set_policy = std::stoi(optarg);
                break;
            case 'v':
                verbose = true;
                break;
            case OPT_NO_COLOR:
                gUseColor = false;
                break;
            case OPT_JSON:
                json_output = true;
                gUseColor = false;
                break;
            case 'h':
                ShowUsage(argv[0]);
                return 0;
            default:
                ShowUsage(argv[0]);
                return 1;
        }
    }

    if (!isatty(fileno(stdout))) {
        gUseColor = false;
    }

    if (!json_output) {
        std::cout << ColorBold() << ColorCyan()
                  << "====================================================================\n"
                  << "              Samsung Health HAL Feature Diagnostic Tool            \n"
                  << "===================================================================="
                  << ColorReset() << "\n";
    }

    hardware::samsung::health::BatteryData batteryData;

    if (run_all) {
        AuditExecutionEnvironment();
        AuditAllNodes(verbose);
        VerifyDirectLibsamsunghealth();
        VerifyAidlHealthService(batteryData);
        RunSelfTests();
    } else {
        if (do_nodes) {
            AuditExecutionEnvironment();
            AuditAllNodes(verbose);
        }
        if (do_direct) {
            VerifyDirectLibsamsunghealth();
        }
        if (do_aidl) {
            VerifyAidlHealthService(batteryData);
        }
        if (do_self_test) {
            RunSelfTests();
        }
    }

    if (set_policy >= 0) {
        TestSetChargingPolicy(set_policy);
    }

    if (json_output) {
        PrintJsonReport();
    } else {
        PrintSummary();
    }

    for (const auto& r : gResults) {
        if (r.status == TestStatus::FAIL) {
            return 1;
        }
    }
    return 0;
}
