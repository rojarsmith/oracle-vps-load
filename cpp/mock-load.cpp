#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr uint64_t kBytesPerMebibyte = 1024ull * 1024ull;
constexpr uint64_t kDefaultNetworkCapacityBps = 100ull * 1000ull * 1000ull;
constexpr uint16_t kDefaultNetworkPort = 49010;
constexpr int kDefaultDurationSeconds = 300;
constexpr int kDefaultSampleIntervalSeconds = 1;
constexpr int kCpuPeriodMilliseconds = 100;
constexpr uint64_t kMemoryBlockBytes = 16ull * kBytesPerMebibyte;

std::atomic<bool> g_shouldRun{true};

void handleSignal(int) {
    g_shouldRun.store(false);
}

double clampDouble(double value, double low, double high) {
    return std::max(low, std::min(value, high));
}

[[maybe_unused]] std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string formatBytes(uint64_t bytes) {
    std::ostringstream out;
    const double mib = static_cast<double>(bytes) / static_cast<double>(kBytesPerMebibyte);
    if (mib < 1024.0) {
        out << std::fixed << std::setprecision(1) << mib << " MiB";
    } else {
        out << std::fixed << std::setprecision(2) << (mib / 1024.0) << " GiB";
    }
    return out.str();
}

std::string formatRate(double bytesPerSecond) {
    std::ostringstream out;
    const double mbps = bytesPerSecond * 8.0 / 1000.0 / 1000.0;
    out << std::fixed << std::setprecision(2) << mbps << " Mbps";
    return out.str();
}

struct CpuTimes {
    uint64_t idle = 0;
    uint64_t total = 0;
};

struct MemoryInfo {
    uint64_t totalBytes = 0;
    uint64_t availableBytes = 0;

    double usedPercent() const {
        if (totalBytes == 0) {
            return 0.0;
        }
        const uint64_t used = totalBytes > availableBytes ? totalBytes - availableBytes : 0;
        return static_cast<double>(used) * 100.0 / static_cast<double>(totalBytes);
    }
};

struct NetworkSnapshot {
    uint64_t bytes = 0;
    uint64_t capacityBps = 0;
};

#ifdef _WIN32
uint64_t fileTimeToUInt64(const FILETIME& fileTime) {
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}
#endif

CpuTimes readCpuTimes() {
#ifdef _WIN32
    FILETIME idleTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        throw std::runtime_error("GetSystemTimes failed");
    }

    const uint64_t idle = fileTimeToUInt64(idleTime);
    const uint64_t kernel = fileTimeToUInt64(kernelTime);
    const uint64_t user = fileTimeToUInt64(userTime);
    return CpuTimes{idle, kernel + user};
#else
    std::ifstream input("/proc/stat");
    if (!input) {
        throw std::runtime_error("Unable to read /proc/stat");
    }

    std::string cpuLabel;
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    input >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    if (cpuLabel != "cpu") {
        throw std::runtime_error("Unexpected /proc/stat format");
    }

    const uint64_t idleAll = idle + iowait;
    const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
    return CpuTimes{idleAll, total};
#endif
}

double calculateCpuPercent(const CpuTimes& previous, const CpuTimes& current) {
    if (current.total <= previous.total || current.idle < previous.idle) {
        return 0.0;
    }

    const uint64_t totalDelta = current.total - previous.total;
    const uint64_t idleDelta = current.idle - previous.idle;
    if (totalDelta == 0 || idleDelta > totalDelta) {
        return 0.0;
    }

    return static_cast<double>(totalDelta - idleDelta) * 100.0 / static_cast<double>(totalDelta);
}

MemoryInfo readMemoryInfo() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        throw std::runtime_error("GlobalMemoryStatusEx failed");
    }
    return MemoryInfo{status.ullTotalPhys, status.ullAvailPhys};
#else
    std::ifstream input("/proc/meminfo");
    if (!input) {
        throw std::runtime_error("Unable to read /proc/meminfo");
    }

    uint64_t totalKb = 0;
    uint64_t availableKb = 0;
    uint64_t freeKb = 0;
    uint64_t buffersKb = 0;
    uint64_t cachedKb = 0;

    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
            totalKb = value;
        } else if (key == "MemAvailable:") {
            availableKb = value;
        } else if (key == "MemFree:") {
            freeKb = value;
        } else if (key == "Buffers:") {
            buffersKb = value;
        } else if (key == "Cached:") {
            cachedKb = value;
        }
    }

    if (availableKb == 0) {
        availableKb = freeKb + buffersKb + cachedKb;
    }
    return MemoryInfo{totalKb * 1024ull, availableKb * 1024ull};
#endif
}

NetworkSnapshot readNetworkSnapshot(uint64_t fallbackCapacityBps) {
    NetworkSnapshot snapshot{};

#ifdef _WIN32
    ULONG tableSize = 0;
    DWORD result = GetIfTable(nullptr, &tableSize, FALSE);
    if (result != ERROR_INSUFFICIENT_BUFFER || tableSize == 0) {
        snapshot.capacityBps = fallbackCapacityBps;
        return snapshot;
    }

    std::vector<uint8_t> tableBuffer(tableSize);
    auto* table = reinterpret_cast<MIB_IFTABLE*>(tableBuffer.data());
    result = GetIfTable(table, &tableSize, FALSE);
    if (result != NO_ERROR) {
        snapshot.capacityBps = fallbackCapacityBps;
        return snapshot;
    }

    constexpr DWORD operationalStatus = 5;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IFROW& row = table->table[i];
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK || row.dwOperStatus != operationalStatus) {
            continue;
        }

        snapshot.bytes += static_cast<uint64_t>(row.dwInOctets) + static_cast<uint64_t>(row.dwOutOctets);
        if (row.dwSpeed > 0) {
            snapshot.capacityBps += static_cast<uint64_t>(row.dwSpeed);
        }
    }
#else
    std::ifstream input("/proc/net/dev");
    if (!input) {
        snapshot.capacityBps = fallbackCapacityBps;
        return snapshot;
    }

    std::string line;
    std::getline(input, line);
    std::getline(input, line);

    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string interfaceName = trim(line.substr(0, colon));
        if (interfaceName.empty() || interfaceName == "lo") {
            continue;
        }

        std::ifstream operState("/sys/class/net/" + interfaceName + "/operstate");
        std::string state;
        if (operState && std::getline(operState, state) && trim(state) != "up") {
            continue;
        }

        std::istringstream values(line.substr(colon + 1));
        uint64_t rxBytes = 0;
        uint64_t txBytes = 0;
        uint64_t ignored = 0;
        values >> rxBytes;
        for (int i = 0; i < 7; ++i) {
            values >> ignored;
        }
        values >> txBytes;
        snapshot.bytes += rxBytes + txBytes;

        std::ifstream speedFile("/sys/class/net/" + interfaceName + "/speed");
        int64_t speedMbps = 0;
        if (speedFile >> speedMbps) {
            if (speedMbps > 0) {
                snapshot.capacityBps += static_cast<uint64_t>(speedMbps) * 1000ull * 1000ull;
            }
        }
    }
#endif

    if (snapshot.capacityBps == 0) {
        snapshot.capacityBps = fallbackCapacityBps;
    }
    return snapshot;
}

double calculateNetworkPercent(
    const NetworkSnapshot& previous,
    const NetworkSnapshot& current,
    double elapsedSeconds
) {
    if (elapsedSeconds <= 0.0 || current.capacityBps == 0 || current.bytes < previous.bytes) {
        return 0.0;
    }

    const uint64_t byteDelta = current.bytes - previous.bytes;
    const double bitsPerSecond = static_cast<double>(byteDelta) * 8.0 / elapsedSeconds;
    return bitsPerSecond * 100.0 / static_cast<double>(current.capacityBps);
}

class CpuStressor {
public:
    CpuStressor(bool enabled, unsigned logicalCpuCount)
        : enabled_(enabled),
          logicalCpuCount_(std::max(1u, logicalCpuCount)),
          workerCount_(enabled ? std::max(1u, logicalCpuCount) : 0u) {}

    void start() {
        if (!enabled_) {
            return;
        }

        running_.store(true);
        workers_.reserve(workerCount_);
        for (unsigned i = 0; i < workerCount_; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    void updatePressurePercent(double pressurePercent) {
        if (!enabled_) {
            return;
        }

        const double boundedPressure = clampDouble(pressurePercent, 0.0, 100.0);
        const double desiredBusyCpus = boundedPressure * static_cast<double>(logicalCpuCount_) / 100.0;
        const double workerDuty = workerCount_ == 0
            ? 0.0
            : clampDouble(desiredBusyCpus / static_cast<double>(workerCount_), 0.0, 1.0);

        duty_.store(workerDuty);
        actualPressurePercent_.store(
            workerDuty * static_cast<double>(workerCount_) * 100.0 / static_cast<double>(logicalCpuCount_)
        );
    }

    double pressurePercent() const {
        return actualPressurePercent_.load();
    }

    void stop() {
        running_.store(false);
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    ~CpuStressor() {
        stop();
    }

private:
    void workerLoop() {
        volatile uint64_t value = 0;
        const auto period = std::chrono::milliseconds(kCpuPeriodMilliseconds);

        while (running_.load()) {
            const double duty = duty_.load();
            if (duty <= 0.0) {
                std::this_thread::sleep_for(period);
                continue;
            }

            const auto busyTime = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(kCpuPeriodMilliseconds) * duty / 1000.0)
            );
            const auto periodStart = std::chrono::steady_clock::now();
            const auto busyUntil = periodStart + busyTime;

            while (running_.load() && std::chrono::steady_clock::now() < busyUntil) {
                for (int i = 0; i < 4096; ++i) {
                    value = value * 1664525ull + 1013904223ull;
                }
            }

            const auto elapsed = std::chrono::steady_clock::now() - periodStart;
            if (elapsed < period) {
                std::this_thread::sleep_for(period - elapsed);
            }
        }
    }

    bool enabled_ = true;
    unsigned logicalCpuCount_ = 1;
    unsigned workerCount_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<double> duty_{0.0};
    std::atomic<double> actualPressurePercent_{0.0};
    std::vector<std::thread> workers_;
};

class MemoryStressor {
public:
    explicit MemoryStressor(uint64_t blockBytes) : blockBytes_(blockBytes) {}

    void adjust(double measuredPercent, double targetPercent, uint64_t totalBytes, uint64_t maxBytes) {
        const double gapPercent = targetPercent - measuredPercent;
        const auto deltaBytes = static_cast<int64_t>(
            static_cast<double>(totalBytes) * gapPercent / 100.0
        );

        std::lock_guard<std::mutex> lock(mutex_);
        int64_t desiredBytes = static_cast<int64_t>(heldBytes_) + deltaBytes;
        if (desiredBytes < 0) {
            desiredBytes = 0;
        }

        uint64_t boundedDesired = static_cast<uint64_t>(desiredBytes);
        if (maxBytes > 0) {
            boundedDesired = std::min(boundedDesired, maxBytes);
        }

        resizeLocked(boundedDesired);
    }

    uint64_t heldBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heldBytes_;
    }

    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        heldBytes_ = 0;
    }

private:
    void resizeLocked(uint64_t desiredBytes) {
        while (heldBytes_ < desiredBytes) {
            const uint64_t remaining = desiredBytes - heldBytes_;
            const uint64_t nextBlockBytes = std::min(blockBytes_, remaining);
            if (nextBlockBytes == 0) {
                break;
            }

            try {
                std::vector<uint8_t> block(static_cast<size_t>(nextBlockBytes), 0);
                for (size_t i = 0; i < block.size(); i += 4096) {
                    block[i] = static_cast<uint8_t>(i);
                }
                if (!block.empty()) {
                    block.back() = 1;
                }
                heldBytes_ += nextBlockBytes;
                blocks_.push_back(std::move(block));
            } catch (const std::bad_alloc&) {
                break;
            }
        }

        while (heldBytes_ > desiredBytes && !blocks_.empty()) {
            heldBytes_ -= static_cast<uint64_t>(blocks_.back().size());
            blocks_.pop_back();
        }
    }

    uint64_t blockBytes_ = kMemoryBlockBytes;
    mutable std::mutex mutex_;
    uint64_t heldBytes_ = 0;
    std::vector<std::vector<uint8_t>> blocks_;
};

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void closeSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        closesocket(socketHandle);
    }
}

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        initialized_ = true;
    }

    ~SocketRuntime() {
        if (initialized_) {
            WSACleanup();
        }
    }

private:
    bool initialized_ = false;
};
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void closeSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        close(socketHandle);
    }
}

class SocketRuntime {
public:
    SocketRuntime() = default;
};
#endif

bool isLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

class NetworkStressor {
public:
    NetworkStressor(bool enabled, std::string host, uint16_t port, bool localSink)
        : enabled_(enabled),
          host_(std::move(host)),
          port_(port),
          localSink_(localSink) {}

    void start() {
        if (!enabled_) {
            return;
        }

        running_.store(true);
        if (localSink_) {
            receiver_ = std::thread([this]() { receiverLoop(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sender_ = std::thread([this]() { senderLoop(); });
    }

    void updateBytesPerSecond(double bytesPerSecond) {
        if (!enabled_) {
            return;
        }
        bytesPerSecond_.store(std::max(0.0, bytesPerSecond));
    }

    double currentBytesPerSecond() const {
        return bytesPerSecond_.load();
    }

    void stop() {
        running_.store(false);
        if (sender_.joinable()) {
            sender_.join();
        }
        if (receiver_.joinable()) {
            receiver_.join();
        }
    }

    ~NetworkStressor() {
        stop();
    }

private:
    void senderLoop() {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* result = nullptr;
        const std::string portText = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
            return;
        }

        SocketHandle socketHandle = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (socketHandle == kInvalidSocket) {
            freeaddrinfo(result);
            return;
        }

        std::vector<char> packet(1200, 'm');
        auto last = std::chrono::steady_clock::now();
        double tokens = 0.0;

        while (running_.load()) {
            const double rate = bytesPerSecond_.load();
            if (rate <= 1.0) {
                tokens = 0.0;
                last = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - last).count();
            last = now;
            tokens = std::min(tokens + elapsed * rate, rate * 0.25 + static_cast<double>(packet.size()));

            int burstCount = 0;
            while (running_.load() && tokens >= static_cast<double>(packet.size()) && burstCount < 128) {
#ifdef _WIN32
                const int sent = sendto(
                    socketHandle,
                    packet.data(),
                    static_cast<int>(packet.size()),
                    0,
                    result->ai_addr,
                    static_cast<int>(result->ai_addrlen)
                );
#else
                const ssize_t sent = sendto(
                    socketHandle,
                    packet.data(),
                    packet.size(),
                    0,
                    result->ai_addr,
                    result->ai_addrlen
                );
#endif
                if (sent <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    break;
                }

                tokens -= static_cast<double>(sent);
                ++burstCount;
            }

            if (tokens < static_cast<double>(packet.size())) {
                const double missingBytes = static_cast<double>(packet.size()) - tokens;
                const auto sleepTime = std::chrono::duration<double>(std::min(0.02, missingBytes / rate));
                std::this_thread::sleep_for(sleepTime);
            }
        }

        closeSocket(socketHandle);
        freeaddrinfo(result);
    }

    void receiverLoop() {
        SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == kInvalidSocket) {
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closeSocket(socketHandle);
            return;
        }

#ifdef _WIN32
        DWORD timeoutMilliseconds = 100;
        setsockopt(
            socketHandle,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMilliseconds),
            sizeof(timeoutMilliseconds)
        );
#else
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

        std::vector<char> buffer(65536);
        while (running_.load()) {
#ifdef _WIN32
            recvfrom(socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
#else
            recvfrom(socketHandle, buffer.data(), buffer.size(), 0, nullptr, nullptr);
#endif
        }

        closeSocket(socketHandle);
    }

    bool enabled_ = true;
    std::string host_;
    uint16_t port_ = kDefaultNetworkPort;
    bool localSink_ = true;
    std::atomic<bool> running_{false};
    std::atomic<double> bytesPerSecond_{0.0};
    std::thread sender_;
    std::thread receiver_;
};

struct Options {
    double cpuTargetPercent = 10.0;
    double memoryTargetPercent = 10.0;
    double networkTargetPercent = 10.0;
    int durationSeconds = kDefaultDurationSeconds;
    int sampleIntervalSeconds = kDefaultSampleIntervalSeconds;
    uint64_t fallbackNetworkCapacityBps = kDefaultNetworkCapacityBps;
    std::string networkHost = "127.0.0.1";
    uint16_t networkPort = kDefaultNetworkPort;
    bool localNetworkSink = true;
    bool cpuEnabled = true;
    bool memoryEnabled = true;
    bool networkEnabled = true;
    bool quiet = false;
    uint64_t maxMemoryBytes = 0;
};

double parseDouble(const std::string& text, const std::string& optionName) {
    try {
        size_t position = 0;
        const double value = std::stod(text, &position);
        if (position != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + optionName + ": " + text);
    }
}

int parseInt(const std::string& text, const std::string& optionName) {
    try {
        size_t position = 0;
        const int value = std::stoi(text, &position);
        if (position != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + optionName + ": " + text);
    }
}

uint64_t parseUInt64(const std::string& text, const std::string& optionName) {
    try {
        size_t position = 0;
        const unsigned long long value = std::stoull(text, &position);
        if (position != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<uint64_t>(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + optionName + ": " + text);
    }
}

void validatePercent(double value, const std::string& optionName) {
    if (value < 0.0 || value > 100.0) {
        throw std::runtime_error(optionName + " must be between 0 and 100");
    }
}

void printHelp() {
    std::cout
        << "mock-load\n"
        << "\n"
        << "Options:\n"
        << "  --target PERCENT             Set CPU, memory, and network targets.\n"
        << "  --cpu-target PERCENT         Set the CPU target.\n"
        << "  --memory-target PERCENT      Set the memory target.\n"
        << "  --network-target PERCENT     Set the network target.\n"
        << "  --duration SECONDS           Run time. Use 0 to run until Ctrl+C.\n"
        << "  --sample-interval SECONDS    Measurement interval.\n"
        << "  --network-cap-mbps MBPS      Fallback network capacity when link speed is unavailable.\n"
        << "  --network-host HOST          UDP target host. Defaults to 127.0.0.1.\n"
        << "  --network-port PORT          UDP target port. Defaults to 49010.\n"
        << "  --no-local-sink              Do not start the local UDP receiver.\n"
        << "  --max-memory-mb MB           Cap memory allocation. Use 0 for no cap.\n"
        << "  --no-cpu                     Disable CPU pressure.\n"
        << "  --no-memory                  Disable memory pressure.\n"
        << "  --no-network                 Disable network pressure.\n"
        << "  --quiet                      Print only startup and shutdown messages.\n"
        << "  --help                       Show help.\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;

    auto requireValue = [&](int& index, const std::string& optionName) -> std::string {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for " + optionName);
        }
        ++index;
        return argv[index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printHelp();
            std::cout.flush();
            std::exit(0);
        } else if (argument == "--target") {
            const double value = parseDouble(requireValue(i, argument), argument);
            options.cpuTargetPercent = value;
            options.memoryTargetPercent = value;
            options.networkTargetPercent = value;
        } else if (argument == "--cpu-target") {
            options.cpuTargetPercent = parseDouble(requireValue(i, argument), argument);
        } else if (argument == "--memory-target") {
            options.memoryTargetPercent = parseDouble(requireValue(i, argument), argument);
        } else if (argument == "--network-target") {
            options.networkTargetPercent = parseDouble(requireValue(i, argument), argument);
        } else if (argument == "--duration") {
            options.durationSeconds = parseInt(requireValue(i, argument), argument);
        } else if (argument == "--sample-interval") {
            options.sampleIntervalSeconds = parseInt(requireValue(i, argument), argument);
        } else if (argument == "--network-cap-mbps") {
            const double mbps = parseDouble(requireValue(i, argument), argument);
            if (mbps <= 0.0) {
                throw std::runtime_error("--network-cap-mbps must be greater than 0");
            }
            options.fallbackNetworkCapacityBps = static_cast<uint64_t>(mbps * 1000.0 * 1000.0);
        } else if (argument == "--network-host") {
            options.networkHost = requireValue(i, argument);
            options.localNetworkSink = isLoopbackHost(options.networkHost);
        } else if (argument == "--network-port") {
            const int port = parseInt(requireValue(i, argument), argument);
            if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
                throw std::runtime_error("--network-port must be between 1 and 65535");
            }
            options.networkPort = static_cast<uint16_t>(port);
        } else if (argument == "--no-local-sink") {
            options.localNetworkSink = false;
        } else if (argument == "--max-memory-mb") {
            options.maxMemoryBytes = parseUInt64(requireValue(i, argument), argument) * kBytesPerMebibyte;
        } else if (argument == "--no-cpu") {
            options.cpuEnabled = false;
        } else if (argument == "--no-memory") {
            options.memoryEnabled = false;
        } else if (argument == "--no-network") {
            options.networkEnabled = false;
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else {
            throw std::runtime_error("Unknown option: " + argument);
        }
    }

    validatePercent(options.cpuTargetPercent, "--cpu-target");
    validatePercent(options.memoryTargetPercent, "--memory-target");
    validatePercent(options.networkTargetPercent, "--network-target");

    if (options.durationSeconds < 0) {
        throw std::runtime_error("--duration must be 0 or greater");
    }
    if (options.sampleIntervalSeconds <= 0) {
        throw std::runtime_error("--sample-interval must be greater than 0");
    }

    return options;
}

double estimateBackgroundPercent(double measuredPercent, double appliedPressurePercent) {
    return std::max(0.0, measuredPercent - appliedPressurePercent);
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        const Options options = parseOptions(argc, argv);
        SocketRuntime socketRuntime;

        const unsigned logicalCpuCount = std::max(1u, std::thread::hardware_concurrency());
        CpuStressor cpuStressor(options.cpuEnabled, logicalCpuCount);
        MemoryStressor memoryStressor(kMemoryBlockBytes);
        NetworkStressor networkStressor(
            options.networkEnabled,
            options.networkHost,
            options.networkPort,
            options.localNetworkSink
        );

        cpuStressor.start();
        networkStressor.start();

        std::cout
            << "mock-load started: cpu_target=" << options.cpuTargetPercent
            << "% memory_target=" << options.memoryTargetPercent
            << "% network_target=" << options.networkTargetPercent
            << "% duration=" << options.durationSeconds
            << "s\n";

        CpuTimes previousCpu = readCpuTimes();
        NetworkSnapshot previousNetwork = readNetworkSnapshot(options.fallbackNetworkCapacityBps);
        auto previousTime = std::chrono::steady_clock::now();
        const auto startTime = previousTime;

        double cpuPressurePercent = 0.0;
        double networkPressurePercent = 0.0;

        while (g_shouldRun.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(options.sampleIntervalSeconds));

            const auto now = std::chrono::steady_clock::now();
            if (options.durationSeconds > 0) {
                const auto elapsedRunSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
                if (elapsedRunSeconds > options.durationSeconds) {
                    break;
                }
            }

            const CpuTimes currentCpu = readCpuTimes();
            const NetworkSnapshot currentNetwork = readNetworkSnapshot(options.fallbackNetworkCapacityBps);
            const MemoryInfo memoryInfo = readMemoryInfo();
            const double elapsedSeconds = std::chrono::duration<double>(now - previousTime).count();

            const double measuredCpuPercent = calculateCpuPercent(previousCpu, currentCpu);
            const double measuredMemoryPercent = memoryInfo.usedPercent();
            const double measuredNetworkPercent = calculateNetworkPercent(
                previousNetwork,
                currentNetwork,
                elapsedSeconds
            );

            if (options.cpuEnabled) {
                const double backgroundCpuPercent = estimateBackgroundPercent(
                    measuredCpuPercent,
                    cpuPressurePercent
                );
                cpuPressurePercent = clampDouble(
                    options.cpuTargetPercent - backgroundCpuPercent,
                    0.0,
                    options.cpuTargetPercent
                );
                cpuStressor.updatePressurePercent(cpuPressurePercent);
            }

            if (options.memoryEnabled) {
                memoryStressor.adjust(
                    measuredMemoryPercent,
                    options.memoryTargetPercent,
                    memoryInfo.totalBytes,
                    options.maxMemoryBytes
                );
            } else {
                memoryStressor.releaseAll();
            }

            if (options.networkEnabled) {
                const uint64_t controlCapacityBps = options.localNetworkSink
                    ? options.fallbackNetworkCapacityBps
                    : (currentNetwork.capacityBps == 0
                        ? options.fallbackNetworkCapacityBps
                        : currentNetwork.capacityBps);
                const double currentNetworkPressureBytesPerSecond = networkStressor.currentBytesPerSecond();
                const double appliedNetworkPercent = currentNetworkPressureBytesPerSecond
                    * 8.0
                    * 100.0
                    / static_cast<double>(controlCapacityBps);
                const double controlNetworkPercent = options.localNetworkSink
                    ? measuredNetworkPercent + appliedNetworkPercent
                    : measuredNetworkPercent;
                const double backgroundNetworkPercent = estimateBackgroundPercent(
                    controlNetworkPercent,
                    networkPressurePercent
                );
                networkPressurePercent = clampDouble(
                    options.networkTargetPercent - backgroundNetworkPercent,
                    0.0,
                    options.networkTargetPercent
                );
                const double targetBytesPerSecond = static_cast<double>(controlCapacityBps)
                    * networkPressurePercent
                    / 100.0
                    / 8.0;
                networkStressor.updateBytesPerSecond(targetBytesPerSecond);
            } else {
                networkStressor.updateBytesPerSecond(0.0);
            }

            if (!options.quiet) {
                std::cout
                    << std::fixed << std::setprecision(1)
                    << "cpu=" << measuredCpuPercent << "%"
                    << " cpu_pressure=" << cpuStressor.pressurePercent() << "%"
                    << " memory=" << measuredMemoryPercent << "%"
                    << " memory_pressure=" << formatBytes(memoryStressor.heldBytes())
                    << " network=" << measuredNetworkPercent << "%"
                    << " network_pressure=" << formatRate(networkStressor.currentBytesPerSecond())
                    << "\n";
            }

            previousCpu = currentCpu;
            previousNetwork = currentNetwork;
            previousTime = now;
        }

        cpuStressor.updatePressurePercent(0.0);
        networkStressor.updateBytesPerSecond(0.0);
        memoryStressor.releaseAll();
        networkStressor.stop();
        cpuStressor.stop();

        std::cout << "mock-load stopped\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mock-load error: " << error.what() << "\n";
        return 1;
    }
}
