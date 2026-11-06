#include "Application.h"
#include "ThreadSafeQueue.h"

#include <Antilatency.InterfaceContract.LibraryLoader.h>
#include <Antilatency.InterfaceContract.PathResolver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <thread>

#include <csv2/writer.hpp>

namespace {
    struct DurationStats {
        std::size_t count{0};
        std::int64_t min{0};
        std::int64_t max{0};
        std::int64_t median{0};
        std::int64_t p95{0};
        std::int64_t p99{0};
        std::int64_t sum{0};
        double mean{0.0};
        double stddev{0.0};
    };

    // Nearest-rank percentile, expects sorted non-empty input
    std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
        auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
        return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
    }

    DurationStats computeStats(std::vector<std::int64_t> values) {
        DurationStats stats{};
        if (values.empty()) {
            return stats;
        }
        std::sort(values.begin(), values.end());

        stats.count = values.size();
        stats.min = values.front();
        stats.max = values.back();
        stats.median = percentile(values, 0.50);
        stats.p95 = percentile(values, 0.95);
        stats.p99 = percentile(values, 0.99);
        stats.sum = std::accumulate(values.begin(), values.end(), std::int64_t{0});
        stats.mean = static_cast<double>(stats.sum) / static_cast<double>(stats.count);

        double sqSum{0.0};
        for (auto v : values) {
            const double d = static_cast<double>(v) - stats.mean;
            sqSum += d * d;
        }
        stats.stddev = std::sqrt(sqSum / static_cast<double>(stats.count));
        return stats;
    }

    // durations[readAllIndex] is the sum of all property reads per iteration; Share_% is
    // computed against it for rows up to and including it. Rows after it (trigger timing,
    // poll counts) are not parts of that sum and get no share.
    void reportTiming(const std::string& fileName, const std::vector<std::string>& names, const std::vector<std::vector<std::int64_t>>& durations, std::size_t readAllIndex, std::chrono::steady_clock::duration totalRunTime) {
        std::vector<DurationStats> stats{};
        for (const auto& d : durations) {
            stats.push_back(computeStats(d));
        }
        const auto totalReadUs = stats[readAllIndex].sum;

        auto share = [&](std::size_t i) -> std::string {
            if (i > readAllIndex) {
                return "";
            }
            const double value = totalReadUs > 0 ? 100.0 * static_cast<double>(stats[i].sum) / static_cast<double>(totalReadUs) : 0.0;
            return std::format("{:.1f}", value);
        };

        std::cout << "\nTiming summary (us), samples: " << stats[readAllIndex].count << std::endl;
        std::cout << std::format("{:<24}{:>10}{:>10}{:>10}{:>10}{:>10}{:>10}{:>10}{:>14}{:>9}", "Property", "Min", "Mean", "Median", "P95", "P99", "Max", "StdDev", "Total", "Share%") << std::endl;
        for (std::size_t i = 0; i < stats.size(); ++i) {
            const auto& s = stats[i];
            std::cout << std::format("{:<24}{:>10}{:>10.1f}{:>10}{:>10}{:>10}{:>10}{:>10.1f}{:>14}{:>9}", names[i], s.min, s.mean, s.median, s.p95, s.p99, s.max, s.stddev, s.sum, share(i)) << std::endl;
        }
        std::cout << "Total read time: " << std::format("{:.3f}", static_cast<double>(totalReadUs) / 1000.0) << " ms, total run time: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(totalRunTime).count() << " ms" << std::endl;

        std::ofstream stream{fileName};
        if (!stream.is_open()) {
            std::cout << "Failed to create timing file " << fileName << std::endl;
            return;
        }
        csv2::Writer<csv2::delimiter<';'>> writer{stream};
        writer.write_row(std::vector<std::string>{"Property", "Count", "Min_us", "Mean_us", "Median_us", "P95_us", "P99_us", "Max_us", "StdDev_us", "Total_us", "Share_%"});
        for (std::size_t i = 0; i < stats.size(); ++i) {
            const auto& s = stats[i];
            writer.write_row(std::vector<std::string>{
                names[i],
                std::to_string(s.count),
                std::to_string(s.min),
                std::format("{:.1f}", s.mean),
                std::to_string(s.median),
                std::to_string(s.p95),
                std::to_string(s.p99),
                std::to_string(s.max),
                std::format("{:.1f}", s.stddev),
                std::to_string(s.sum),
                share(i),
            });
        }
        std::cout << "Timing summary saved to " << fileName << std::endl;
    }
}

Application::Application(const ConfigSettings& configSettings, const ApplicationSettings& settings) : settings(std::move(settings)), configSettings(std::move(configSettings)) {
    using namespace Antilatency;
    auto deviceNetworkLibrary = InterfaceContract::getLibraryInterface<DeviceNetwork::ILibrary>(InterfaceContract::PathResolver::siblingLibraryPath("AntilatencyDeviceNetwork").c_str());

    if (!deviceNetworkLibrary) {
        throw std::runtime_error("Failed to load AntilatencyDeviceNetwork library");
    }

    std::cout << "Load AntilatencyDeviceNetwork library " << deviceNetworkLibrary.getVersion() << std::endl;

    auto filter = deviceNetworkLibrary.createFilter();
    filter.addUsbDevice(DeviceNetwork::Constants::AllUsbDevices);
    if (settings.enableIpDevices) {
        std::cout << "Add All IP devices to device network filter" << std::endl;
        filter.addIpDevice(DeviceNetwork::Constants::AllIpDevicesIp, DeviceNetwork::Constants::AllIpDevicesMask);
    }

    _deviceNetwork = deviceNetworkLibrary.createNetwork(filter);

    if (!_deviceNetwork) {
        throw std::runtime_error("Failed to create device network");
    }
}

std::string getCurrentDateAndTime(const char* timeFormat = "%d_%m_%Y_%H_%M_%S") {
    auto n = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(n);
    std::tm buf;
    localtime_s(&buf, &in_time_t);
    std::stringstream oss{};
    oss << std::put_time(&buf, timeFormat);
    return oss.str();
}

int Application::run() {
    auto targetNode = waitTargetDevice();

    if (targetNode == Antilatency::DeviceNetwork::NodeHandle::Null) {
        std::cout << "Failed to find target device" << std::endl;
        return -1;
    }

    if (!targetNodeContainRequiredProperties(targetNode)) {
        return -1;
    }

    using Clock = std::chrono::steady_clock;

    // One property task for the whole capture: we measure property reads, not task open/close
    Antilatency::DeviceNetwork::IPropertyCotask propertyManager{};
    try {
        propertyManager = _deviceNetwork.nodeStartPropertyTask(targetNode);
    } catch (const std::exception& exp) {
        std::cout << "Failed to start property task with error: " << exp.what() << std::endl;
    }
    if (!propertyManager) {
        std::cout << "Failed to create property manager" << std::endl;
        return -1;
    }

    const auto baseFileName = getCurrentDateAndTime();
    const std::string fileName{baseFileName + ".csv"};
    std::ofstream csvFileStream{fileName};

    if (!csvFileStream.is_open()) {
        std::cout << "Failed to create file " << fileName << std::endl;
        return -1;
    }

    csv2::Writer<csv2::delimiter<';'>> csvWriter{csvFileStream};

    const bool measureTiming = settings.measureTiming;
    const auto propertiesCount = configSettings.dumpProperties.size();
    const auto& trigger = configSettings.trigger;
    // Extra timing columns of a triggered run, after ReadAll_us. Cycle = Trigger + Wait + ReadAll,
    // i.e. the time to obtain one fresh set of values.
    const std::vector<std::string> triggerTimingNames{"Trigger", "Wait", "WaitPolls", "Cycle"};
    const std::size_t extraTimingCount = trigger ? triggerTimingNames.size() : 0;

    std::vector<std::string> columnNames{};
    for (const auto& property : configSettings.dumpProperties) {
        columnNames.emplace_back(property.alias.empty() ? property.propertyName : property.alias);
    }

    {
        std::vector<std::string> csvHead{"Timestamp", "ElapsedTime_ms"};
        csvHead.insert(csvHead.end(), columnNames.begin(), columnNames.end());
        if (measureTiming) {
            for (const auto& name : columnNames) {
                csvHead.emplace_back(name + "_us");
            }
            csvHead.emplace_back("ReadAll_us");
            if (trigger) {
                csvHead.insert(csvHead.end(), {"Trigger_us", "Wait_us", "WaitPolls", "Cycle_us"});
            }
        }
        csvWriter.write_row(csvHead);
    }

    // std::nullopt marks the end of data, so the writer never waits for rows that will not come
    TSQueue<std::optional<std::vector<std::string>>> queue{};
    std::thread writeToFile{[&] {
        try {
            std::size_t i = 0;
            while (auto row = queue.pop()) {
                csvWriter.write_row(*row);
                ++i;

                if (settings.printProgress) {
                    std::cout << "Iteration " << i << "/" << settings.samples_count << " completed" << std::endl;
                }
            }
        } catch (const std::exception& exp) {
            std::cout << "Failed to write file with error: " << exp.what() << std::endl;
        }
    }};

    // Per property read durations, then the sum of all reads in an iteration (ReadAll),
    // then for a triggered run Trigger, Wait, WaitPolls and Cycle.
    const std::size_t readAllIndex = propertiesCount;
    std::vector<std::vector<std::int64_t>> readDurations(measureTiming ? propertiesCount + 1 + extraTimingCount : 0);
    for (auto& d : readDurations) {
        d.reserve(settings.samples_count);
    }
    std::vector<std::int64_t> iterationDurations(propertiesCount);

    auto toUs = [](Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    };

    const auto startDumpTime = Clock::now();
    try {
        for (std::size_t i = 0; i < settings.samples_count; ++i) {
            const auto beforeDump = Clock::now();

            std::vector<std::string> row{};
            row.reserve(2 + propertiesCount * 2 + 1 + extraTimingCount);
            row.emplace_back(getCurrentDateAndTime("%d/%m/%Y %H:%M:%S"));
            row.emplace_back(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(beforeDump - startDumpTime).count()));

            // Trigger: start a new measurement and wait for it before reading values.
            // A rejected write throws and ends the run like a failed read.
            std::int64_t triggerUs{0};
            std::int64_t waitUs{0};
            std::int64_t waitPolls{0};
            if (trigger) {
                const auto triggerStart = Clock::now();
                for (const auto& write : trigger->write) {
                    propertyManager.setStringProperty(write.propertyName, write.value);
                }
                const auto waitStart = Clock::now();
                const auto deadline = waitStart + std::chrono::milliseconds(trigger->timeout_ms);
                bool reached{false};
                while (true) {
                    ++waitPolls;
                    if (propertyManager.getStringProperty(trigger->waitUntil.propertyName) == trigger->waitUntil.value) {
                        reached = true;
                        break;
                    }
                    if (Clock::now() >= deadline) {
                        break;
                    }
                    if (trigger->pollPeriod_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(trigger->pollPeriod_ms));
                    }
                }
                const auto waitEnd = Clock::now();
                triggerUs = toUs(waitStart - triggerStart);
                waitUs = toUs(waitEnd - waitStart);
                if (!reached) {
                    std::cout << "Iteration " << i + 1 << ": " << trigger->waitUntil.propertyName << " did not become \""
                              << trigger->waitUntil.value << "\" within " << trigger->timeout_ms << " ms after "
                              << waitPolls << " polls, stopping" << std::endl;
                    break;
                }
            }

            std::int64_t readAllUs{0};
            for (std::size_t p = 0; p < propertiesCount; ++p) {
                const auto readStart = Clock::now();
                auto value = propertyManager.getStringProperty(configSettings.dumpProperties[p].propertyName);
                const auto readEnd = Clock::now();

                row.push_back(std::move(value));
                iterationDurations[p] = std::chrono::duration_cast<std::chrono::microseconds>(readEnd - readStart).count();
                readAllUs += iterationDurations[p];
            }

            // Commit durations only for fully read rows, so all properties have equal sample counts
            if (measureTiming) {
                for (std::size_t p = 0; p < propertiesCount; ++p) {
                    readDurations[p].push_back(iterationDurations[p]);
                    row.push_back(std::to_string(iterationDurations[p]));
                }
                readDurations[readAllIndex].push_back(readAllUs);
                row.push_back(std::to_string(readAllUs));
                if (trigger) {
                    const std::int64_t cycleUs = triggerUs + waitUs + readAllUs;
                    const std::int64_t extra[] = {triggerUs, waitUs, waitPolls, cycleUs};
                    for (std::size_t e = 0; e < extraTimingCount; ++e) {
                        readDurations[readAllIndex + 1 + e].push_back(extra[e]);
                        row.push_back(std::to_string(extra[e]));
                    }
                }
            }

            queue.push(std::move(row));

            std::this_thread::sleep_until(beforeDump + std::chrono::milliseconds(settings.period_ms));
        }
    } catch (const std::exception& exp) {
        std::cout << "Failed to dump with error: " << exp.what() << std::endl;
    }
    const auto totalRunTime = Clock::now() - startDumpTime;

    queue.push(std::nullopt);
    if (writeToFile.joinable()) {
        writeToFile.join();
    }

    csvFileStream.close();

    if (measureTiming) {
        auto names = columnNames;
        names.emplace_back("ReadAll");
        if (trigger) {
            names.insert(names.end(), triggerTimingNames.begin(), triggerTimingNames.end());
        }
        if (readDurations[readAllIndex].empty()) {
            std::cout << "No complete iterations, no timing summary" << std::endl;
        } else {
            reportTiming(baseFileName + "_timing.csv", names, readDurations, readAllIndex, totalRunTime);
        }
    }

    return 0;
}

Antilatency::DeviceNetwork::NodeHandle Application::waitTargetDevice() {
    using namespace Antilatency::DeviceNetwork;
    NodeHandle result{NodeHandle::Null};
    auto start = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    while (static_cast<std::size_t>(duration_ms) < settings.waitDeviceTimeout_ms) {
        now = std::chrono::steady_clock::now();
        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        const auto nodes = _deviceNetwork.getNodes();
        for (const auto& node : nodes) {
            if (_deviceNetwork.nodeGetStatus(node) != NodeStatus::Idle) {
                continue;
            }
            if (isTargetNode(node)) {
                result = node;
                break;
            }
        }
        if (result != NodeHandle::Null) {
            break;
        }
    }

    return result;
}

bool Application::isTargetNode(const Antilatency::DeviceNetwork::NodeHandle& node) {
    using namespace Antilatency::DeviceNetwork;

    bool result{node != NodeHandle::Null};

    try {
        auto propertyTask = _deviceNetwork.nodeStartPropertyTask(node);
        result &= propertyTask;
        for (const auto& [key, value] : configSettings.targetDeviceProperties) {
            if (!result) {
                break;
            }
            result &= _deviceNetwork.nodeGetStringProperty(node, key) == value;
        }
    } catch (const std::exception& exp) {
        std::cout << "Failed to get property with error " << exp.what() << std::endl;
        result &= false;
    }

    return result;
}

bool Application::targetNodeContainRequiredProperties(const Antilatency::DeviceNetwork::NodeHandle& node) {
    using namespace Antilatency::DeviceNetwork;
    if (node == NodeHandle::Null) {
        return false;
    }

    try {
        auto propertyTask = _deviceNetwork.nodeStartPropertyTask(node);
        if (!propertyTask) {
            return false;
        }
        for (const auto& property : configSettings.dumpProperties) {
            propertyTask.getStringProperty(property.propertyName);
        }

        // Trigger properties must already exist. On Antilatency firmwares a write to a key
        // nobody registered is not rejected: it creates a file with that name on the
        // device's flash. An absent key reads back empty, so refuse before writing anything.
        if (configSettings.trigger) {
            std::vector<std::string> keys{};
            for (const auto& write : configSettings.trigger->write) {
                keys.push_back(write.propertyName);
            }
            keys.push_back(configSettings.trigger->waitUntil.propertyName);
            for (const auto& key : keys) {
                if (propertyTask.getStringProperty(key).empty()) {
                    std::cout << "Trigger property " << key << " is absent on the device (reads empty), nothing written" << std::endl;
                    return false;
                }
            }
        }
    } catch (const std::exception& exp) {
        std::cout << "Failed to get property with error " << exp.what() << std::endl;
        return false;
    }

    return true;
}