#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include "ftl.hpp"

// A tiny NVMe-style front end. Real NVMe: the host puts 64-byte commands in a
// Submission Queue (SQ) in host memory, rings a doorbell register, and the
// controller posts 16-byte entries to a Completion Queue (CQ). Same shape here.

enum class NvmeOpcode : uint8_t {
    Write       = 0x01,
    Read        = 0x02,
    DatasetMgmt = 0x09,   // with the Deallocate attribute = TRIM
};

namespace NvmeStatus {
    constexpr uint16_t Success       = 0x00;
    constexpr uint16_t InvalidOpcode = 0x01;
    constexpr uint16_t LbaOutOfRange = 0x80;
}

struct NvmeCommand {
    uint16_t   cid;   // command identifier, echoed back in the completion
    NvmeOpcode opc;
    uint64_t   slba;  // starting LBA (1 LBA = 1 page = 4 KB in this sim)
    uint16_t   nlb;   // number of logical blocks, 0-based like the spec: 0 means 1 block
};

struct NvmeCompletion {
    uint16_t cid;
    uint16_t status;
    uint64_t latencyUs;   // FTL simulated time spent on this command
};

class NvmeController {
public:
    explicit NvmeController(Ftl& ftl) : ftl_(ftl) {}

    void   submit(const NvmeCommand& cmd) { sq_.push_back(cmd); }   // host writes an SQ entry
    size_t process(size_t maxCommands);                            // "doorbell": device runs up to N commands
    bool   popCompletion(NvmeCompletion& out);                     // host reaps one CQ entry

    size_t sqDepth() const { return sq_.size(); }
    size_t cqDepth() const { return cq_.size(); }

private:
    Ftl& ftl_;
    std::deque<NvmeCommand>    sq_;
    std::deque<NvmeCompletion> cq_;
};