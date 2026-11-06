#include "Application.Settings.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void from_json(const json& j, Property& property) {
    j.at("alias").get_to(property.alias);
    j.at("propertyName").get_to(property.propertyName);
}

void from_json(const json& j, PropertyValue& property) {
    j.at("propertyName").get_to(property.propertyName);
    j.at("value").get_to(property.value);
}

void from_json(const json& j, TriggerSettings& trigger) {
    j.at("write").get_to(trigger.write);
    j.at("waitUntil").get_to(trigger.waitUntil);
    if (j.contains("pollPeriod_ms")) {
        j.at("pollPeriod_ms").get_to(trigger.pollPeriod_ms);
    }
    if (j.contains("timeout_ms")) {
        j.at("timeout_ms").get_to(trigger.timeout_ms);
    }
}

void from_json(const json& j, ConfigSettings& settings) {
    j.at("targetDeviceProperties").get_to(settings.targetDeviceProperties);
    j.at("dumpProperties").get_to(settings.dumpProperties);
    if (j.contains("trigger")) {
        settings.trigger = j.at("trigger").get<TriggerSettings>();
    }
}

ConfigSettings parseFromString(const std::string& str) {
    auto json = json::parse(str);
    return json.get<ConfigSettings>();
}