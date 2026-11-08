#include "ftl.hpp"
#include <stdexcept>

Ftl::Ftl(const Config& cfg)
    : cfg_(cfg), nand_(cfg.blocks, cfg.pagesPerBlock) {
    // the drive needs at least one QLC block to hold data
    if (cfg.slcBlocks >= cfg.blocks) throw std::invalid_argument("slcBlocks must leave QLC blocks");

    const uint64_t physPages = uint64_t(cfg.blocks) * cfg.pagesPerBlock;
    // get the number of pages for qlc
    const uint64_t qlcPages  = uint64_t(cfg.blocks - cfg.slcBlocks) * cfg.pagesPerBlock;
    // logical capacity comes from QLC only; the SLC cache is temporary space, not extra storage
    l2p.assign(uint64_t(qlcPages * (1.0 - cfg.opRatio)), -1);
    // the reverse map still covers every physical page, SLC included
    p2l.assign(physPages, -1);

    for (uint32_t b = 0; b < cfg.blocks; ++b) {
        // the first cfg.slcBlocks becomes SLC cache
        if (b < cfg.slcBlocks) {
            // switch block b to SLC mode (sets mode AND capacity = pagesPerBlock / 4)
            nand_.setMode(b, CellMode::SLC);
            // put b in the SLC free pool
            freeSlcBlocks_.push_back(b);
        } else {
            freeBlocks_.push_back(b);   // QLC free pool, same as before
        }
    }
}

uint32_t Ftl::allocateBlock() {
    // detemine if gc already opened a new block and it is able to be writed to
    if (activeBlock_ >= 0 && freeBlocks_.size() <= cfg_.gcThreshold) {
        runGc();
        // sometimes gc moves pages before erasing sometimes not
        if (nand_.block(uint32_t(activeBlock_)).writePtr < nand_.block(uint32_t(activeBlock_)).capacity){
            return uint32_t(activeBlock_);
        }
    }
    return takeFreeBlock();
}

uint32_t Ftl::takeFreeBlock() {
    if (freeBlocks_.empty()) throw std::runtime_error("out of free blocks");

    // TODO 1 (wear leveling):
    //   Find the block in freeBlocks_ with the LOWEST nand_.block(b).eraseCount.
    //   On a tie keep the first one you see. Remove it from the deque and return it.
    //   Hint: loop with an index, then freeBlocks_.erase(freeBlocks_.begin() + bestIdx).
    size_t bestIdx = 0;
    // freeBlocks is the double ended queue that allows for fast insertion and deletion at the beginning and end...
    for(size_t i = 1; i < freeBlocks_.size(); ++i){
        // if the block at this new index has less eraseCount than the current lowest this one becomes the lowest
        if(nand_.block(freeBlocks_[i]).eraseCount < nand_.block(freeBlocks_[bestIdx]).eraseCount){
            bestIdx = i;
        }
    }
    const uint32_t b = freeBlocks_[bestIdx];
    // move to the index found and erase the block there
    freeBlocks_.erase(freeBlocks_.begin() + bestIdx);
    return b;
}

void Ftl::runGc() {
    // TODO 2 (victim selection, Greedy):
    //   Loop over all blocks. A candidate must be FULL (writePtr == cfg_.pagesPerBlock)
    //   and must NOT be activeBlock_. Pick the one with the lowest validCount.
    //   If there is no candidate, return.
    int64_t victim = -1;
    for(uint32_t b = 0; b < nand_.numBlocks(); ++b){
        if(uint32_t(b) == activeBlock_){
            continue;
        }
        // if the number of pages in the block has currently is the max move on
        // GC is not used for SLC
        if(nand_.block(b).mode == CellMode::SLC){
            continue;
        }
        // blocks that are not full are skipped
        if(nand_.block(b).writePtr != nand_.block(b).capacity){
            continue;
        }
        // choosing the block with the fewest pages as the victim... default to the first block on the first iteration
        if (victim < 0 || nand_.block(b).validCount < nand_.block(uint32_t(victim)).validCount) {
            victim = b;                                 
        }
    }
    if (victim < 0) return;

    // TODO 3 (migrate + erase):
    //   For each page p of the victim whose state is PageState::Valid:
    //     a. srcPpn = victim * pagesPerBlock + p;  lpn = p2l[srcPpn]
    //     b. if activeBlock_ is full: activeBlock_ = takeFreeBlock();
    //     c. dstPpn = activeBlock_ * pagesPerBlock + writePtr of activeBlock_
    //     d. nand_.invalidate(srcPpn); nand_.program(dstPpn, ++tick_);
    //        l2p[lpn] = dstPpn; p2l[dstPpn] = lpn; p2l[srcPpn] = -1;
    //     e. stats: ++nandWrites, ++migratedPages,
    //        simTimeUs += timing.readUs + timing.programUs
    //   Then: nand_.erase(victim); freeBlocks_.push_back(victim);
    //         ++stats.erases; ++stats.gcRuns; simTimeUs += timing.eraseUs
    for(uint32_t p = 0; p < cfg_.pagesPerBlock; ++p){
        // free the invalid pages from memory
        if (nand_.block(victim).pages[p] != PageState::Valid) continue;

        // Get the flat physical page numbers instead of using (block, page) for the victim
        uint64_t srcPpn = uint64_t(victim) * cfg_.pagesPerBlock + p;

        // Get the logical page for the victim
        const int64_t lpn = p2l[srcPpn];

        // the moved page has to go somewhere: if the active block is compeletely full (pointer pointing to the end)
        if (nand_.block(uint32_t(activeBlock_)).writePtr == nand_.block(uint32_t(activeBlock_)).capacity)
            activeBlock_ = takeFreeBlock();

        // set the destination pointer based on the assignment of the active block
        uint64_t dstPpn = uint64_t(activeBlock_) * cfg_.pagesPerBlock + nand_.block(uint32_t(activeBlock_)).writePtr;

        // the souce is no longer used so invalidate it
        nand_.invalidate(srcPpn);

        // write the data into the page of the destination pointer and store the timestamp
        nand_.program(dstPpn, ++tick_);

        // point the logical page pointer at the new location
        l2p[uint64_t(lpn)] = int64_t(dstPpn);

        // reverse map the phyisical page pointer
        p2l[dstPpn] = lpn;

        // null the source as its not being used
        p2l[srcPpn] = -1;

        // keeping a tally of the nandWrites and migratedPages
        ++stats.nandWrites;
        ++stats.migratedPages;
        ++stats.qlcWrites;

        // update time to account for read it from the victim + program it into the active block
        stats.simTimeUs += cfg_.timing.readUs + cfg_.timing.programUs;
    }
        // every valid page has moved out, so wipe the whole block
        nand_.erase(uint32_t(victim));

        // free up the block
        freeBlocks_.push_back(uint32_t(victim));

        // +1 erase
        ++stats.erases;
        // +1 garbage collection run
        ++stats.gcRuns;
        // simulation time calculation adding erase time
        stats.simTimeUs += cfg_.timing.eraseUs;

    //   Questions to answer for yourself:
    //   - Why must the victim never be activeBlock_?
    // gc writes to the active block... its the destination... not the thing being erased
    //   - Why does GC only run when a new block is needed, not on every write?
   // less gc means lower waf which is good and erasing a full block is expensive in time
}

void Ftl::write(uint64_t lpn) {
    if (lpn >= l2p.size()) throw std::out_of_range("write: LPN beyond logical capacity");

    // ---- SLC path: try the fast cache first ----
    // does the SLC active block exist and have a free page left?
    const bool slcRoom = activeSlcBlock_ >= 0 && nand_.block(uint32_t(activeSlcBlock_)).writePtr < nand_.block(activeSlcBlock_).capacity;

    if (slcRoom || !freeSlcBlocks_.empty()) {
        // open a new SLC block if the current one is missing or full
        if (!slcRoom) {
            // TODO b: activeSlcBlock_ = the front of freeSlcBlocks_
            activeSlcBlock_ = freeSlcBlocks_.front();
            // TODO b: remove it from freeSlcBlocks_   (hint: pop_front)
            freeSlcBlocks_.pop_front();
        }

        
        // TODO c: copy the 4 lines of "2. Out-of-place update" from below
        // when its out-of-place invalidate the old copy
        if(l2p[lpn] >= 0){
            nand_.invalidate(uint64_t(l2p[lpn]));
            p2l[uint64_t(l2p[lpn])] = -1;
        }

        // program the next page of the SLC active block
        const uint64_t ppn = uint64_t(activeSlcBlock_) * cfg_.pagesPerBlock + nand_.block(uint32_t(activeSlcBlock_)).writePtr;
        // update time
        nand_.program(ppn, ++tick_);
        // update both maps
        l2p[lpn] = int64_t(ppn);
        p2l[ppn] = int64_t(lpn);


        // if the block filled up just now... remember it 
        if (nand_.block(uint32_t(activeSlcBlock_)).writePtr == nand_.block(uint32_t(activeSlcBlock_)).capacity)
            fullSlcBlocks_.push_back(uint32_t(activeSlcBlock_));

        ++stats.hostWrites;
        ++stats.nandWrites;
        ++stats.slcWrites;
        stats.simTimeUs += cfg_.slcTiming.programUs;   // fast SLC program (150 us)
        return;   // done: don't fall through to the QLC path
    }

    // ---- QLC path: SLC cache is full -> slow direct-to-QLC write (the "write cliff") ----


    // 1. Make sure the active block has room
    if (activeBlock_ < 0 || nand_.block(uint32_t(activeBlock_)).writePtr == nand_.block(uint32_t(activeBlock_)).capacity)
        activeBlock_ = allocateBlock();

    // 2. Out-of-place update: invalidate the old copy
    if (l2p[lpn] >= 0) {
        nand_.invalidate(uint64_t(l2p[lpn]));
        p2l[uint64_t(l2p[lpn])] = -1;
    }

    // 3. Program the next page of the active block and update both maps
    const uint64_t ppn = uint64_t(activeBlock_) * cfg_.pagesPerBlock
                       + nand_.block(uint32_t(activeBlock_)).writePtr;
    nand_.program(ppn, ++tick_);
    l2p[lpn] = int64_t(ppn);
    p2l[ppn] = int64_t(lpn);

    ++stats.hostWrites;
    ++stats.nandWrites;
    ++stats.qlcWrites;          // this host write went to QLC
    ++stats.directQlcWrites;    // ...and skipped the SLC cache
    stats.simTimeUs += cfg_.timing.programUs;

}

void Ftl::foldOneBlock() {
    // TODO F1: nothing to fold? return
    //   take the OLDEST full SLC block: v = front of fullSlcBlocks_, then pop it
    if(fullSlcBlocks_.empty()) return;

    // the oldest slc block is at the front because its a queue... pop it and choose it as the victim
    const uint32_t v = fullSlcBlocks_.front();
    fullSlcBlocks_.pop_front();

    // TODO F2: loop p over the victim's pages (p < its capacity):
    //   - Invalid page -> ++stats.absorbedPages; continue   (overwritten while in SLC: never goes to QLC)
    //   - not Valid -> continue
    //   - Valid page -> move it to QLC, exactly like your runGc() loop:
    //       a. srcPpn and lpn                         (same as GC)
    //       b. QLC active block missing or full?      activeBlock_ = allocateBlock();
    //          (allocateBlock, not takeFreeBlock: it runs QLC GC if the pool is low)
    //       c. dstPpn                                 (same as GC)
    //       d. invalidate src, program dst, update l2p / p2l   (same as GC)
    //       e. stats: ++nandWrites, ++qlcWrites, ++foldedPages
    //          simTimeUs += cfg_.slcTiming.readUs + cfg_.timing.programUs   (read from SLC, program to QLC)
    for(uint32_t p = 0; p < nand_.block(v).capacity; ++p){

        // if an invalid page was overwritten in the SLC it never has to go into the QLC
        if (nand_.block(v).pages[p] == PageState::Invalid) {
            ++stats.absorbedPages;
            continue;
        }

        // Get the flat physical page numbers instead of using (block, page) for the victim
        uint64_t srcPpn = uint64_t(v) * cfg_.pagesPerBlock + p;

        // Get the logical page for the victim
        const int64_t lpn = p2l[srcPpn];

        // allocate a block if there no activeBlock or its pointing at the end
        if (activeBlock_ < 0 || nand_.block(uint32_t(activeBlock_)).writePtr == nand_.block(uint32_t(activeBlock_)).capacity)
            activeBlock_ = allocateBlock();

        // set the destination pointer based on the assignment of the active block
        uint64_t dstPpn = uint64_t(activeBlock_) * cfg_.pagesPerBlock + nand_.block(uint32_t(activeBlock_)).writePtr;

        // the souce is no longer used so invalidate it
        nand_.invalidate(srcPpn);

        // write the data into the page of the destination pointer and store the timestamp
        nand_.program(dstPpn, ++tick_);

        // point the logical page pointer at the new location
        l2p[uint64_t(lpn)] = int64_t(dstPpn);

        // reverse map the phyisical page pointer
        p2l[dstPpn] = lpn;

        // null the source as its not being used
        p2l[srcPpn] = -1;


        // update metrics after the fold
        ++stats.nandWrites;
        ++stats.qlcWrites;
        ++stats.foldedPages;
        stats.simTimeUs += cfg_.slcTiming.readUs + cfg_.timing.programUs;

        // erase the victim from SLC as everything is moved to the QLC
        
    }

    // TODO F3: after the loop:
    //   - erase v, push it onto freeSlcBlocks_
    nand_.erase(v);
    freeSlcBlocks_.push_back(v);
    //   - if v was the SLC active block: activeSlcBlock_ = -1      <- important, see question 2
    if (activeSlcBlock_ == int64_t(v)) activeSlcBlock_ = -1;

    //   metrics
    ++stats.erases;
    ++stats.folds;

    stats.simTimeUs += cfg_.slcTiming.eraseUs;
}

void Ftl::idle(uint64_t us) {
    // when does this idle period end?
    const uint64_t end = stats.simTimeUs + us;

    // fold full SLC blocks while there's idle time left (a started fold always finishes)
    while (cfg_.foldOnIdle && !fullSlcBlocks_.empty() && stats.simTimeUs < end)
        foldOneBlock();

    // the idle time passes even if there was nothing (or not enough) to fold
    if (stats.simTimeUs < end) stats.simTimeUs = end;
}

int64_t Ftl::read(uint64_t lpn) {
    if (lpn >= l2p.size()) throw std::out_of_range("read: LPN beyond logical capacity");
    ++stats.hostReads;
    if (l2p[lpn] >= 0) stats.simTimeUs += cfg_.timing.readUs;  // unmapped: no NAND access
    return l2p[lpn];
}

void Ftl::trim(uint64_t lpn) {
    if (lpn >= l2p.size()) throw std::out_of_range("trim: LPN beyond logical capacity");
    if (l2p[lpn] < 0) return;
    nand_.invalidate(uint64_t(l2p[lpn]));
    p2l[uint64_t(l2p[lpn])] = -1;
    l2p[lpn] = -1;
}

bool Ftl::checkInvariants() const {
    uint64_t mapped = 0, valid = 0;
    for (uint64_t lpn = 0; lpn < l2p.size(); ++lpn) {
        if (l2p[lpn] < 0) continue;
        ++mapped;
        if (p2l[uint64_t(l2p[lpn])] != int64_t(lpn)) return false;  // maps disagree
    }
    for (uint32_t b = 0; b < nand_.numBlocks(); ++b) valid += nand_.block(b).validCount;
    return mapped == valid;
}
