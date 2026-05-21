/**
 * @file bench_common.hpp
 * @brief Shared utilities for benchmark scenarios and the measurement runner.
 */

#pragma once

#include "matching/order_book.hpp"

#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace benchmark_runner {

/**
 * @brief Minimal splitmix64 PRNG — ~2 ns/call, deterministic, seedable.
 */
struct SplitMix64 {
    std::uint64_t state;

    explicit SplitMix64(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        // splitmix64: one of the fastest high-quality 64-bit PRNGs.
        state += 0x9e3779b97f4a7c15;           // Add golden ratio.
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;  // Xorshift + multiply.
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;  // Second mixing round.
        return z ^ (z >> 31);                       // Final avalanche.
    }
};

/**
 * @brief Thin wrapper around the perf_event_open syscall.
 */
inline int perf_event_open(struct perf_event_attr* hw_event, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return static_cast<int>(
            syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

/**
 * @brief Manages a group of Linux perf_event hardware counters.
 */
class PerfGroup {
    public:
    /**
     * @brief Open all five counters as a single event group.
     */
    bool Open() {
        CloseAll();
        // Open the group leader first (group_fd = -1).
        if (!OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1)) {
            return false;
        }
        // Subsequent counters join the group by passing the leader FD.
        if (!OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, leader_fd_)) {
            return false;
        }
        if (!OpenCounter(
                        PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, leader_fd_)) {
            return false;
        }
        if (!OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, leader_fd_)) {
            return false;
        }
        if (!OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, leader_fd_)) {
            return false;
        }
        return true;
    }

    /**
     * @brief Reset all counters to zero and start counting.
     */
    bool ResetEnable() const {
        if (leader_fd_ < 0) return false;
        if (ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1) {
            return false;
        }
        if (ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == -1) {
            return false;
        }
        return true;
    }

    /**
     * @brief Freeze (disable) the entire counter group.
     */
    bool Disable() const {
        if (leader_fd_ < 0) return false;
        return ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != -1;
    }

    /**
     * @brief Atomically read all counter values from the group.
     */
    bool ReadValues(std::array<std::uint64_t, 5>& out) const {
        if (leader_fd_ < 0) return false;
        // The read() returns a header (nr) followed by the counter values.
        struct ReadData {
            std::uint64_t nr;         // Number of counters in the group.
            std::uint64_t values[5];  // Values in the order they were opened.
        } data{};
        const ssize_t n = read(leader_fd_, &data, sizeof(data));
        if (n != static_cast<ssize_t>(sizeof(data)) || data.nr != 5) {
            return false;
        }
        for (std::size_t i = 0; i < 5; ++i) out[i] = data.values[i];
        return true;
    }

    ~PerfGroup() { CloseAll(); }

    private:
    /**
     * @brief Open a single counter and join it to the group.
     */
    bool OpenCounter(std::uint64_t type, std::uint64_t config, int group_fd) {
        struct perf_event_attr pe {};
        pe.type = type;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = config;
        pe.disabled = 1;        // Start disabled; ResetEnable() activates the group.
        pe.exclude_kernel = 1;  // Count user-space only (avoids context-switch noise).
        pe.exclude_hv = 1;
        pe.read_format = PERF_FORMAT_GROUP;  // Read atomically as a group.

        const int fd = perf_event_open(&pe, 0, -1, group_fd, 0);
        if (fd == -1) {
            std::cerr << "perf_event_open failed: " << std::strerror(errno) << "\n";
            CloseAll();
            return false;
        }
        // The first counter (group_fd == -1) becomes the leader.
        if (group_fd == -1) leader_fd_ = fd;
        fds_.push_back(fd);
        return true;
    }

    void CloseAll() {
        for (const int fd : fds_) {
            if (fd >= 0) close(fd);
        }
        fds_.clear();
        leader_fd_ = -1;
    }

    int leader_fd_{-1};
    std::vector<int> fds_{};
};

/**
 * @brief Ensure a CSV file has a header row.
 */
inline void EnsureCsvHeader(const std::string& path, const std::string& header) {
    if (path.empty()) return;
    std::error_code ec;
    // Only write the header if the file is new or empty (idempotent).
    if (std::filesystem::exists(path, ec) &&
            std::filesystem::file_size(path, ec) > 0) {
        return;
    }
    std::ofstream f(path, std::ios::app);
    f << header << "\n";
}

/**
 * @brief Compute the p-th percentile of a numeric vector (R-7 interpolation).
 */
inline double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    // R-7 interpolation: pos = p * (n - 1), then linear blend between the
    // two neighbours.  Matches numpy's default percentile behaviour.
    const double pos = p * (values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - lo;
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

/**
 * @brief Prefill an OrderBook with resting sell orders at increasing prices.
 */
inline void PrefillSellBook(matching::OrderBook& book, std::uint64_t orders,
                            std::uint64_t levels, std::uint64_t id_base) {
    // Distribute orders evenly across the requested number of price levels.
    const std::uint64_t per_level =
            std::max<std::uint64_t>(1, orders / std::max<std::uint64_t>(1, levels));
    std::uint64_t id = id_base;
    for (std::uint64_t lvl = 0; lvl < levels; ++lvl) {
        const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
        for (std::uint64_t j = 0; j < per_level; ++j) {
            (void)book.add_limit_order(id, matching::Side::Sell, ask_price, 1, id);
            ++id;
        }
    }
}

}  // namespace benchmark_runner
