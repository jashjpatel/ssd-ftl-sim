#include "nvme.hpp"

// The "doorbell": the host has written commands into the SQ and tells the
// device to go. Run up to maxCommands of them, in order, and post one
// completion per command. Returns how many commands were processed.
size_t NvmeController::process(size_t maxCommands) {
    size_t done = 0;

    // N1: run commands in order until we hit the limit or the SQ is empty
    while (done < maxCommands && !sq_.empty()) {
        // take the oldest command (FIFO, just like fullSlcBlocks_)
        const NvmeCommand cmd = sq_.front();
        sq_.pop_front();

        // clock before the command runs, and the drive size
        const uint64_t t0  = ftl_.stats.simTimeUs;
        const uint64_t cap = ftl_.logicalPages();

        // c. assume success until a check fails
        uint16_t status = NvmeStatus::Success;

        // measures whether or not the opcode is valid 
        if (cmd.opc != NvmeOpcode::Write && cmd.opc != NvmeOpcode::Read && cmd.opc != NvmeOpcode::DatasetMgmt) {
            status = NvmeStatus::InvalidOpcode;
        } 
        else if (cmd.slba >= cap || uint64_t(cmd.nlb) > cap - 1 - cmd.slba) {
            status = NvmeStatus::LbaOutOfRange;
        } 
        else {
            // execute every LBA in the range 
            for (uint64_t lba = cmd.slba; lba <= cmd.slba + cmd.nlb; ++lba){
                if (cmd.opc == NvmeOpcode::Write){    
                    ftl_.write(lba);
                }
                else if(cmd.opc == NvmeOpcode::Read){           
                    ftl_.read(lba);
                }
                else{
                    ftl_.trim(lba);
                }   
            }
        }

        // the command has been processed and completed
        cq_.push_back({cmd.cid, status, ftl_.stats.simTimeUs - t0});

        // e. count it
        ++done;
    }
    return done;
}

// The host reaps one completion (oldest first). Returns false if the CQ is empty.
bool NvmeController::popCompletion(NvmeCompletion& out) {
    if (cq_.empty()) return false;
    out = cq_.front();
    cq_.pop_front();
    return true;
}