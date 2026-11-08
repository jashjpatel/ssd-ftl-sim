// Runs the roadmap's four experiments and prints a results table.
// Build target: ftl_experiments.  These are the numbers for your README.
#include <cstdio>
#include "workload.hpp"

static void header() {
    std::printf("%-38s %9s %8s %7s %8s %6s %8s %8s %6s %9s\n",
                "experiment", "avg us", "max us", "slow", "MB/s", "WAF", "QLC/host", "ingress", "folds", "absorbed");
}

static void row(const char* name, const ExperimentResult& r) {
    std::printf("%-38s %9.0f %8llu %7llu %8.1f %6.2f %8.2f %8.2f %6llu %9llu\n",
                name, r.avgLatencyUs, (unsigned long long)r.maxLatencyUs, (unsigned long long)r.slowCommands,
                r.throughputMBps, r.waf, r.qlcPerHost, r.qlcIngressPerHost,
                (unsigned long long)r.folds, (unsigned long long)r.absorbedPages);
}

int main() {
    // 256 blocks x 64 pages, 16 SLC blocks (= 256 SLC pages of cache), 7% OP, realistic QLC timing
    Config drive;
    drive.timing = {80, 2500, 3000};
    const uint64_t idle = 5000000;   // 5 s between bursts: enough to fold the cache, including the QLC GC it triggers

    std::printf("\nDrive: %u blocks x %u pages, %u SLC blocks, %.0f%% OP, SLC program %u us, QLC program %u us\n\n",
                drive.blocks, drive.pagesPerBlock, drive.slcBlocks, drive.opRatio * 100,
                drive.slcTiming.programUs, drive.timing.programUs);
    header();

    WorkloadConfig w;

    std::printf("-- 1. the write cliff\n");
    w = {}; w.bursts = 1; w.burstCommands = 1000;
    row("1 burst of 1000 (> 256-page cache)", runExperiment(drive, w));

    std::printf("-- 2. idle time lets the cache recover\n");
    w = {}; w.bursts = 100; w.burstCommands = 200;
    row("bursts of 200, no idle", runExperiment(drive, w));
    w.idleUs = idle;
    row("bursts of 200, 5 s idle", runExperiment(drive, w));

    std::printf("-- 3. hot data is absorbed by the cache (5 s idle)\n");
    w = {}; w.bursts = 100; w.burstCommands = 200; w.idleUs = idle;
    row("uniform random", runExperiment(drive, w));
    w.pattern = Pattern::HotCold; w.hotFraction = 0.2;
    row("hot/cold: 80% of writes -> 20% of LBAs", runExperiment(drive, w));
    w.hotFraction = 0.01;
    row("hot/cold: 80% of writes -> 1% of LBAs", runExperiment(drive, w));

    std::printf("-- 4. cache size sweep (hot/cold 1%%, bursts of 400, 5 s idle)\n");
    for (uint32_t n : {8u, 16u, 32u}) {
        Config c = drive; c.slcBlocks = n;
        w = {}; w.pattern = Pattern::HotCold; w.hotFraction = 0.01; w.bursts = 100; w.burstCommands = 400; w.idleUs = idle;
        const ExperimentResult r = runExperiment(c, w);
        char name[64];
        std::snprintf(name, sizeof name, "%2u SLC blocks (capacity %llu LBAs)", n, (unsigned long long)r.logicalPages);
        row(name, r);
    }
    std::printf("\n");
    return 0;
}