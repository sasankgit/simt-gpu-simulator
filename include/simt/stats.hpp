// stats.hpp - counters collected during simulation and the report printer.
#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace simt {

// What a resident warp was doing on a given cycle (one count per warp-cycle).
enum class WarpState : uint8_t {
    Issued = 0,    // won arbitration and issued
    Ready,         // eligible but another warp was picked
    WaitAlu,       // scoreboard: waiting on an ALU/FMA/SFU result
    WaitMem,       // scoreboard: waiting on a load result
    WaitBarrier,   // parked at a __syncthreads
    UnitBusy,      // functional unit / LSU occupied
    kCount
};

inline const char* to_string(WarpState s) {
    static const char* names[] = {"issued", "ready", "wait_alu", "wait_mem", "wait_barrier", "unit_busy"};
    return names[static_cast<int>(s)];
}

struct CacheStats {
    uint64_t accesses = 0, hits = 0, misses = 0, evictions = 0;
    double hit_rate() const { return accesses ? static_cast<double>(hits) / static_cast<double>(accesses) : 0.0; }
};

struct Stats {
    uint64_t cycles = 0;
    uint64_t warp_instructions = 0;     // one per issued warp instruction
    uint64_t thread_instructions = 0;   // sum of active lanes over issued instructions
    uint64_t branches = 0;
    uint64_t divergent_branches = 0;
    uint64_t barriers = 0;
    uint64_t blocks_completed = 0;

    // memory system
    uint64_t gld_requests = 0, gst_requests = 0, atomics = 0;
    uint64_t gld_transactions = 0, gst_transactions = 0;
    uint64_t gld_bytes_requested = 0, gst_bytes_requested = 0;
    uint64_t gld_bytes_moved = 0, gst_bytes_moved = 0;
    uint64_t shared_requests = 0, shared_conflict_cycles = 0, shared_bank_conflicts = 0;
    uint64_t dram_bytes = 0;
    CacheStats l1, l2;

    // issue slot accounting
    uint64_t issue_slots = 0;
    uint64_t issue_slots_used = 0;

    // occupancy samples (sum of resident warps over every SM-cycle)
    uint64_t resident_warp_cycles = 0;
    uint64_t sm_cycles = 0;
    uint32_t max_warps_per_sm = 0;

    std::array<uint64_t, static_cast<size_t>(WarpState::kCount)> warp_state_cycles{};
    std::array<uint64_t, 5> unit_issues{};  // ALU, FMA, SFU, LSU, CTRL

    // ---- derived metrics --------------------------------------------------
    double ipc() const { return cycles ? static_cast<double>(warp_instructions) / static_cast<double>(cycles) : 0.0; }
    double simd_efficiency() const {  // average active lanes / 32
        return warp_instructions ? static_cast<double>(thread_instructions) /
                                       (32.0 * static_cast<double>(warp_instructions))
                                 : 0.0;
    }
    double issue_utilisation() const {
        return issue_slots ? static_cast<double>(issue_slots_used) / static_cast<double>(issue_slots) : 0.0;
    }
    double achieved_occupancy() const {
        return (sm_cycles && max_warps_per_sm)
                   ? static_cast<double>(resident_warp_cycles) /
                         (static_cast<double>(sm_cycles) * max_warps_per_sm)
                   : 0.0;
    }
    double load_efficiency() const {  // requested bytes / bytes moved
        return gld_bytes_moved ? static_cast<double>(gld_bytes_requested) / static_cast<double>(gld_bytes_moved) : 1.0;
    }
    double store_efficiency() const {
        return gst_bytes_moved ? static_cast<double>(gst_bytes_requested) / static_cast<double>(gst_bytes_moved) : 1.0;
    }
    double transactions_per_load() const {
        return gld_requests ? static_cast<double>(gld_transactions) / static_cast<double>(gld_requests) : 0.0;
    }
    double dram_bandwidth_gbps(double clock_ghz) const {
        return cycles ? static_cast<double>(dram_bytes) / static_cast<double>(cycles) * clock_ghz : 0.0;
    }
};

struct KernelReport {
    std::string name;
    uint32_t grid = 0, block = 0;
    uint32_t regs_per_thread = 0, shared_bytes = 0;
    uint32_t blocks_per_sm = 0;               // resource-limited residency
    std::string occupancy_limiter;
    double theoretical_occupancy = 0.0;
    Stats stats;
};

void print_report(std::ostream& os, const KernelReport& r, bool color);

}  // namespace simt
