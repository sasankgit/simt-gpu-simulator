// memory.hpp - device memory, address coalescing, shared-memory banks, caches.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "simt/config.hpp"
#include "simt/stats.hpp"

namespace simt {

class MemoryFault : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Flat device global memory with a bump allocator.  Address 0 is reserved so
// that a null pointer faults.
// ---------------------------------------------------------------------------
class GlobalMemory {
public:
    explicit GlobalMemory(size_t capacity_bytes = 64u << 20);

    // Allocates `bytes` (aligned to 256 B like cudaMalloc) and returns the device address.
    uint32_t alloc(size_t bytes);
    void reset();

    uint32_t load32(uint32_t addr) const;
    void store32(uint32_t addr, uint32_t value);
    uint32_t atomic_add32(uint32_t addr, uint32_t value);

    // Typed host <-> device copies.
    template <typename T>
    void copy_in(uint32_t dev, const std::vector<T>& host) {
        write_bytes(dev, host.data(), host.size() * sizeof(T));
    }
    template <typename T>
    std::vector<T> copy_out(uint32_t dev, size_t count) const {
        std::vector<T> out(count);
        read_bytes(dev, out.data(), count * sizeof(T));
        return out;
    }

    void write_bytes(uint32_t dev, const void* src, size_t n);
    void read_bytes(uint32_t dev, void* dst, size_t n) const;
    size_t used() const { return brk_; }
    size_t capacity() const { return bytes_.size(); }

private:
    void check(uint32_t addr, size_t n) const;
    std::vector<uint8_t> bytes_;
    size_t brk_ = 256;
};

// ---------------------------------------------------------------------------
// Per-block shared memory.
// ---------------------------------------------------------------------------
class SharedMemory {
public:
    explicit SharedMemory(uint32_t bytes = 0) : bytes_(bytes, 0) {}
    uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
    uint32_t load32(uint32_t addr) const;
    void store32(uint32_t addr, uint32_t value);

private:
    void check(uint32_t addr) const;
    std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------------------
// Coalescing: map the per-lane addresses of one warp-wide access to the set of
// distinct memory segments (transactions) it touches.
// ---------------------------------------------------------------------------
struct CoalesceResult {
    std::vector<uint32_t> segments;   // base addresses, in first-touch order
    uint32_t active_lanes = 0;
    uint32_t bytes_requested = 0;     // 4 bytes per active lane
    uint32_t moved = 0;               // segments * segment size (bytes actually transferred)
};

CoalesceResult coalesce(const std::array<uint32_t, kWarpSize>& addr, LaneMask active, uint32_t segment_bytes);

// ---------------------------------------------------------------------------
// Shared-memory bank conflict analysis.  Returns the serialisation degree: the
// largest number of *distinct words* mapped to any single bank (accesses to
// the same word are a broadcast and do not conflict).  Degree 1 == no conflict.
// ---------------------------------------------------------------------------
uint32_t bank_conflict_degree(const std::array<uint32_t, kWarpSize>& addr, LaneMask active, uint32_t banks);

// ---------------------------------------------------------------------------
// Set-associative cache with true-LRU replacement (tags only; data lives in
// GlobalMemory).  Used for both L1 (per SM) and L2 (shared).
// ---------------------------------------------------------------------------
class Cache {
public:
    explicit Cache(const CacheConfig& cfg);

    // Returns true on hit.  On a miss the line is allocated (evicting LRU).
    bool access(uint32_t addr, bool allocate_on_miss = true);
    bool probe(uint32_t addr) const;
    void invalidate_all();

    const CacheStats& stats() const { return stats_; }
    const CacheConfig& config() const { return cfg_; }
    uint32_t sets() const { return sets_; }

private:
    struct Way {
        uint64_t tag = 0;
        uint64_t last_use = 0;
        bool valid = false;
    };
    uint32_t index_of(uint32_t addr) const { return (addr / cfg_.line_bytes) % sets_; }
    uint64_t tag_of(uint32_t addr) const { return (addr / cfg_.line_bytes) / sets_; }

    CacheConfig cfg_;
    uint32_t sets_;
    uint64_t tick_ = 0;
    std::vector<Way> ways_;
    CacheStats stats_;
};

// ---------------------------------------------------------------------------
// Shared L2 + DRAM model: answers "how many cycles until this line arrives".
// DRAM bandwidth is modelled as a queue that drains dram_bytes_per_cycle.
// ---------------------------------------------------------------------------
class MemoryHierarchy {
public:
    MemoryHierarchy(const GpuConfig& cfg, Stats& stats);

    // Latency of fetching one line that missed in L1 at time `now`.
    uint32_t read_line(uint32_t addr, uint64_t now);
    // Write-through: charges L2/DRAM traffic, never stalls the issuing warp.
    void write_line(uint32_t addr, uint64_t now);

    Cache& l2() { return l2_; }

private:
    uint32_t dram_access(uint64_t now, uint32_t bytes);

    GpuConfig cfg_;
    Stats& stats_;
    Cache l2_;
    uint64_t dram_free_at_ = 0;
};

}  // namespace simt
