#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <climits>
#include "ftl.hpp"
#include "nvme.hpp"
#include "workload.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); ++failures; } } while (0)

// Tiny drive from the test-case walkthrough: 4 blocks x 4 pages, 8 logical pages.
static Config tinyConfig() {
    Config c;
    c.blocks = 4; c.pagesPerBlock = 4; c.opRatio = 0.5;
    c.slcBlocks = 0;                 // no SLC cache: TC1-TC7 test the plain QLC FTL
    return c;
}

// SLC drive: 8 blocks x 8 pages. B0-B1 are SLC (8/4 = 2 pages each -> 4 SLC pages total),
// B2-B7 are QLC (48 pages). Logical capacity comes from QLC only: 48 * 0.75 = 36.
static Config slcConfig() {
    Config c;
    c.blocks = 8; c.pagesPerBlock = 8; c.opRatio = 0.25;
    c.slcBlocks = 2;
    c.timing    = {80, 2500, 3000};  // QLC: slow program
    c.slcTiming = {20, 150, 3000};   // SLC: fast program
    return c;
}

static void printBlocks(const Ftl& ftl) {
    const Nand& n = ftl.nand();
    for (uint32_t b = 0; b < n.numBlocks(); ++b) {
        std::printf("  B%u %s [", b, n.block(b).mode == CellMode::SLC ? "SLC" : "QLC");
        for (uint32_t p = 0; p < n.block(b).capacity; ++p) { auto s = n.block(b).pages[p]; std::printf(" %c", s == PageState::Free ? '.' : s == PageState::Valid ? 'V' : 'x'); }
        std::printf(" ] valid=%u erase=%u\n", n.block(b).validCount, n.block(b).eraseCount);
    }
}

static void tc1_first_writes() {
    std::printf("TC1: first writes\n");
    Ftl ftl(tinyConfig());
    for (uint64_t l = 0; l < 4; ++l) ftl.write(l);
    printBlocks(ftl);
    CHECK(ftl.read(2) == 2);
    CHECK(ftl.nand().block(0).validCount == 4);
    CHECK(ftl.freeBlocks() == 3);
    CHECK(ftl.stats.waf() == 1.0);
    CHECK(ftl.checkInvariants());
}

static void tc2_overwrite() {
    std::printf("TC2: overwrite (out-of-place update)\n");
    Ftl ftl(tinyConfig());
    for (uint64_t l = 0; l < 4; ++l) ftl.write(l);
    ftl.write(1);
    printBlocks(ftl);
    CHECK(ftl.read(1) == 4);                                   // moved to B1 page 0
    CHECK(ftl.nand().block(0).pages[1] == PageState::Invalid); // old copy invalid
    CHECK(ftl.nand().block(0).validCount == 3);
    CHECK(ftl.freeBlocks() == 2);
    CHECK(ftl.checkInvariants());
}

static void tc3_reads() {
    std::printf("TC3: reads\n");
    Ftl ftl(tinyConfig());
    ftl.write(1);
    const uint64_t t0 = ftl.stats.simTimeUs;
    CHECK(ftl.read(1) == 0);
    CHECK(ftl.stats.simTimeUs - t0 == 25);   // mapped read hits NAND
    CHECK(ftl.read(6) == -1);
    CHECK(ftl.stats.simTimeUs - t0 == 25);   // unmapped read costs nothing
}

static void trim_and_edges() {
    std::printf("TRIM + edge cases\n");
    Ftl ftl(tinyConfig());
    ftl.write(5);
    ftl.trim(5);
    CHECK(ftl.read(5) == -1);
    CHECK(ftl.nand().block(0).validCount == 0);
    CHECK(ftl.checkInvariants());

    bool rejected = false;
    try { ftl.write(8); } catch (const std::out_of_range&) { rejected = true; }
    CHECK(rejected);                         // LPN 8 is beyond 8-page capacity

    Nand n(1, 4);
    bool outOfOrder = false;
    try { n.program(2, 0); } catch (const std::logic_error&) { outOfOrder = true; }
    CHECK(outOfOrder);                       // must program page 0 first
}

// Builds the TC4 state from the walkthrough:
//   B0 [x x x x]  B1 [1 4 x 6]  B2 [0 2 3 5]  B3 [. . . .]   pool {B3}
static void buildTc4(Ftl& ftl) {
    for (uint64_t l : {0, 1, 2, 3}) ftl.write(l);
    ftl.write(1);
    for (uint64_t l : {4, 5, 6}) ftl.write(l);
    for (uint64_t l : {0, 2, 3, 5}) ftl.write(l);
}

static void tc4_setup() {
    std::printf("TC4: drive fills, no GC yet\n");
    Ftl ftl(tinyConfig());
    buildTc4(ftl);
    printBlocks(ftl);
    CHECK(ftl.nand().block(0).validCount == 0);
    CHECK(ftl.nand().block(1).validCount == 3);
    CHECK(ftl.freeBlocks() == 1);
    CHECK(ftl.stats.gcRuns == 0);
    CHECK(ftl.stats.waf() == 1.0);
}

static void tc5_free_victim_and_wear_leveling() {
    std::printf("TC5: GC erases a fully-invalid block, wear leveling picks B3\n");
    Ftl ftl(tinyConfig());
    buildTc4(ftl);
    ftl.write(7);
    printBlocks(ftl);
    CHECK(ftl.stats.gcRuns == 1);
    CHECK(ftl.stats.migratedPages == 0);              // B0 had nothing valid
    CHECK(ftl.nand().block(0).eraseCount == 1);
    CHECK(ftl.read(7) == 12);                         // B3 page 0, NOT B0 (erase count 1)
    CHECK(ftl.stats.waf() == 1.0);
    CHECK(ftl.checkInvariants());
}

static void tc6_migration() {
    std::printf("TC6: GC migrates a valid page (write amplification)\n");
    Ftl ftl(tinyConfig());
    buildTc4(ftl);
    ftl.write(7);
    for (uint64_t l : {0, 2, 3}) ftl.write(l);
    const uint64_t t0 = ftl.stats.simTimeUs;
    ftl.write(6);
    const uint64_t dt = ftl.stats.simTimeUs - t0;     // measure before any reads below
    printBlocks(ftl);
    CHECK(ftl.stats.gcRuns == 2);
    CHECK(ftl.stats.migratedPages == 1);              // LPN 5 moved out of B2
    CHECK(ftl.read(5) == 0);                          // ...into B0 page 0
    CHECK(ftl.read(6) == 1);                          // host write continues in B0
    CHECK(ftl.nand().block(2).eraseCount == 1);
    CHECK(ftl.stats.hostWrites == 17 && ftl.stats.nandWrites == 18);
    std::printf("  WAF = %.3f, this write took %llu us (normal write: 200 us)\n",
                ftl.stats.waf(), (unsigned long long)dt);
    CHECK(dt == 25 + 200 + 1500 + 200);               // GC read+program+erase, then host program
    CHECK(ftl.checkInvariants());
}

static void tc7_trim_avoids_copy() {
    std::printf("TC7: TRIM before GC -> nothing to migrate\n");
    Ftl ftl(tinyConfig());
    buildTc4(ftl);
    ftl.write(7);
    for (uint64_t l : {0, 2, 3}) ftl.write(l);
    ftl.trim(5);
    ftl.write(6);
    CHECK(ftl.stats.migratedPages == 0);
    CHECK(ftl.stats.waf() == 1.0);
    CHECK(ftl.read(5) == -1);
    CHECK(ftl.checkInvariants());
}

static void stress_random() {
    std::printf("Stress: random writes never run out of space\n");
    for (Config cfg : {tinyConfig(), Config{}}) {
        Ftl ftl(cfg);
        uint64_t x = 12345;                           // tiny LCG, deterministic
        for (int i = 0; i < 300000; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            ftl.write((x >> 33) % ftl.logicalPages());
        }
        uint32_t minE = UINT32_MAX, maxE = 0;
        for (uint32_t b = 0; b < ftl.nand().numBlocks(); ++b) {
            minE = std::min(minE, ftl.nand().block(b).eraseCount);
            maxE = std::max(maxE, ftl.nand().block(b).eraseCount);
        }
        std::printf("  %u blocks: WAF %.2f, GC runs %llu, erase count min/max %u/%u\n",
                    cfg.blocks, ftl.stats.waf(), (unsigned long long)ftl.stats.gcRuns, minE, maxE);
        CHECK(ftl.stats.waf() > 1.0);
        CHECK(ftl.checkInvariants());
    }
}


static void tcS0_nand_slc_mode() {
    std::printf("TC-S0: NAND SLC mode (given, should already pass)\n");
    Nand n(1, 8);
    n.setMode(0, CellMode::SLC);
    CHECK(n.block(0).capacity == 2);
    n.program(0, 1); n.program(1, 2);
    bool rejected = false;
    try { n.program(2, 3); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);                                  // SLC block holds only 2 of its 8 pages
}

static void tcS1_write_lands_in_slc() {
    std::printf("TC-S1: a write lands in SLC and costs 150 us\n");
    Ftl ftl(slcConfig());
    CHECK(ftl.logicalPages() == 36);                  // QLC pages only
    CHECK(ftl.freeSlcBlocks() == 2 && ftl.freeBlocks() == 6);
    CHECK(ftl.nand().block(0).mode == CellMode::SLC && ftl.nand().block(2).mode == CellMode::QLC);
    const uint64_t t0 = ftl.stats.simTimeUs;
    ftl.write(0);
    CHECK(ftl.stats.simTimeUs - t0 == 150);
    CHECK(ftl.read(0) == 0);                          // B0 page 0
    CHECK(ftl.stats.slcWrites == 1 && ftl.stats.qlcWrites == 0);
    CHECK(ftl.freeSlcBlocks() == 1);
    CHECK(ftl.checkInvariants());
}

static void tcS2_cliff() {
    std::printf("TC-S2: SLC full -> direct-to-QLC write costs 2500 us (the cliff)\n");
    Ftl ftl(slcConfig());
    for (uint64_t l = 0; l < 4; ++l) {                // 4 SLC pages: all fast
        const uint64_t t0 = ftl.stats.simTimeUs;
        ftl.write(l);
        CHECK(ftl.stats.simTimeUs - t0 == 150);
    }
    CHECK(ftl.read(3) == 9);                          // B1 page 1: second SLC block, last SLC page
    CHECK(ftl.freeSlcBlocks() == 0 && ftl.fullSlcBlocks() == 2);
    uint64_t t0 = ftl.stats.simTimeUs;
    ftl.write(4);                                     // cache full
    CHECK(ftl.stats.simTimeUs - t0 == 2500);
    CHECK(ftl.read(4) / 8 >= 2);                      // landed in a QLC block
    t0 = ftl.stats.simTimeUs;
    ftl.write(0);                                     // overwrite of cached data also goes to QLC now
    CHECK(ftl.stats.simTimeUs - t0 == 2500);
    CHECK(ftl.nand().block(0).pages[0] == PageState::Invalid);
    CHECK(ftl.stats.slcWrites == 4 && ftl.stats.qlcWrites == 2 && ftl.stats.directQlcWrites == 2);
    printBlocks(ftl);
    CHECK(ftl.checkInvariants());
}

static void tcS3_invariants_both_regions() {
    std::printf("TC-S3: random writes across both regions; GC never touches SLC\n");
    for (Config cfg : {slcConfig(), Config{}}) {
        Ftl ftl(cfg);
        uint64_t x = 777;
        for (int i = 0; i < 100000; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            ftl.write((x >> 33) % ftl.logicalPages());
        }
        uint32_t slcErases = 0;
        for (uint32_t b = 0; b < cfg.slcBlocks; ++b) slcErases += ftl.nand().block(b).eraseCount;
        std::printf("  %u blocks (%u SLC): WAF %.2f, slc %llu, qlc %llu, direct %llu, GC runs %llu\n",
                    cfg.blocks, cfg.slcBlocks, ftl.stats.waf(),
                    (unsigned long long)ftl.stats.slcWrites, (unsigned long long)ftl.stats.qlcWrites,
                    (unsigned long long)ftl.stats.directQlcWrites, (unsigned long long)ftl.stats.gcRuns);
        CHECK(slcErases == 0);                        // no folding yet, so SLC blocks are never erased
        CHECK(ftl.stats.slcWrites == uint64_t(cfg.slcBlocks) * (cfg.pagesPerBlock / 4));
        CHECK(ftl.stats.slcWrites + ftl.stats.qlcWrites == ftl.stats.nandWrites);
        CHECK(ftl.checkInvariants());
    }
}

static void tcS4_fold() {
    std::printf("TC-S4: folding moves only valid pages, oldest SLC block first\n");
    Ftl ftl(slcConfig());
    for (uint64_t l = 0; l < 4; ++l) ftl.write(l);    // SLC: B0 [0 1]  B1 [2 3]
    ftl.write(1);                                      // cache full -> QLC B2 page 0; B0 page 1 invalid
    uint64_t t0 = ftl.stats.simTimeUs;
    ftl.idle(1);                                       // any budget starts one fold, which runs to completion
    const uint64_t dt = ftl.stats.simTimeUs - t0;      // measure before any reads below
    CHECK(dt == 1 * (20 + 2500) + 3000);               // 1 valid page (SLC read + QLC program) + SLC erase
    CHECK(ftl.stats.folds == 1);
    CHECK(ftl.nand().block(0).eraseCount == 1);        // FIFO: B0 (filled first) is folded first
    CHECK(ftl.nand().block(1).eraseCount == 0);
    CHECK(ftl.read(0) == 17);                          // LPN 0 moved to QLC B2 page 1
    CHECK(ftl.stats.absorbedPages == 1);               // old copy of LPN 1 skipped, never written to QLC
    t0 = ftl.stats.simTimeUs;
    ftl.idle(100000);                                  // plenty of time: fold the rest
    CHECK(ftl.stats.simTimeUs - t0 == 100000);         // idle always advances the clock by at least `us`
    CHECK(ftl.stats.folds == 2 && ftl.stats.foldedPages == 3);
    CHECK(ftl.read(2) == 18 && ftl.read(3) == 19 && ftl.read(1) == 16);
    CHECK(ftl.freeSlcBlocks() == 2 && ftl.fullSlcBlocks() == 0);
    CHECK(ftl.stats.hostWrites == 5 && ftl.stats.slcWrites == 4 && ftl.stats.qlcWrites == 4);
    printBlocks(ftl);
    CHECK(ftl.checkInvariants());
}

static void tcS5_absorption() {
    std::printf("TC-S5: a page overwritten while in SLC never reaches QLC\n");
    Ftl ftl(slcConfig());
    ftl.write(0); ftl.write(1);                        // B0 [0 1]
    ftl.write(0); ftl.write(1);                        // hot data: rewritten into B1, B0 is now all invalid
    const uint64_t t0 = ftl.stats.simTimeUs;
    ftl.idle(1);                                       // fold B0
    CHECK(ftl.stats.simTimeUs - t0 == 3000);           // nothing to move: just the SLC erase
    CHECK(ftl.stats.qlcWrites == 0);                   // the first copies were absorbed by the cache
    CHECK(ftl.stats.absorbedPages == 2);
    ftl.idle(100000);                                  // fold B1: only the latest copies go to QLC
    CHECK(ftl.stats.qlcWrites == 2 && ftl.stats.hostWrites == 4);
    CHECK(ftl.stats.qlcPerHost() < 1.0);               // 0.5: half the host writes never cost QLC wear
    CHECK(ftl.checkInvariants());
}

static void tcS6_cache_recovers() {
    std::printf("TC-S6: after idle the cache is empty and the next burst is fast again\n");
    Ftl ftl(slcConfig());
    for (uint64_t l = 0; l < 5; ++l) ftl.write(l);    // 4 fast + 1 cliff
    ftl.idle(100000);                                  // drain both SLC blocks (B1 was the SLC active block)
    for (uint64_t l = 10; l < 14; ++l) {
        const uint64_t t0 = ftl.stats.simTimeUs;
        ftl.write(l);
        CHECK(ftl.stats.simTimeUs - t0 == 150);        // fast again
    }
    CHECK(ftl.freeSlcBlocks() == 0 && ftl.fullSlcBlocks() == 2);  // each SLC block used once, not twice
    const uint64_t t0 = ftl.stats.simTimeUs;
    ftl.write(14);
    CHECK(ftl.stats.simTimeUs - t0 == 2500);           // and the cliff comes back once it is full
    CHECK(ftl.checkInvariants());
}

static void tcS7_bursts_with_idle() {
    std::printf("TC-S7: bursts smaller than the cache + idle gaps never hit the cliff\n");
    for (bool withIdle : {false, true}) {
        Ftl ftl(Config{});                             // 16 SLC blocks x 16 pages = 256 SLC pages
        uint64_t x = 4242;
        for (int burst = 0; burst < 500; ++burst) {
            for (int i = 0; i < 200; ++i) {            // 200 writes per burst < 256-page cache
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                ftl.write((x >> 33) % ftl.logicalPages());
            }
            if (withIdle) ftl.idle(500000);              // 0.5 s idle: enough to fold the burst, including QLC GC it triggers
        }
        std::printf("  idle=%s: direct-to-QLC %llu, folds %llu, absorbed %llu, QLC writes/host write %.2f, WAF %.2f\n",
                    withIdle ? "yes" : "no ", (unsigned long long)ftl.stats.directQlcWrites,
                    (unsigned long long)ftl.stats.folds, (unsigned long long)ftl.stats.absorbedPages,
                    ftl.stats.qlcPerHost(), ftl.stats.waf());
        if (withIdle) CHECK(ftl.stats.directQlcWrites == 0 && ftl.stats.folds > 0);
        else          CHECK(ftl.stats.directQlcWrites > 0 && ftl.stats.folds == 0);
        CHECK(ftl.stats.slcWrites + ftl.stats.qlcWrites == ftl.stats.nandWrites);
        CHECK(ftl.checkInvariants());
    }
}

// ---------------------------------------------------------------------------
// NVMe front end (hour 4.75-5.5)
// ---------------------------------------------------------------------------
static NvmeCompletion submitAndRun(NvmeController& ctrl, NvmeCommand cmd) {
    ctrl.submit(cmd);
    ctrl.process(1);
    NvmeCompletion c{0xFFFF, 0xFFFF, 0};
    ctrl.popCompletion(c);
    return c;
}

static void tcN1_multi_block_write() {
    std::printf("TC-N1: Write with nlb=3 writes 4 pages; completion echoes the cid\n");
    Ftl ftl(slcConfig());
    NvmeController ctrl(ftl);
    ctrl.submit({7, NvmeOpcode::Write, 10, 3});        // LBAs 10..13 (nlb is 0-based)
    CHECK(ctrl.sqDepth() == 1 && ctrl.cqDepth() == 0); // queued, not run yet
    CHECK(ctrl.process(8) == 1);                       // doorbell: device runs what's there
    CHECK(ctrl.sqDepth() == 0 && ctrl.cqDepth() == 1);
    NvmeCompletion c{};
    CHECK(ctrl.popCompletion(c));
    CHECK(c.cid == 7 && c.status == NvmeStatus::Success);
    CHECK(c.latencyUs == 4 * 150);                     // 4 pages, all into SLC
    CHECK(ftl.stats.hostWrites == 4);
    CHECK(ftl.read(10) >= 0 && ftl.read(13) >= 0 && ftl.read(14) == -1);
    CHECK(!ctrl.popCompletion(c));                     // CQ is empty now

    for (uint16_t id = 1; id <= 3; ++id) ctrl.submit({id, NvmeOpcode::Read, id, 0});
    CHECK(ctrl.process(2) == 2);                       // maxCommands is respected
    CHECK(ctrl.sqDepth() == 1 && ctrl.cqDepth() == 2);
    CHECK(ctrl.process(8) == 1);
    for (uint16_t id = 1; id <= 3; ++id) { CHECK(ctrl.popCompletion(c)); CHECK(c.cid == id); }  // FIFO order
}

static void tcN2_errors_are_statuses() {
    std::printf("TC-N2: bad commands return an error status, change nothing, never throw\n");
    Ftl ftl(slcConfig());                              // 36 logical pages: LBAs 0..35
    NvmeController ctrl(ftl);
    bool threw = false;
    NvmeCompletion c{};
    try {
        c = submitAndRun(ctrl, {1, NvmeOpcode::Write, 34, 3});          // 34..37: runs off the end
        CHECK(c.cid == 1 && c.status == NvmeStatus::LbaOutOfRange);
        CHECK(c.latencyUs == 0);
        CHECK(ftl.stats.hostWrites == 0);              // all-or-nothing: 34 and 35 NOT written
        c = submitAndRun(ctrl, {2, NvmeOpcode::Read, 1000000000000ULL, 0});  // way out of range
        CHECK(c.status == NvmeStatus::LbaOutOfRange);
        c = submitAndRun(ctrl, {3, NvmeOpcode(0x7F), 0, 0});            // opcode we don't support
        CHECK(c.cid == 3 && c.status == NvmeStatus::InvalidOpcode);
        c = submitAndRun(ctrl, {4, NvmeOpcode::Write, 35, 0});          // last valid LBA is fine
        CHECK(c.status == NvmeStatus::Success);
    } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(ftl.stats.hostWrites == 1 && ftl.read(34) == -1);
    CHECK(ftl.checkInvariants());
}

static void tcN3_trim_then_read() {
    std::printf("TC-N3: Dataset Management (deallocate) then Read: the LBA reads as unmapped\n");
    Ftl ftl(slcConfig());
    NvmeController ctrl(ftl);
    CHECK(submitAndRun(ctrl, {1, NvmeOpcode::Write, 5, 1}).status == NvmeStatus::Success);  // LBAs 5, 6
    const NvmeCompletion r1 = submitAndRun(ctrl, {2, NvmeOpcode::Read, 5, 0});
    CHECK(r1.status == NvmeStatus::Success && r1.latencyUs == 80);      // mapped read hits NAND
    CHECK(submitAndRun(ctrl, {3, NvmeOpcode::DatasetMgmt, 5, 0}).status == NvmeStatus::Success);
    const NvmeCompletion r2 = submitAndRun(ctrl, {4, NvmeOpcode::Read, 5, 0});
    CHECK(r2.status == NvmeStatus::Success && r2.latencyUs == 0);       // unmapped: no NAND access
    CHECK(ftl.read(5) == -1 && ftl.read(6) >= 0);                       // only LBA 5 was deallocated
    CHECK(ftl.checkInvariants());
}

static void tcN4_gc_latency_spike() {
    std::printf("TC-N4: the command that triggers GC has a much higher completion latency\n");
    Ftl ftl(tinyConfig());                             // same drive and sequence as TC6
    NvmeController ctrl(ftl);
    const uint64_t seq[] = {0,1,2,3, 1, 4,5,6, 0,2,3,5, 7, 0,2,3, 6};
    uint16_t cid = 0;
    for (uint64_t lba : seq) ctrl.submit({cid++, NvmeOpcode::Write, lba, 0});
    CHECK(ctrl.process(100) == 17);
    NvmeCompletion c{};
    uint64_t maxLat = 0; uint16_t slowCid = 0;
    while (ctrl.popCompletion(c)) {
        CHECK(c.status == NvmeStatus::Success);
        if (c.latencyUs > maxLat) { maxLat = c.latencyUs; slowCid = c.cid; }
        if (c.cid == 15) CHECK(c.latencyUs == 200);    // the neighbor before it: a normal write
    }
    std::printf("  slowest completion: cid %u, %llu us (normal: 200 us)\n", slowCid, (unsigned long long)maxLat);
    CHECK(slowCid == 16 && maxLat == 1925);            // GC: read + program + erase, then the write
    CHECK(ftl.checkInvariants());
}

// ---------------------------------------------------------------------------
// Workloads (hour 4.75-5.5, second half)
// ---------------------------------------------------------------------------
static void tcW1_patterns() {
    std::printf("TC-W1: sequential wraps, uniform stays in range, hot/cold hits the hot set ~80%%\n");
    WorkloadConfig wc;
    wc.pattern = Pattern::Sequential;
    Workload seq(wc, 5);
    const uint64_t expect[] = {0, 1, 2, 3, 4, 0, 1};
    for (uint64_t e : expect) CHECK(seq.nextLba() == e);

    wc.pattern = Pattern::UniformRandom;
    Workload uni(wc, 100);
    bool inRange = true, sawLow = false, sawHigh = false;
    for (int i = 0; i < 10000; ++i) {
        const uint64_t l = uni.nextLba();
        inRange = inRange && l < 100; sawLow = sawLow || l < 10; sawHigh = sawHigh || l >= 90;
    }
    CHECK(inRange && sawLow && sawHigh);                // spread over the whole drive

    wc.pattern = Pattern::HotCold; wc.hotFraction = 0.2; wc.hotProbability = 0.8;
    Workload hc(wc, 1000);                             // hot set = LBAs 0..199
    int hot = 0; bool hcRange = true, sawCold = false;
    for (int i = 0; i < 100000; ++i) {
        const uint64_t l = hc.nextLba();
        hcRange = hcRange && l < 1000;
        if (l < 200) ++hot; else sawCold = true;
    }
    std::printf("  hot/cold: %.1f%% of writes went to the hot 20%%\n", hot / 1000.0);
    CHECK(hcRange && sawCold);
    CHECK(hot > 78000 && hot < 82000);                 // about 80%
}

static void tcW2_experiment_loop() {
    std::printf("TC-W2: runExperiment measures every command\n");
    Config d = slcConfig();
    WorkloadConfig wc;
    wc.pattern = Pattern::Sequential; wc.bursts = 3; wc.burstCommands = 4; wc.precondition = false;
    ExperimentResult r = runExperiment(d, wc);         // 3 bursts x 4 writes, no idle
    CHECK(r.hostWrites == 12 && r.latenciesUs.size() == 12);
    if (r.latenciesUs.size() == 12) {                  // only index if the loop recorded everything
        CHECK(r.latenciesUs[0] == 150 && r.latenciesUs[3] == 150);   // first 4 fill the 4-page SLC cache
        CHECK(r.latenciesUs[4] == 2500);               // 5th write: the cliff
    }
    CHECK(r.slowCommands == 8 && r.maxLatencyUs == 2500);
    wc.idleUs = 1000000;                               // now idle 1 s after every burst
    r = runExperiment(d, wc);
    CHECK(r.slowCommands == 0 && r.folds == 6);        // 2 SLC blocks folded after each of the 3 bursts
    CHECK(r.avgLatencyUs == 150);
}

// ---------------------------------------------------------------------------
// Step 1: geometry + parallelism
// ---------------------------------------------------------------------------
// 2 channels x 2 dies = 4 dies, 16 blocks x 4 pages (4 blocks per die), no SLC
static Config geoConfig(uint32_t channels, uint32_t diesPerChannel) {
    Config c;
    c.blocks = 16; c.pagesPerBlock = 4; c.opRatio = 0.25; c.slcBlocks = 0;
    c.channels = channels; c.diesPerChannel = diesPerChannel; c.planesPerDie = 2;
    return c;
}

static void tcG1_geometry_decode() {
    std::printf("TC-G1: blocks interleave across dies, dies across channels\n");
    Ftl ftl(geoConfig(2, 2));
    CHECK(ftl.numDies() == 4);
    CHECK(ftl.dieOf(0) == 0 && ftl.dieOf(1) == 1 && ftl.dieOf(5) == 1 && ftl.dieOf(15) == 3);
    CHECK(ftl.channelOf(0) == 0 && ftl.channelOf(1) == 1 && ftl.channelOf(2) == 0 && ftl.channelOf(3) == 1);
    const PhysAddr a = ftl.decode(6 * 4 + 2);          // block 6, page 2
    CHECK(a.block == 6 && a.page == 2);
    CHECK(a.die == 2 && a.channel == 0);
    CHECK(a.plane == 1);                               // 2nd block on die 2 -> plane 1
    bool rejected = false;
    Config bad = geoConfig(2, 2); bad.blocks = 18;     // 18 blocks don't split evenly over 4 dies
    try { Ftl x(bad); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

static void tcG2_slc_spread() {
    std::printf("TC-G2: the SLC cache is spread over every die\n");
    Config c = geoConfig(2, 2);
    c.blocks = 32; c.pagesPerBlock = 8; c.slcBlocks = 4;
    Ftl ftl(c);
    for (uint32_t b = 0; b < 4; ++b) {
        CHECK(ftl.nand().block(b).mode == CellMode::SLC);
        CHECK(ftl.dieOf(b) == b);                      // SLC blocks 0..3 sit on dies 0..3
    }
    for (uint64_t l = 0; l < 4; ++l) ftl.write(l);     // 4 writes: one per die, all SLC
    for (uint64_t l = 0; l < 4; ++l) CHECK(ftl.decode(uint64_t(ftl.read(l))).die == l);
    CHECK(ftl.stats.slcWrites == 4);
}

static void tcG3_striping() {
    std::printf("TC-G3: consecutive writes stripe round-robin across dies\n");
    Ftl ftl(geoConfig(2, 2));
    for (uint64_t l = 0; l < 8; ++l) ftl.write(l);
    for (uint64_t l = 0; l < 8; ++l) CHECK(ftl.decode(uint64_t(ftl.read(l))).die == l % 4);
    for (uint32_t d = 0; d < 4; ++d) CHECK(ftl.activeBlock(d) >= 0 && ftl.dieOf(uint32_t(ftl.activeBlock(d))) == d);
    CHECK(ftl.checkInvariants());
}

static void tcG4_per_die_gc() {
    std::printf("TC-G4: GC and wear leveling stay inside each die\n");
    Config c; c.slcBlocks = 16; c.channels = 2; c.diesPerChannel = 4;   // 8 dies, 32 blocks each
    Ftl ftl(c);
    std::vector<int64_t> dieAtWrite(ftl.logicalPages(), -1);   // which die each LBA was last written to
    uint64_t x = 99, wrongDie = 0;
    for (int i = 0; i < 200000; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint64_t l = (x >> 33) % ftl.logicalPages();
        ftl.write(l);
        dieAtWrite[l] = ftl.decode(uint64_t(ftl.read(l))).die;
        if (dieAtWrite[l] != i % 8) ++wrongDie;                // striping: write i goes to die i % 8
        if (i % 5000 == 4999) ftl.idle(10000000);
    }
    CHECK(wrongDie == 0);
    CHECK(ftl.checkInvariants());
    uint64_t moved = 0;                                        // GC and folds must never move data to another die
    for (uint64_t l = 0; l < ftl.logicalPages(); ++l)
        if (dieAtWrite[l] >= 0 && int64_t(ftl.decode(uint64_t(ftl.read(l))).die) != dieAtWrite[l]) ++moved;
    CHECK(moved == 0);
    for (uint32_t d = 0; d < ftl.numDies(); ++d) {
        CHECK(ftl.activeBlock(d) < 0 || ftl.dieOf(uint32_t(ftl.activeBlock(d))) == d);
        uint32_t erases = 0;
        for (uint32_t b = d; b < c.blocks; b += ftl.numDies()) erases += ftl.nand().block(b).eraseCount;
        CHECK(erases > 0);                             // every die did its share of GC
    }
    std::printf("  8 dies: WAF %.2f, GC runs %llu, folds %llu\n", ftl.stats.waf(),
                (unsigned long long)ftl.stats.gcRuns, (unsigned long long)ftl.stats.folds);
}

static uint64_t makespanFor(uint32_t channels, uint32_t diesPerChannel, uint32_t xferUs) {
    Config c; c.slcBlocks = 0; c.channels = channels; c.diesPerChannel = diesPerChannel;
    c.timing = {80, 2500, 3000}; c.channelXferUs = xferUs;
    Ftl ftl(c);
    for (uint64_t l = 0; l < 1024; ++l) ftl.write(l);  // 1024 sequential writes, fresh drive (no GC)
    return ftl.makespanUs();
}

static void tcG5_parallel_speedup() {
    std::printf("TC-G5: more dies -> more parallel writes, until a channel saturates\n");
    const uint64_t t1 = makespanFor(1, 1, 10), t4 = makespanFor(2, 2, 10), t8 = makespanFor(2, 4, 10);
    std::printf("  1 die %llu us, 4 dies %llu us (%.1fx), 8 dies %llu us (%.1fx)\n",
                (unsigned long long)t1, (unsigned long long)t4, double(t1) / t4,
                (unsigned long long)t8, double(t1) / t8);
    CHECK(t1 == 1024 * (10 + 2500));                   // one die does everything, one after another
    CHECK(t4 * 4 == t1 && t8 * 8 == t1);               // perfect striping: each die does 1/N of the work
    // a slow channel shared by 8 dies becomes the bottleneck
    Config c; c.slcBlocks = 0; c.channels = 1; c.diesPerChannel = 8; c.timing = {80, 2500, 3000}; c.channelXferUs = 500;
    Ftl ftl(c);
    for (uint64_t l = 0; l < 1024; ++l) ftl.write(l);
    std::printf("  1 channel x 8 dies, 500 us transfers: channel busy %llu us, busiest die %llu us\n",
                (unsigned long long)ftl.channelBusyUs(0), (unsigned long long)ftl.dieBusyUs(0));
    CHECK(ftl.makespanUs() == ftl.channelBusyUs(0));   // channel-bound, not die-bound
    CHECK(ftl.channelBusyUs(0) == 1024 * 500);
}

// ---------------------------------------------------------------------------
// Step 2: event clock + queue depth
// ---------------------------------------------------------------------------
static Config qdConfig(uint32_t channels, uint32_t diesPerChannel) {
    Config c; c.slcBlocks = 0; c.channels = channels; c.diesPerChannel = diesPerChannel;
    c.timing = {80, 2500, 3000}; c.channelXferUs = 10;
    return c;
}
static WorkloadConfig seqFresh() { WorkloadConfig w; w.pattern = Pattern::Sequential; w.precondition = false; return w; }

static void tcE1_die_and_channel_waits() {
    std::printf("TC-E1: a command waits for a busy channel and a busy die\n");
    Config c = qdConfig(1, 2); c.channelXferUs = 100;        // 2 dies share ONE channel
    Ftl ftl(c);
    ftl.setNow(0); ftl.write(0);                              // die 0: bus 0-100, program 100-2600
    CHECK(ftl.lastDoneUs() == 2600);
    ftl.setNow(0); ftl.write(1);                              // die 1: waits for the bus (100-200), program 200-2700
    CHECK(ftl.lastDoneUs() == 2700);
    ftl.setNow(0); ftl.write(2);                              // die 0 again: bus 200-300, but die 0 is busy until 2600
    CHECK(ftl.lastDoneUs() == 2600 + 2500);
    ftl.setNow(10000); ftl.read(0);                           // read: die reads 10000-10080, then bus 10080-10180
    CHECK(ftl.lastDoneUs() == 10180);
    ftl.setNow(20000); ftl.read(500);                         // never written: no NAND access
    CHECK(ftl.lastDoneUs() == 20000);
}

static void tcE2_percentile() {
    std::printf("TC-E2: percentiles (nearest rank)\n");
    std::vector<uint64_t> v;
    for (uint64_t i = 100; i >= 1; --i) v.push_back(i);       // 100 .. 1, unsorted on purpose
    CHECK(percentile(v, 50) == 50);
    CHECK(percentile(v, 99) == 99);
    CHECK(percentile(v, 100) == 100);
    CHECK(percentile({7}, 99) == 7);
    CHECK(percentile({}, 50) == 0);
}

static void tcE3_qd1_no_parallelism() {
    std::printf("TC-E3: at queue depth 1, 4 dies are no faster than 1\n");
    const QdResult one = runQueueDepth(qdConfig(1, 1), seqFresh(), 1, 2000);
    const QdResult four = runQueueDepth(qdConfig(2, 2), seqFresh(), 1, 2000);
    std::printf("  QD1: 1 die %.0f IOPS, 4 dies %.0f IOPS, latency %.0f us\n", one.iops, four.iops, four.avgLatencyUs);
    CHECK(one.p50Us == 2510 && one.maxUs == 2510);           // every write: 10 us transfer + 2500 us program
    CHECK(four.iops == one.iops);                             // only one command in flight -> only one die busy
}

static void tcE4_qd_fills_dies() {
    std::printf("TC-E4: queue depth = number of dies -> full speed, same latency\n");
    const QdResult q1 = runQueueDepth(qdConfig(2, 2), seqFresh(), 1, 2000);
    const QdResult q4 = runQueueDepth(qdConfig(2, 2), seqFresh(), 4, 2000);
    const QdResult q16 = runQueueDepth(qdConfig(2, 2), seqFresh(), 16, 2000);
    std::printf("  4 dies: QD1 %.0f, QD4 %.0f, QD16 %.0f IOPS; p50 %llu / %llu / %llu us\n", q1.iops, q4.iops, q16.iops,
                (unsigned long long)q1.p50Us, (unsigned long long)q4.p50Us, (unsigned long long)q16.p50Us);
    CHECK(q4.iops > 3.9 * q1.iops);                           // 4 dies busy at once
    CHECK(q4.p50Us == 2510);                                  // ...and nobody waited
    CHECK(q16.iops < 1.01 * q4.iops);                         // more QD can't beat 4 busy dies
    CHECK(q16.p50Us == 4 * 2500);                             // the extra commands just queue (each waits 3 programs)
    for (const QdResult* r : {&q1, &q4, &q16}) {              // Little's law: latency x throughput = queue depth
        const double inFlight = r->avgLatencyUs * r->iops / 1e6;
        const double qd = r == &q1 ? 1 : r == &q4 ? 4 : 16;
        CHECK(inFlight > 0.98 * qd && inFlight < 1.02 * qd);
    }
}

static void tcE5_gc_tail_latency() {
    std::printf("TC-E5: GC on a die makes the host commands behind it wait (tail latency)\n");
    WorkloadConfig wl; wl.pattern = Pattern::UniformRandom; wl.precondition = true;
    const QdResult r = runQueueDepth(qdConfig(2, 2), wl, 1, 20000);
    std::printf("  random writes, full drive: p50 %llu us, p99 %llu us, max %llu us, %llu GC runs\n",
                (unsigned long long)r.p50Us, (unsigned long long)r.p99Us, (unsigned long long)r.maxUs,
                (unsigned long long)r.gcRuns);
    CHECK(r.gcRuns > 0);
    CHECK(r.p50Us == 2510);                                   // most writes never meet a GC
    CHECK(r.p99Us > 20 * r.p50Us);                            // the unlucky 1% wait for a whole GC
    CHECK(r.maxUs >= 3000);                                   // a GC always includes an erase
}

static void run(void (*test)()) {
    try { test(); }
    catch (const std::exception& e) { std::printf("  FAIL: exception: %s\n", e.what()); ++failures; }
}

int main() {
    run(tc1_first_writes);
    run(tc2_overwrite);
    run(tc3_reads);
    run(trim_and_edges);
    run(tc4_setup);
    run(tc5_free_victim_and_wear_leveling);
    run(tc6_migration);
    run(tc7_trim_avoids_copy);
    run(stress_random);
    run(tcS0_nand_slc_mode);
    run(tcS1_write_lands_in_slc);
    run(tcS2_cliff);
    run(tcS3_invariants_both_regions);
    run(tcS4_fold);
    run(tcS5_absorption);
    run(tcS6_cache_recovers);
    run(tcS7_bursts_with_idle);
    run(tcN1_multi_block_write);
    run(tcN2_errors_are_statuses);
    run(tcN3_trim_then_read);
    run(tcN4_gc_latency_spike);
    run(tcW1_patterns);
    run(tcW2_experiment_loop);
    run(tcG1_geometry_decode);
    run(tcG2_slc_spread);
    run(tcG3_striping);
    run(tcG4_per_die_gc);
    run(tcG5_parallel_speedup);
    run(tcE1_die_and_channel_waits);
    run(tcE2_percentile);
    run(tcE3_qd1_no_parallelism);
    run(tcE4_qd_fills_dies);
    run(tcE5_gc_tail_latency);
    std::printf(failures ? "\n%d CHECK(s) FAILED\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
