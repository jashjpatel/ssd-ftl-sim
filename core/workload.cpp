#include "workload.hpp"
#include <algorithm>

// The hot set is the first hotFraction of the drive (at least 1 LBA).
Workload::Workload(const WorkloadConfig& cfg, uint64_t logicalPages)
    : cfg_(cfg), logicalPages_(logicalPages),
      hotLbas_(std::max<uint64_t>(1, uint64_t(double(logicalPages) * cfg.hotFraction))),
      rng_(cfg.seed) {}

uint64_t Workload::nextLba() {
    switch (cfg_.pattern) {
    case Pattern::Sequential: {
        // TODO W1 (sequential): return seqNext_, then advance it by 1,
        uint64_t lba = seqNext_;
        seqNext_++;
        //   wrapping back to 0 at the end of the drive.
        seqNext_ = (seqNext_) % logicalPages_;
        return lba;
    }
    case Pattern::UniformRandom:
        // TODO W2 (uniform): any LBA from 0 to logicalPages_ - 1, all equally likely.
        // return random LBA
        return rng_.below(logicalPages_);
    
    case Pattern::HotCold:
        // hot lba 80% of time
        if (rng_.unit() < cfg_.hotProbability)
            return rng_.below(hotLbas_);                          // HOT:  0 .. hotLbas_-1
        // cold lba 20% of time
        return hotLbas_ + rng_.below(logicalPages_ - hotLbas_);   // COLD: hotLbas_ .. logicalPages_-1
    }
}

ExperimentResult runExperiment(const Config& drive, const WorkloadConfig& wl) {
    Ftl ftl(drive);
    NvmeController ctrl(ftl);

    // Precondition: write every LBA once and let folding finish, so we measure a
    // full, steady drive instead of a fresh one. Then zero the stats.
    if (wl.precondition) {
        for (uint64_t l = 0; l < ftl.logicalPages(); ++l) ftl.write(l);
        ftl.idle(1000000000ULL);
        ftl.stats = Stats{};
    }

    Workload w(wl, ftl.logicalPages());
    ExperimentResult r;
    uint16_t cid = 0;
    uint64_t busyUs = 0;                                        // total time spent on host commands
    const uint64_t slowThreshold = drive.slcTiming.programUs;   // slower than an SLC write = cliff or GC

    // TODO W4 (the measurement loop):
    //   for each burst b
    for(uint32_t b = 0; b < wl.bursts; ++b){
        // for each command within the burst
        for(uint32_t c = 0; c < wl.burstCommands; ++c){
            // submit the next write
            ctrl.submit(w.nextWrite(cid++));
            // perform the action
            ctrl.process(1);
            // process that its done
            NvmeCompletion done{};  ctrl.popCompletion(done);
            // note down the latency
            r.latenciesUs.push_back(done.latencyUs);
            busyUs += done.latencyUs;
            r.maxLatencyUs = std::max(r.maxLatencyUs, done.latencyUs);
            // check the latency againt the threshold
            if (done.latencyUs > slowThreshold){
                ++r.slowCommands;
            }
            // detect the idle time between bursts and let the flash transition layer handle folding
        }
        if (wl.idleUs > 0){
                ftl.idle(wl.idleUs);
        }
    }

    const Stats& s = ftl.stats;
    r.logicalPages = ftl.logicalPages();
    r.hostWrites = s.hostWrites;
    r.avgLatencyUs = r.latenciesUs.empty() ? 0 : double(busyUs) / double(r.latenciesUs.size());
    r.throughputMBps = busyUs ? double(s.hostWrites) * 4096.0 / double(busyUs) : 0;   // bytes per us = MB/s
    r.waf = s.waf();
    r.qlcPerHost = s.qlcPerHost();
    r.qlcIngressPerHost = s.hostWrites ? double(s.directQlcWrites + s.foldedPages) / double(s.hostWrites) : 0;
    r.folds = s.folds;
    r.absorbedPages = s.absorbedPages;
    r.gcRuns = s.gcRuns;
    r.directQlcWrites = s.directQlcWrites;
    return r;
}