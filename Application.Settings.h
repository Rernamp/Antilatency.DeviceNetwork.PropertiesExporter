#pragma once

#include <string>
#include <map>
#include <optional>
#include <vector>

struct Property {
    std::string propertyName {};
    std::string alias {};
};

struct PropertyValue {
    std::string propertyName {};
    std::string value {};
};

// Optional per-iteration trigger: write `write` properties, then poll `waitUntil` until it
// reads the expected value, then read dumpProperties. For firmwares where a read returns
// the last measurement and a new one has to be started explicitly.
struct TriggerSettings {
    std::vector<PropertyValue> write {};
    PropertyValue waitUntil {};
    std::size_t pollPeriod_ms {0};
    std::size_t timeout_ms {1000};
};

struct ConfigSettings {
    std::map<std::string, std::string> targetDeviceProperties {};
    std::vector<Property> dumpProperties {};
    std::optional<TriggerSettings> trigger {};
};

struct ApplicationSettings {
    std::size_t period_ms {1};
    std::size_t samples_count {10};
    std::size_t waitDeviceTimeout_ms {2000};
    bool printProgress {false};
    bool enableIpDevices {false};
    bool measureTiming {false};
};

ConfigSettings parseFromString(const std::string& str);

