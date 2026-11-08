#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include "nand.hpp"

enum class GcPolicy { Greedy, CostBenefit };

struct Config {
    uint32_t blocks        = 256;
    uint32_t pagesPerBlock = 64;
    double   opRatio       = 0.07;  // over-provisioning
    uint32_t gcThreshold   = 1;     // run GC when free blocks <= this
    GcPolicy policy        = GcPolicy::Greedy;
    NandTiming timing;                       // QLC region timing (also used by GC)
    uint32_t   slcBlocks   = 16;             // first N blocks run in SLC mode (the write cache)
    NandTiming slcTiming{20, 150, 3000};     // SLC region timing
    bool       foldOnIdle  = true;           // move SLC data to QLC during idle time
};

struct Stats {
    uint64_t hostWrites = 0, nandWrites = 0, hostReads = 0;
    uint64_t gcRuns = 0, migratedPages = 0, erases = 0;
    uint64_t simTimeUs = 0;
    uint64_t slcWrites = 0, qlcWrites = 0, directQlcWrites = 0;  // direct = host write that skipped SLC
    uint64_t folds = 0, foldedPages = 0, absorbedPages = 0;      // absorbed = invalid when folded (never hit QLC)
    double waf() const { return hostWrites ? double(nandWrites) / double(hostWrites) : 1.0; }
    double qlcPerHost() const { return hostWrites ? double(qlcWrites) / double(hostWrites) : 0.0; }  // QLC wear per host write
};

class Ftl {
public:
    explicit Ftl(const Config& cfg);

    uint64_t logicalPages() const { return l2p.size(); }
    const Nand& nand() const { return nand_; }
    const Config& config() const { return cfg_; }
    size_t freeBlocks() const { return freeBlocks_.size(); }        // QLC free pool
    size_t freeSlcBlocks() const { return freeSlcBlocks_.size(); }
    size_t fullSlcBlocks() const { return fullSlcBlocks_.size(); }
    int64_t activeBlock() const { return activeBlock_; }         // QLC active block (-1 = none), for the GUI
    int64_t activeSlcBlock() const { return activeSlcBlock_; }   // SLC active block (-1 = none), for the GUI
    int64_t lpnAt(uint64_t ppn) const { return p2l[ppn]; }       // PPN -> LPN (-1 = none), for the GUI
    int64_t ppnOf(uint64_t lpn) const { return l2p[lpn]; }       // LPN -> PPN (-1 = unmapped), no side effects

    void    write(uint64_t lpn);
    int64_t read(uint64_t lpn);   // returns PPN, or -1 if unmapped
    void    trim(uint64_t lpn);
    void    idle(uint64_t us);    // background time: fold full SLC blocks into QLC
    bool    checkInvariants() const;

    Stats stats;

private:
    uint32_t allocateBlock();     // runs GC if needed, returns the block to write next
    uint32_t takeFreeBlock();     // wear leveling: lowest-eraseCount free block
    void     runGc();             // reclaim one block
    void     foldOneBlock();      // oldest full SLC block -> valid pages to QLC -> erase it
    uint64_t tick_ = 0;
    Config cfg_;
    Nand nand_;
    std::vector<int64_t> l2p;  // LPN -> PPN (-1 = unmapped)
    std::vector<int64_t> p2l;  // PPN -> LPN (-1 = none)
    std::deque<uint32_t> freeBlocks_;
    int64_t activeBlock_ = -1;                // QLC active block
    int64_t activeSlcBlock_ = -1;             // SLC active block
    std::deque<uint32_t> freeSlcBlocks_;      // erased SLC blocks
    std::deque<uint32_t> fullSlcBlocks_;      // full SLC blocks, oldest first (folded in hour 4)
};
