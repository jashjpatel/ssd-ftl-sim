#pragma once
#include <cstdint>
#include <vector>

enum class PageState : uint8_t { Free, Valid, Invalid };
enum class CellMode  : uint8_t { SLC, QLC };   // SLC = 1 bit/cell (fast), QLC = 4 bits/cell (dense)

struct Block {
    std::vector<PageState> pages;
    uint32_t writePtr      = 0;  // next page to program (pages are written in order)
    uint32_t validCount    = 0;
    uint32_t eraseCount    = 0;
    uint64_t lastWriteTick = 0;  // used by Cost-Benefit GC
    CellMode mode          = CellMode::QLC;
    uint32_t capacity      = 0;  // usable pages: pagesPerBlock/4 in SLC, pagesPerBlock in QLC
};

struct NandTiming {
    uint32_t readUs    = 25;
    uint32_t programUs = 200;
    uint32_t eraseUs   = 1500;
};

// Raw flash array: enforces the NAND rules, knows nothing about LPNs.
class Nand {
public:
    Nand(uint32_t blocks, uint32_t pagesPerBlock);

    uint32_t numBlocks() const { return static_cast<uint32_t>(blocks_.size()); }
    uint32_t pagesPerBlock() const { return pagesPerBlock_; }
    const Block& block(uint32_t b) const { return blocks_[b]; }

    void program(uint64_t ppn, uint64_t tick);
    void invalidate(uint64_t ppn);
    void erase(uint32_t b);
    void setMode(uint32_t b, CellMode m);   // only allowed on an empty block

private:
    uint32_t pagesPerBlock_;
    std::vector<Block> blocks_;
};