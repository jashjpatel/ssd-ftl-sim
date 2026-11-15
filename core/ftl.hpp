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

    // ---- geometry (Step 1) ----
    uint32_t   channels       = 1;           // NAND channels (independent buses)
    uint32_t   diesPerChannel = 1;           // dies sharing each channel
    uint32_t   planesPerDie   = 1;           // used for addressing only (multi-plane ops come later)
    uint32_t   channelXferUs  = 0;           // time to move one page over a channel (0 = ignore)
};

// Where a physical page really is. Blocks are interleaved across dies (block b lives on die
// b % numDies) and dies across channels (die d sits on channel d % channels), so consecutive
// blocks, including the SLC cache, are spread over every die and channel.
struct PhysAddr { uint32_t channel, die, plane, block, page; };

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
    int64_t activeBlock(uint32_t die = 0) const { return activeBlock_[die]; }         // QLC active block of a die (-1 = none)
    int64_t activeSlcBlock(uint32_t die = 0) const { return activeSlcBlock_[die]; }   // SLC active block of a die (-1 = none)

    // ---- geometry (Step 1) ----
    uint32_t numDies() const { return cfg_.channels * cfg_.diesPerChannel; }
    uint32_t dieOf(uint32_t b) const;          // which die block b is on
    uint32_t channelOf(uint32_t die) const;    // which channel a die is on
    PhysAddr decode(uint64_t ppn) const;       // flat PPN -> channel/die/plane/block/page
    uint64_t dieBusyUs(uint32_t die) const { return dieBusyUs_[die]; }
    uint64_t channelBusyUs(uint32_t ch) const { return channelBusyUs_[ch]; }
    uint64_t makespanUs() const;               // time if dies and channels work in parallel = the busiest one

    // ---- event clock (Step 2) ----
    // The host says WHEN each command arrives (setNow), the FTL schedules it on its die and channel,
    // and lastDoneUs() says when it finished. A die or channel that is still busy makes it wait.
    void     setNow(uint64_t us) { nowUs_ = us; }
    uint64_t lastDoneUs() const { return lastDoneUs_; }
    uint64_t dieFreeAtUs(uint32_t die) const { return dieFreeAtUs_[die]; }
    uint64_t channelFreeAtUs(uint32_t ch) const { return channelFreeAtUs_[ch]; }
    int64_t lpnAt(uint64_t ppn) const { return p2l[ppn]; }       // PPN -> LPN (-1 = none), for the GUI
    int64_t ppnOf(uint64_t lpn) const { return l2p[lpn]; }       // LPN -> PPN (-1 = unmapped), no side effects

    void    write(uint64_t lpn);
    int64_t read(uint64_t lpn);   // returns PPN, or -1 if unmapped
    void    trim(uint64_t lpn);
    void    idle(uint64_t us);    // background time: fold full SLC blocks into QLC
    bool    checkInvariants() const;

    Stats stats;

private:
    uint32_t allocateBlock(uint32_t die);   // runs GC on that die if needed, returns its block to write next
    uint32_t takeFreeBlock(uint32_t die);   // wear leveling: lowest-eraseCount free block ON THAT DIE
    void     runGc(uint32_t die);           // reclaim one block on that die
    void     foldOneBlock();      // oldest full SLC block -> valid pages to QLC -> erase it
    uint64_t tick_ = 0;
    Config cfg_;
    Nand nand_;
    std::vector<int64_t> l2p;  // LPN -> PPN (-1 = unmapped)
    std::vector<int64_t> p2l;  // PPN -> LPN (-1 = none)
    std::deque<uint32_t> freeBlocks_;
    std::vector<int64_t> activeBlock_;        // QLC active block, one per die (-1 = none)
    std::vector<int64_t> activeSlcBlock_;     // SLC active block, one per die (-1 = none)
    uint32_t nextDie_ = 0;                    // round-robin striping: die for the next host write
    std::vector<uint64_t> dieBusyUs_;         // total time each die has spent working
    std::vector<uint64_t> channelBusyUs_;     // total time each channel has spent transferring
    uint64_t nowUs_ = 0;                      // arrival time of the current host command
    uint64_t lastDoneUs_ = 0;                 // completion time of the last host command
    std::vector<uint64_t> dieFreeAtUs_;       // when each die finishes everything it has been given
    std::vector<uint64_t> channelFreeAtUs_;   // when each channel's bus is free again
    std::deque<uint32_t> freeSlcBlocks_;      // erased SLC blocks
    std::deque<uint32_t> fullSlcBlocks_;      // full SLC blocks, oldest first (folded in hour 4)
};
