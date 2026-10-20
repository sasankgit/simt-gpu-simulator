// config.hpp - architectural parameters of the simulated GPU.
#pragma once

#include <cstdint>
#include <string>

namespace simt {

constexpr int kWarpSize = 32;
using LaneMask = uint32_t;
constexpr LaneMask kFullMask = 0xFFFFFFFFu;

enum class SchedPolicy { LRR, GTO };  // loose round robin / greedy-then-oldest

inline const char* to_string(SchedPolicy p) { return p == SchedPolicy::LRR ? "lrr" : "gto"; }

struct CacheConfig {
    uint32_t size_bytes;
    uint32_t line_bytes;
    uint32_t ways;
    uint32_t hit_latency;
};

struct GpuConfig {
    // ---- compute --------------------------------------------------------
    uint32_t num_sms = 4;
    uint32_t schedulers_per_sm = 2;       // issue slots per SM per cycle
    uint32_t max_warps_per_sm = 32;
    uint32_t max_blocks_per_sm = 8;
    uint32_t regs_per_sm = 32768;         // 32-bit registers in the register file
    uint32_t shared_bytes_per_sm = 49152;
    SchedPolicy policy = SchedPolicy::GTO;

    // ---- memory ---------------------------------------------------------
    uint32_t shared_banks = 32;
    uint32_t shared_latency = 22;
    CacheConfig l1{16 * 1024, 128, 4, 28};
    CacheConfig l2{256 * 1024, 128, 8, 90};
    uint32_t dram_latency = 260;
    uint32_t dram_bytes_per_cycle = 64;   // per-GPU peak DRAM bandwidth
    uint32_t segment_bytes = 128;         // coalescing granularity == L1 line

    // ---- bookkeeping ----------------------------------------------------
    uint64_t max_cycles = 200'000'000;    // watchdog for runaway kernels

    // Returns an empty string when valid, otherwise a diagnostic.
    std::string validate() const;
};

}  // namespace simt
