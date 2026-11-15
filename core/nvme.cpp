#include "nvme.hpp"
#include <algorithm>

size_t NvmeController::process(size_t maxCommands) {
    size_t done = 0;
    while (done < maxCommands && !sq_.empty()) {
        const NvmeCommand cmd = sq_.front(); sq_.pop_front();
        const uint64_t t0 = ftl_.stats.simTimeUs;
        const uint64_t cap = ftl_.logicalPages();
        uint16_t status = NvmeStatus::Success;
        if (cmd.opc != NvmeOpcode::Write && cmd.opc != NvmeOpcode::Read && cmd.opc != NvmeOpcode::DatasetMgmt) {
            status = NvmeStatus::InvalidOpcode;
        } else if (cmd.slba >= cap || uint64_t(cmd.nlb) > cap - 1 - cmd.slba) {
            status = NvmeStatus::LbaOutOfRange;
        } else {
            const uint32_t lu = ftl_.config().luPerIu;
            const uint64_t last = cmd.slba + cmd.nlb;                    // nlb is 0-based: last LBA, inclusive
            for (uint64_t lba = cmd.slba; lba <= last; ) {
                // TODO I4: split the command at IU boundaries, so each ftl_ call stays inside ONE IU.
                //   nextIu = the first LBA of the NEXT IU after lba      (lba / lu gives lba's IU)
                //   n      = how many LBAs from lba up to nextIu, but never past the end of the command
                //            (careful: last is inclusive, so the command ends just before last + 1)
                //   example, lu = 4, command LBAs 2..9: chunks [2,3]  [4..7]  [8,9]  -> n = 2, 4, 2
                //   Writes of a whole IU skip read-modify-write, so splitting this way is what makes 16 KB writes cheap.
                const uint64_t nextIu = (lba / lu+1) * lu;
                const uint32_t n = uint32_t(std::min(nextIu, last + 1) - lba);
                if (cmd.opc == NvmeOpcode::Write) ftl_.write(lba, n);
                else if (cmd.opc == NvmeOpcode::Read) { for (uint32_t k = 0; k < n; ++k) ftl_.read(lba + k); }
                else ftl_.trim(lba, n);
                lba += n;
            }
        }
        cq_.push_back({cmd.cid, status, ftl_.stats.simTimeUs - t0});
        ++done;
    }
    return done;
}

bool NvmeController::popCompletion(NvmeCompletion& out) {
    if (cq_.empty()) return false;
    out = cq_.front(); cq_.pop_front();
    return true;
}
