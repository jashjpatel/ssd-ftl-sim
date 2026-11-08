#include "nand.hpp"
#include <stdexcept>
#include <string>

// NAND rules are enforced with exceptions (not assert) so they stay on in Release builds.
static void require(bool cond, const char* msg, uint64_t where) {
    if (!cond) throw std::logic_error(std::string("NAND rule violated: ") + msg + " @ " + std::to_string(where));
}

Nand::Nand(uint32_t blocks, uint32_t pagesPerBlock)
    : pagesPerBlock_(pagesPerBlock), blocks_(blocks) {
    for (auto& b : blocks_) {
        b.pages.assign(pagesPerBlock, PageState::Free);
        b.capacity = pagesPerBlock;   // QLC by default
    }
}

void Nand::program(uint64_t ppn, uint64_t tick) {
    const uint64_t b = ppn / pagesPerBlock_, p = ppn % pagesPerBlock_;
    require(b < blocks_.size(), "ppn out of range", ppn);
    Block& blk = blocks_[b];
    require(blk.pages[p] == PageState::Free, "program on non-free page", ppn);
    require(p == blk.writePtr, "out-of-order program", ppn);
    require(p < blk.capacity, "program past block capacity", ppn);
    blk.pages[p] = PageState::Valid;
    ++blk.writePtr;
    ++blk.validCount;
    blk.lastWriteTick = tick;
}

void Nand::invalidate(uint64_t ppn) {
    const uint64_t b = ppn / pagesPerBlock_, p = ppn % pagesPerBlock_;
    require(b < blocks_.size(), "ppn out of range", ppn);
    Block& blk = blocks_[b];
    require(blk.pages[p] == PageState::Valid, "invalidate on non-valid page", ppn);
    blk.pages[p] = PageState::Invalid;
    --blk.validCount;
}

void Nand::erase(uint32_t b) {
    require(b < blocks_.size(), "block out of range", b);
    Block& blk = blocks_[b];
    blk.pages.assign(pagesPerBlock_, PageState::Free);
    blk.writePtr = 0;
    blk.validCount = 0;
    ++blk.eraseCount;
}

void Nand::setMode(uint32_t b, CellMode m) {
    require(b < blocks_.size(), "block out of range", b);
    Block& blk = blocks_[b];
    require(blk.writePtr == 0, "setMode on non-empty block", b);
    blk.mode = m;
    blk.capacity = (m == CellMode::SLC) ? pagesPerBlock_ / 4 : pagesPerBlock_;
}