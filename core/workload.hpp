#pragma once
#include <cstdint>
#include <vector>
#include "ftl.hpp"
#include "nvme.hpp"

// ---------------------------------------------------------------------------
// Workloads: generate host commands, run them through the NVMe controller,
// and measure what the host would see (latency, throughput) and what the
// drive pays for it (QLC writes, WAF, folds, GC).
// ---------------------------------------------------------------------------

// Small deterministic random number generator (same LCG as the stress tests),
// so every experiment is repeatable: same seed -> same numbers.
class Rng {
public:
    explicit Rng(uint64_t seed) : x_(seed) {}
    uint64_t next() {                       // 31 random bits
        x_ = x_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return x_ >> 33;
    }
    uint64_t below(uint64_t n) { return next() % n; }                    // 0 .. n-1
    double   unit() { return double(next() & 0xFFFFFF) / double(0x1000000); }  // [0, 1)
private:
    uint64_t x_;
};

enum class Pattern {
    Sequential,      // LBA 0, 1, 2, ... wrapping at the end of the drive
    UniformRandom,   // every LBA equally likely
    HotCold,         // a small "hot" set of LBAs gets most of the writes
};

struct WorkloadConfig {
    Pattern  pattern        = Pattern::UniformRandom;
    double   hotFraction    = 0.2;   // HotCold: the first 20% of LBAs are hot...
    double   hotProbability = 0.8;   // ...and receive 80% of the writes
    uint32_t bursts         = 100;   // how many bursts to run
    uint32_t burstCommands  = 200;   // single-LBA write commands per burst
    uint64_t idleUs         = 0;     // idle time after each burst (0 = no idle, so no folding)
    bool     precondition   = true;  // fill the drive once before measuring (standard SSD benchmarking)
    uint64_t seed           = 1;
};

// Produces the LBA for each write, following the configured pattern.
class Workload {
public:
    Workload(const WorkloadConfig& cfg, uint64_t logicalPages);
    uint64_t nextLba();
    NvmeCommand nextWrite(uint16_t cid) { return {cid, NvmeOpcode::Write, nextLba(), 0}; }

private:
    WorkloadConfig cfg_;
    uint64_t logicalPages_;
    uint64_t hotLbas_;       // size of the hot set: LBAs [0, hotLbas_) are hot
    uint64_t seqNext_ = 0;   // next LBA for Sequential
    Rng      rng_;
};

// What one experiment measured. Everything counts the measured phase only
// (preconditioning is excluded).
struct ExperimentResult {
    uint64_t logicalPages = 0;       // drive capacity (a bigger SLC cache leaves less of it)
    uint64_t hostWrites = 0;
    double   avgLatencyUs = 0;       // mean completion latency per command
    uint64_t maxLatencyUs = 0;       // worst single command (the tail)
    uint64_t slowCommands = 0;       // completions slower than one SLC program: cliff or GC
    double   throughputMBps = 0;     // host data / time spent on host commands (4 KB per LBA)
    double   waf = 0;                // (SLC + QLC writes) / host writes
    double   qlcPerHost = 0;         // ALL QLC writes / host writes (cache traffic + QLC's own GC copies)
    double   qlcIngressPerHost = 0;  // host data that reached QLC (direct + folded) / host writes:
                                     // the SLC cache's effect alone. < 1 means the cache absorbed writes
    uint64_t folds = 0, absorbedPages = 0, gcRuns = 0, directQlcWrites = 0;
    std::vector<uint64_t> latenciesUs;   // every completion, in order (for charts later)
};

// Build a drive, optionally precondition it, run the workload through an
// NvmeController, and collect the results.
ExperimentResult runExperiment(const Config& drive, const WorkloadConfig& wl);

// ---- Step 2: queue depth ----
// A closed-loop host: it keeps `qd` write commands in flight. Whenever one completes, it submits the
// next one at that moment. This is how fio / real benchmarks measure SSDs ("4K random write, QD32").
struct QdResult {
    uint64_t commands = 0;
    uint64_t totalUs = 0;            // from the first submit to the last completion
    double   iops = 0;               // commands per second
    double   avgLatencyUs = 0;
    uint64_t p50Us = 0, p99Us = 0, maxUs = 0;
    uint64_t gcRuns = 0;
    std::vector<uint64_t> latenciesUs;   // per command, in submit order
};
QdResult runQueueDepth(const Config& drive, const WorkloadConfig& wl, uint32_t qd, uint64_t commands);

// The latency below which pct percent of the commands finished (pct = 50 -> median, 99 -> p99).
uint64_t percentile(std::vector<uint64_t> latencies, double pct);
