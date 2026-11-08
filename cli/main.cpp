#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <climits>
#include "ftl.hpp"
#include "nvme.hpp"

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
    std::printf(failures ? "\n%d CHECK(s) FAILED\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}