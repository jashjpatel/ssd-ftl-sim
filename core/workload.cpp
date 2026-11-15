#include "workload.hpp"
#include <queue>
#include <algorithm>
#include <algorithm>

Workload::Workload(const WorkloadConfig& cfg, uint64_t logicalPages)
    : cfg_(cfg), logicalPages_(logicalPages),
      hotLbas_(std::max<uint64_t>(1, uint64_t(double(logicalPages) * cfg.hotFraction))),
      rng_(cfg.seed) {}

uint64_t Workload::nextLba() {
    switch (cfg_.pattern) {
    case Pattern::Sequential: { const uint64_t l = seqNext_; seqNext_ = (seqNext_ + 1) % logicalPages_; return l; }
    case Pattern::UniformRandom: return rng_.below(logicalPages_);
    case Pattern::HotCold:
        if (rng_.unit() < cfg_.hotProbability) return rng_.below(hotLbas_);
        return hotLbas_ + rng_.below(logicalPages_ - hotLbas_);
    }
    return 0;
}

ExperimentResult runExperiment(const Config& drive, const WorkloadConfig& wl) {
    Ftl ftl(drive);
    NvmeController ctrl(ftl);
    if (wl.precondition) {
        for (uint64_t l = 0; l < ftl.logicalPages(); ++l) ftl.write(l);
        ftl.idle(1000000000ULL);
        ftl.stats = Stats{};
    }
    Workload w(wl, ftl.logicalPages());
    ExperimentResult r;
    uint16_t cid = 0;
    uint64_t busyUs = 0;
    const uint64_t slowThreshold = drive.slcTiming.programUs;
    for (uint32_t b = 0; b < wl.bursts; ++b) {
        for (uint32_t c = 0; c < wl.burstCommands; ++c) {
            ctrl.submit(w.nextWrite(cid++));
            ctrl.process(1);
            NvmeCompletion done{};
            while (ctrl.popCompletion(done)) {
                r.latenciesUs.push_back(done.latencyUs);
                busyUs += done.latencyUs;
                r.maxLatencyUs = std::max(r.maxLatencyUs, done.latencyUs);
                if (done.latencyUs > slowThreshold) ++r.slowCommands;
            }
        }
        if (wl.idleUs) ftl.idle(wl.idleUs);
    }
    const Stats& s = ftl.stats;
    r.logicalPages = ftl.logicalPages();
    r.hostWrites = s.hostWrites;
    r.avgLatencyUs = r.latenciesUs.empty() ? 0 : double(busyUs) / double(r.latenciesUs.size());
    r.throughputMBps = busyUs ? double(s.hostWrites) * 4096.0 / double(busyUs) : 0;
    r.waf = s.waf(); r.qlcPerHost = s.qlcPerHost();
    r.qlcIngressPerHost = s.hostWrites ? double(s.directQlcWrites + s.foldedPages) / double(s.hostWrites) : 0;
    r.folds = s.folds; r.absorbedPages = s.absorbedPages; r.gcRuns = s.gcRuns; r.directQlcWrites = s.directQlcWrites;
    return r;
}

uint64_t percentile(std::vector<uint64_t> latencies, double pct) {
    if (latencies.empty()) return 0;
    // TODO E4 (percentile, nearest rank):
    //   1. sort the latencies (std::sort; it's a copy, so sorting doesn't change the caller's vector)
    std::sort(latencies.begin(), latencies.end());
    //   2. rank = pct% of the count, rounded UP  (e.g. 99% of 100 samples -> 99, 50% of 1 sample -> 1)
    uint64_t rank = size_t(pct/100.0 * double(latencies.size()) + 0.999999);
    //      hint: size_t(pct / 100.0 * double(latencies.size()) + 0.999999), and never less than 1
    if (rank < 1) rank = 1;          // never less than 1
    //   3. return the rank-th smallest value (careful: rank 1 is index 0)
    return latencies[rank - 1];
}

QdResult runQueueDepth(const Config& drive, const WorkloadConfig& wl, uint32_t qd, uint64_t commands) {
    Ftl ftl(drive);
    if (wl.precondition) {
        for (uint64_t l = 0; l < ftl.logicalPages(); ++l) ftl.write(l);
        ftl.idle(1000000000ULL);
        ftl.stats = Stats{};
    }
    // measuring starts when every die and channel is idle
    uint64_t start = 0;
    for (uint32_t d = 0; d < ftl.numDies(); ++d) start = std::max(start, ftl.dieFreeAtUs(d));
    for (uint32_t c = 0; c < drive.channels; ++c) start = std::max(start, ftl.channelFreeAtUs(c));

    Workload w(wl, ftl.logicalPages());
    QdResult r;
    // completion times of the commands in flight; top() = the one that finishes first
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> inFlight;
    uint64_t end = start;
    for (uint64_t i = 0; i < commands; ++i) {
        // TODO E3 (the closed loop: the heart of queue depth)
        //   a. when does command i get submitted?
        //        - while fewer than qd are in flight: right away, at `start`
        //        - once qd are in flight: the host must wait for a free slot, which opens when the
        //          EARLIEST in-flight command completes -> that's inFlight.top(); then pop it
        //   b. tell the FTL the time (ftl.setNow), and write the next LBA (w.nextLba())
        //   c. doneUs = ftl.lastDoneUs(); push it into inFlight (it now occupies a slot)
        //   d. record latency = done - submit in r.latenciesUs, and keep `end` = the latest doneUs
        uint64_t submitUs = start;
        if(inFlight.size() == qd){
            submitUs = inFlight.top();
            inFlight.pop();
        }
        ftl.setNow(submitUs);
        ftl.write(w.nextLba());
        const uint64_t doneUs = ftl.lastDoneUs();
        inFlight.push(doneUs);
        // latency is the time between complete and submitted
        r.latenciesUs.push_back(doneUs - submitUs);
        end = std::max(end, doneUs);
    }
    r.commands = commands;
    r.totalUs = end - start;
    r.iops = r.totalUs ? double(commands) * 1e6 / double(r.totalUs) : 0;
    uint64_t sum = 0;
    for (uint64_t l : r.latenciesUs) { sum += l; r.maxUs = std::max(r.maxUs, l); }
    r.avgLatencyUs = commands ? double(sum) / double(commands) : 0;
    r.p50Us = percentile(r.latenciesUs, 50);
    r.p99Us = percentile(r.latenciesUs, 99);
    r.gcRuns = ftl.stats.gcRuns;
    return r;
}
