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
    std::printf("\n-- 5. parallelism: 4,096 sequential writes on a fresh drive (QLC 2500 us program, 10 us channel transfer)\n");
    std::printf("%-26s %12s %10s %9s\n", "geometry", "makespan us", "MB/s", "speedup");
    uint64_t base = 0;
    for (uint32_t dies : {1u, 2u, 4u, 8u, 16u}) {
        Config c = drive;
        c.slcBlocks = 0;
        c.channels = dies < 4 ? dies : 4;            // up to 4 channels, the rest share them
        c.diesPerChannel = dies / c.channels;
        c.channelXferUs = 10;
        Ftl ftl(c);
        for (uint64_t l = 0; l < 4096; ++l) ftl.write(l);
        const uint64_t t = ftl.makespanUs();
        if (dies == 1) base = t;
        char name[64];
        std::snprintf(name, sizeof name, "%u ch x %u dies = %u dies", c.channels, c.diesPerChannel, dies);
        std::printf("%-26s %12llu %10.1f %8.1fx\n", name, (unsigned long long)t, 4096.0 * 4096.0 / double(t), double(base) / double(t));
    }
    std::printf("\n-- 6. queue depth: 4 KB sequential writes, 8 dies (4 ch x 2), fresh drive\n");
    std::printf("%-6s %10s %12s %10s %10s\n", "QD", "IOPS", "avg lat us", "p50 us", "p99 us");
    for (uint32_t qd : {1u, 2u, 4u, 8u, 16u, 32u}) {
        Config c = drive; c.slcBlocks = 0; c.channels = 4; c.diesPerChannel = 2; c.channelXferUs = 10;
        WorkloadConfig wl; wl.pattern = Pattern::Sequential; wl.precondition = false;
        const QdResult r = runQueueDepth(c, wl, qd, 4096);
        std::printf("%-6u %10.0f %12.0f %10llu %10llu\n", qd, r.iops, r.avgLatencyUs,
                    (unsigned long long)r.p50Us, (unsigned long long)r.p99Us);
    }

    std::printf("\n-- 7. GC tail latency: 4 KB random writes on a full drive, 8 dies, QD8\n");
    {
        Config c = drive; c.slcBlocks = 0; c.channels = 4; c.diesPerChannel = 2; c.channelXferUs = 10;
        WorkloadConfig wl; wl.pattern = Pattern::UniformRandom; wl.precondition = true;
        const QdResult r = runQueueDepth(c, wl, 8, 40000);
        std::printf("IOPS %.0f   p50 %llu us   p99 %llu us   max %llu us   GC runs %llu\n", r.iops,
                    (unsigned long long)r.p50Us, (unsigned long long)r.p99Us, (unsigned long long)r.maxUs,
                    (unsigned long long)r.gcRuns);
    }
    std::printf("\n");
    return 0;
}
