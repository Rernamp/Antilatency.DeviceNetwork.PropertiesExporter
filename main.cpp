
#include "CLI/CLI.hpp"

#include "Application.Settings.h"
#include "Application.h"

#include <fstream>


void fillComandLineArguments(CLI::App& app, ApplicationSettings& settings, std::string& configFilePath) {
    app.add_option("--config", configFilePath, "Config json file path")->check(CLI::ExistingFile)->required();
    app.add_option("--period", settings.period_ms, "Period between dump properties, ms")->check(CLI::NonNegativeNumber)->capture_default_str();
    app.add_option("--samplesCount", settings.samples_count, "Samples count")->check(CLI::NonNegativeNumber)->capture_default_str();
    app.add_option("--waitDeviceTimeout", settings.waitDeviceTimeout_ms, "Wait device timeout, ms")->check(CLI::NonNegativeNumber)->capture_default_str();
    app.add_flag("--ipDevices", settings.enableIpDevices, "Enable IP devices in device network")->capture_default_str();
    app.add_flag("--printProgress", settings.printProgress, "Enable printing progress of capture")->capture_default_str();
    app.add_flag("--measureTiming", settings.measureTiming, "Measure duration of each property read and save timing summary")->capture_default_str();
}

int main(int argc, char** argv) {
    CLI::App app{"PropertiesExporter"};

    ApplicationSettings applicationSettings{};
    std::string configFilePath {};
    fillComandLineArguments(app, applicationSettings, configFilePath);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    std::cout << "Config file: " << configFilePath << std::endl;

    std::string configFileValue {};
    try {
        std::ifstream configFile {configFilePath};
        if (!configFile) {
            throw std::runtime_error("Could not open file: " + configFilePath);
        }

        configFileValue = {std::istreambuf_iterator<char>(configFile), std::istreambuf_iterator<char>()};

    } catch (const std::exception& exp) {
        std::cout << "Failed to read json with error: " << exp.what() << std::endl;
        return -1;
    }

    ConfigSettings configSettings {};
    try {
        configSettings = parseFromString(configFileValue);
    } catch (const std::exception& exp) {
        std::cout << "Failed to parse json with error: " << exp.what() << std::endl;
        return -1;
    }

    if (configSettings.dumpProperties.empty() || configSettings.targetDeviceProperties.empty()) {
        std::cout << "json filed dumpProperties or targetDeviceProperties empty" << std::endl;
        return -1;
    }

    if (configSettings.trigger && configSettings.trigger->waitUntil.propertyName.empty()) {
        std::cout << "json field trigger.waitUntil.propertyName empty" << std::endl;
        return -1;
    }

    try {
        Application application {configSettings, applicationSettings};
    
        return application.run();
    } catch (const std::exception& exp) {
        std::cout << "Failed to run application with error: " << exp.what() << std::endl;
    }
}