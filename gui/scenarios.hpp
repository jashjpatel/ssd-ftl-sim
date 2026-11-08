#pragma once
// Step-through test-case scenarios for the GUI.
// Each scenario replays a test case from cli/main.cpp on its tiny drive, one step
// at a time, through the real NvmeController + Ftl, and draws every page.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"
#include "ftl.hpp"
#include "nvme.hpp"

namespace scn {

// ---------------------------------------------------------------------------
// Scenario definitions
// ---------------------------------------------------------------------------
struct Cmd  { NvmeOpcode opc; uint64_t slba; uint16_t nlb; };
struct Step {
    std::vector<Cmd> cmds;   // NVMe commands sent in this step (may be empty)
    uint64_t idleUs;         // then let the drive idle this long (0 = no idle)
    const char* title;       // short label
    const char* explain;     // plain-English description of what happens
};
enum class Drive { Tiny, Slc };
struct Scenario {
    const char* name;
    const char* summary;
    Drive drive;
    std::vector<Step> steps;
};

static Cmd W(uint64_t slba, uint16_t nlb = 0) { return {NvmeOpcode::Write, slba, nlb}; }
static Cmd R(uint64_t slba, uint16_t nlb = 0) { return {NvmeOpcode::Read, slba, nlb}; }
static Cmd T(uint64_t slba, uint16_t nlb = 0) { return {NvmeOpcode::DatasetMgmt, slba, nlb}; }

// Shared prefix for TC4-TC7: the "drive fills up" sequence.
static std::vector<Step> tc4Steps() {
    return {
        {{W(0, 3)}, 0, "Write LBAs 0-3",
         "One Write command covering 4 LBAs (slba 0, nlb 3). The FTL opens B0 and programs its pages in order. B0 is now full."},
        {{W(1)}, 0, "Overwrite LBA 1",
         "Out-of-place update: the new copy of LBA 1 goes to B1 page 0, and the old copy in B0 page 1 is marked invalid."},
        {{W(4, 2)}, 0, "Write LBAs 4-6",
         "Three new LBAs fill the rest of B1. B1 is now full."},
        {{W(0), W(2), W(3), W(5)}, 0, "Overwrite LBAs 0, 2, 3, 5",
         "Four single-LBA Writes. They fill B2, which leaves B0 with zero valid pages and B1 with one invalid page (old LBA 5). "
         "Only B3 is free: the pool is at the GC threshold, but GC hasn't run because no write has needed a new block yet. WAF is still 1.0."},
    };
}

static std::vector<Scenario> buildScenarios() {
    std::vector<Scenario> v;

    v.push_back({"TC1  First writes", "A fresh drive: pages are programmed in order.", Drive::Tiny, {
        {{W(0, 3)}, 0, "Write LBAs 0-3",
         "One NVMe Write with slba 0, nlb 3 (nlb counts from 0, so that's 4 LBAs). The controller calls the FTL once per LBA. "
         "The FTL opens B0 and fills it in order: LBA n lands on page n. 4 host writes cost 4 NAND writes, so WAF = 1.0. "
         "Latency: 4 programs x 200 us = 800 us."},
    }});

    v.push_back({"TC2  Overwrite (out-of-place update)", "Flash can't overwrite a page in place.", Drive::Tiny, {
        {{W(0, 3)}, 0, "Write LBAs 0-3", "B0 fills with LBAs 0 to 3."},
        {{W(1)}, 0, "Overwrite LBA 1",
         "The host rewrites LBA 1. NAND can't overwrite a programmed page, so the FTL writes the new data to the next free page "
         "(B1 page 0 = PPN 4), marks the old page (PPN 1) invalid, and updates l2p[1] = 4 and p2l. The invalid page still takes up "
         "space until its block is erased."},
    }});

    v.push_back({"TC3  Reads", "A read only touches NAND if the LBA is mapped.", Drive::Tiny, {
        {{W(1)}, 0, "Write LBA 1", "LBA 1 lands on B0 page 0 (PPN 0)."},
        {{R(1)}, 0, "Read LBA 1",
         "l2p[1] = 0, so the controller reads PPN 0 from NAND: 25 us. The completion carries the latency back to the host."},
        {{R(6)}, 0, "Read LBA 6 (never written)",
         "l2p[6] = -1 (unmapped), so there's nothing on flash to read. The FTL answers without touching NAND: 0 us."},
    }});

    v.push_back({"TRIM + bad LBA", "Dataset Management (TRIM) and an error status.", Drive::Tiny, {
        {{W(5)}, 0, "Write LBA 5", "LBA 5 lands on B0 page 0."},
        {{T(5)}, 0, "Dataset Management (deallocate) LBA 5",
         "This is NVMe's TRIM: the host says it no longer needs LBA 5 (say, the file was deleted). The FTL marks the page invalid "
         "and unmaps the LBA. No data is written; GC can now reclaim that page for free."},
        {{R(5)}, 0, "Read LBA 5", "LBA 5 is unmapped now, so the read returns no data and costs 0 us."},
        {{W(8)}, 0, "Write LBA 8 (past the end)",
         "This drive has only 8 LBAs (0 to 7) because half its space is over-provisioning. The controller validates the command "
         "first, rejects it with status 0x80 (LBA Out of Range), and changes nothing. Errors come back in the completion, not as a crash."},
    }});

    v.push_back({"TC4  Drive fills, no GC yet", "Building up invalid pages.", Drive::Tiny, tc4Steps()});

    {
        auto s = tc4Steps();
        s.push_back({{W(7)}, 0, "Write LBA 7: GC + wear leveling",
            "B2 is full and only one free block is left, so GC runs before the write. Greedy GC picks B0, the full block with the "
            "fewest valid pages (zero). Nothing to copy: B0 is erased (erase count 1) and goes back to the pool. Now the pool holds B3 "
            "(erase count 0) and B0 (erase count 1). Wear leveling picks the lowest erase count, B3, so LBA 7 lands at PPN 12, not PPN 0. "
            "The command took 1700 us: the 1500 us erase plus the 200 us program."});
        v.push_back({"TC5  GC frees an all-invalid block", "Free GC, then wear leveling.", Drive::Tiny, s});
    }
    {
        auto s = tc4Steps();
        s.push_back({{W(7)}, 0, "Write LBA 7", "GC erases all-invalid B0; wear leveling puts LBA 7 in B3 (see TC5)."});
        s.push_back({{W(0), W(2), W(3)}, 0, "Overwrite LBAs 0, 2, 3",
            "B3 fills up. B2 is left with just one valid page (LBA 5); its other three are invalid. Only B0 is free again."});
        s.push_back({{W(6)}, 0, "Write LBA 6: GC must copy a page",
            "B3 is full and the pool is at the threshold, so GC runs. It picks B2 (1 valid page) over B1 (3 valid). LBA 5 is still live, "
            "so before erasing B2, GC reads it (25 us) and programs it into a fresh block, B0 page 0 (200 us). That's a NAND write the host "
            "never asked for: write amplification. Then B2 is erased (1500 us) and the host's LBA 6 goes to B0 page 1 (200 us). "
            "This one command took 1925 us instead of 200. 17 host writes, 18 NAND writes: WAF 1.06."});
        v.push_back({"TC6  GC migrates a valid page", "Write amplification and a latency spike.", Drive::Tiny, s});
    }
    {
        auto s = tc4Steps();
        s.push_back({{W(7)}, 0, "Write LBA 7", "GC erases all-invalid B0; wear leveling puts LBA 7 in B3."});
        s.push_back({{W(0), W(2), W(3)}, 0, "Overwrite LBAs 0, 2, 3", "B3 fills. B2 holds one live page, LBA 5."});
        s.push_back({{T(5)}, 0, "TRIM LBA 5",
            "Same point as TC6, but now the host deletes LBA 5 first. Its page in B2 becomes invalid, so B2 is now all garbage."});
        s.push_back({{W(6)}, 0, "Write LBA 6: GC with nothing to copy",
            "GC picks B2 again, but it has zero valid pages: erase it, no copying. Compare with TC6: TRIM saved a NAND write, so WAF stays 1.0."});
        v.push_back({"TC7  TRIM before GC", "TRIM means nothing to migrate.", Drive::Tiny, s});
    }

    v.push_back({"S1/S2  SLC cache and the write cliff", "Fast SLC until it fills, then slow QLC.", Drive::Slc, {
        {{W(0, 3)}, 0, "Write LBAs 0-3",
         "This drive has 2 SLC blocks (B0, B1). SLC stores 1 bit per cell, so each block holds only 2 of its 8 pages, but programs in "
         "150 us. The 4 LBAs fill both SLC blocks: 4 x 150 = 600 us. The SLC cache is now full."},
        {{W(4)}, 0, "Write LBA 4: the write cliff",
         "No SLC page is free and there's no free SLC block, so the write falls through to QLC (4 bits per cell): B2 page 0, 2500 us. "
         "About 17x slower. This is the write cliff a host sees when the cache runs out."},
        {{W(0)}, 0, "Overwrite LBA 0",
         "Even data that's already in SLC can't be rewritten there: the new copy goes to QLC (2500 us) and the old SLC copy is invalidated. "
         "Nothing frees SLC until folding runs."},
    }});

    v.push_back({"S4  Folding (SLC -> QLC)", "Idle time drains the cache, oldest block first.", Drive::Slc, {
        {{W(0, 3)}, 0, "Write LBAs 0-3", "Both SLC blocks fill: B0 holds LBAs 0, 1 and B1 holds LBAs 2, 3."},
        {{W(1)}, 0, "Overwrite LBA 1 (cliff)", "Cache full, so the new LBA 1 goes to QLC B2. Its old copy in B0 becomes invalid."},
        {{}, 1, "Host goes idle: fold one block",
         "With the host quiet, the drive folds the OLDEST full SLC block, B0 (first in, first out). Its one valid page (LBA 0) is read "
         "from SLC and programmed into QLC (20 + 2500 us). The invalid page (old LBA 1) is skipped: it's absorbed and never costs a QLC "
         "write. B0 is erased (3000 us) and returns to the SLC free pool."},
        {{}, 100000, "Keep idling: fold B1",
         "More idle time folds B1: LBAs 2 and 3 move to QLC. Both SLC blocks are empty again, so the next burst will be fast."},
    }});

    v.push_back({"S5  Absorption (the endurance win)", "Hot data overwritten in SLC never reaches QLC.", Drive::Slc, {
        {{W(0, 1)}, 0, "Write LBAs 0-1", "B0 fills with LBAs 0 and 1 (SLC, 150 us each)."},
        {{W(0, 1)}, 0, "Rewrite LBAs 0-1 (hot data)",
         "The host rewrites the same two LBAs. The new copies go to SLC B1, and both pages in B0 become invalid."},
        {{}, 1, "Idle: fold B0",
         "B0 is the oldest full block, but it holds nothing valid: 2 pages absorbed, 0 QLC writes, just the 3000 us erase. "
         "Those first two host writes never cost QLC any wear."},
        {{}, 100000, "Idle: fold B1",
         "B1's two valid pages move to QLC. Total: 4 host writes, 2 QLC writes, so QLC writes per host write = 0.5. "
         "That's why QLC drives put an SLC cache in front."},
    }});

    v.push_back({"N1/N2  NVMe commands and errors", "Multi-LBA commands, cids, and error statuses.", Drive::Slc, {
        {{W(10, 3)}, 0, "Write slba 10, nlb 3",
         "One command, 4 LBAs (10, 11, 12, 13), all into SLC: 4 x 150 = 600 us. The completion echoes the command's cid "
         "so the host can match it."},
        {{W(34, 3)}, 0, "Write slba 34, nlb 3 (runs off the end)",
         "This drive has 36 LBAs (0 to 35), and 34 + 3 = 37 is past the end. The controller checks the whole range first and rejects "
         "the command with status 0x80. Nothing is written, not even LBAs 34 and 35: commands are all-or-nothing."},
        {{{NvmeOpcode(0x7F), 0, 0}}, 0, "Unknown opcode 0x7F",
         "The controller doesn't support this opcode, so it returns status 0x01 (Invalid Command Opcode) and does nothing."},
    }});

    return v;
}

// ---------------------------------------------------------------------------
// Runner: replays steps on a fresh drive (so Back works by replaying)
// ---------------------------------------------------------------------------
struct Result { uint16_t cid; Cmd cmd; uint16_t status; uint64_t latencyUs; };

struct Runner {
    std::vector<Scenario> scenarios = buildScenarios();
    int current = 0;          // scenario index
    int done = 0;             // steps executed so far
    Config cfg;
    std::unique_ptr<Ftl> ftl;
    std::unique_ptr<NvmeController> ctrl;
    uint16_t cid = 1;
    std::vector<Result> lastResults;
    uint64_t lastIdleSpent = 0;
    std::vector<int64_t> prevLpn;      // page -> LPN before the last step
    std::vector<PageState> prevState;  // page -> state before the last step

    static Config driveConfig(Drive d) {
        Config c;
        if (d == Drive::Tiny) { c.blocks = 4; c.pagesPerBlock = 4; c.opRatio = 0.5; c.slcBlocks = 0; }
        else { c.blocks = 8; c.pagesPerBlock = 8; c.opRatio = 0.25; c.slcBlocks = 2; c.timing = {80, 2500, 3000}; c.slcTiming = {20, 150, 3000}; }
        return c;
    }

    void snapshot() {
        const Nand& n = ftl->nand();
        const uint64_t pages = uint64_t(n.numBlocks()) * n.pagesPerBlock();
        prevLpn.assign(pages, -1); prevState.assign(pages, PageState::Free);
        for (uint64_t p = 0; p < pages; ++p) {
            prevLpn[p] = ftl->lpnAt(p);
            prevState[p] = n.block(uint32_t(p / n.pagesPerBlock())).pages[p % n.pagesPerBlock()];
        }
    }

    void execute(const Step& s) {
        lastResults.clear(); lastIdleSpent = 0;
        for (const Cmd& c : s.cmds) {
            ctrl->submit({cid, c.opc, c.slba, c.nlb});
            ctrl->process(1);
            NvmeCompletion done{};
            if (ctrl->popCompletion(done)) lastResults.push_back({done.cid, c, done.status, done.latencyUs});
            ++cid;
        }
        if (s.idleUs) {
            const uint64_t t0 = ftl->stats.simTimeUs;
            ftl->idle(s.idleUs);
            lastIdleSpent = ftl->stats.simTimeUs - t0;
        }
    }

    void runTo(int k) {   // fresh drive, then execute the first k steps
        const Scenario& sc = scenarios[size_t(current)];
        k = std::clamp(k, 0, int(sc.steps.size()));
        cfg = driveConfig(sc.drive);
        ftl = std::make_unique<Ftl>(cfg);
        ctrl = std::make_unique<NvmeController>(*ftl);
        cid = 1; lastResults.clear(); lastIdleSpent = 0;
        for (int i = 0; i < k; ++i) {
            if (i == k - 1) snapshot();
            execute(sc.steps[size_t(i)]);
        }
        if (k == 0) snapshot();
        done = k;
    }

    void select(int i) { current = i; runTo(0); }
};

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static const char* opName(NvmeOpcode o) {
    return o == NvmeOpcode::Write ? "Write" : o == NvmeOpcode::Read ? "Read" : o == NvmeOpcode::DatasetMgmt ? "Dataset Mgmt (TRIM)" : "opcode 0x7F";
}

static void drawDrive(Runner& r) {
    const Nand& n = r.ftl->nand();
    const uint32_t ppb = n.pagesPerBlock();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cell = 46.0f, gap = 6.0f, labelW = 230.0f;
    const ImU32 cFree = IM_COL32(38, 42, 52, 255), cValid = IM_COL32(52, 178, 124, 255),
                cInvalid = IM_COL32(184, 78, 72, 255), cUnused = IM_COL32(24, 27, 33, 255),
                cChanged = IM_COL32(255, 190, 60, 255), cActive = IM_COL32(255, 255, 255, 255);

    for (uint32_t b = 0; b < n.numBlocks(); ++b) {
        const Block& blk = n.block(b);
        const bool active = int64_t(b) == r.ftl->activeBlock() || int64_t(b) == r.ftl->activeSlcBlock();
        const char* state = active ? "ACTIVE" : blk.writePtr == 0 ? "free" : blk.writePtr == blk.capacity ? "full" : "filling";
        ImGui::BeginGroup();
        ImGui::TextColored(blk.mode == CellMode::SLC ? ImVec4(0.45f, 0.85f, 0.6f, 1) : ImVec4(0.6f, 0.75f, 1, 1),
                           "B%u  %s", b, blk.mode == CellMode::SLC ? "SLC" : "QLC");
        ImGui::SameLine(70); ImGui::TextColored(active ? ImVec4(1, 1, 1, 1) : ImVec4(0.6f, 0.6f, 0.65f, 1), "%s", state);
        ImGui::TextDisabled("valid %u/%u   erases %u", blk.validCount, blk.capacity, blk.eraseCount);
        ImGui::EndGroup();
        ImGui::SameLine(labelW);

        const ImVec2 o = ImGui::GetCursorScreenPos();
        for (uint32_t p = 0; p < ppb; ++p) {
            const uint64_t ppn = uint64_t(b) * ppb + p;
            const ImVec2 p0(o.x + float(p) * (cell + gap), o.y), p1(p0.x + cell, p0.y + cell);
            const bool unused = p >= blk.capacity;
            const PageState st = blk.pages[p];
            const ImU32 fill = unused ? cUnused : st == PageState::Valid ? cValid : st == PageState::Invalid ? cInvalid : cFree;
            dl->AddRectFilled(p0, p1, fill, 6.0f);
            const bool changed = !r.prevState.empty() && ppn < r.prevState.size() &&
                                 (r.prevState[ppn] != st || r.prevLpn[ppn] != r.ftl->lpnAt(ppn));
            if (changed) dl->AddRect(p0, p1, cChanged, 4.0f, 0, 3.0f);
            else if (active && !unused) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 90), 4.0f, 0, 1.0f);
            char txt[32];
            if (unused) std::snprintf(txt, sizeof txt, "-");
            else if (st == PageState::Valid) std::snprintf(txt, sizeof txt, "%lld", (long long)r.ftl->lpnAt(ppn));
            else if (st == PageState::Invalid) std::snprintf(txt, sizeof txt, "x");
            else txt[0] = 0;
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText(ImVec2(p0.x + (cell - ts.x) / 2, p0.y + 6), IM_COL32(255, 255, 255, 235), txt);
            char ppnTxt[32];
            std::snprintf(ppnTxt, sizeof ppnTxt, "%llu", (unsigned long long)ppn);
            const ImVec2 ps = ImGui::CalcTextSize(ppnTxt);
            dl->AddText(ImVec2(p1.x - ps.x - 3, p1.y - ps.y - 1), IM_COL32(255, 255, 255, 90), ppnTxt);
            if (ImGui::IsMouseHoveringRect(p0, p1)) {
                ImGui::BeginTooltip();
                ImGui::Text("PPN %llu = block %u, page %u", (unsigned long long)ppn, b, p);
                if (unused) ImGui::TextUnformatted("not used: SLC mode stores 1 bit per cell, so only the first 1/4 of the pages are usable");
                else if (st == PageState::Valid) ImGui::Text("valid: holds LBA %lld", (long long)r.ftl->lpnAt(ppn));
                else if (st == PageState::Invalid) ImGui::TextUnformatted("invalid: an old copy, garbage until the block is erased");
                else ImGui::TextUnformatted("free: erased, ready to program");
                ImGui::EndTooltip();
            }
        }
        if (active) {
            const float w = float(blk.capacity) * (cell + gap) - gap;
            dl->AddRect(ImVec2(o.x - 3, o.y - 3), ImVec2(o.x + w + 3, o.y + cell + 3), cActive, 6.0f, 0, 2.0f);
        }
        ImGui::Dummy(ImVec2(float(ppb) * (cell + gap), cell));
        ImGui::Spacing();
    }
    ImGui::TextDisabled("big number = LBA stored on the page,  x = invalid,  - = unused in SLC mode,  small number = PPN");
    ImGui::TextDisabled("white box = active block,  amber outline = changed by the last step.  Hover any page for details.");
}

static void drawMappingAndStats(Runner& r) {
    const Stats& s = r.ftl->stats;
    ImGui::SeparatorText("Mapping table (l2p): LBA -> PPN");
    int shown = 0;
    for (uint64_t l = 0; l < r.ftl->logicalPages(); ++l) {
        const int64_t ppn = r.ftl->ppnOf(l);
        if (ppn < 0) continue;
        if (shown % 8) ImGui::SameLine();
        ImGui::Text("%2llu -> %-3lld", (unsigned long long)l, (long long)ppn);
        ++shown;
    }
    if (!shown) ImGui::TextDisabled("(nothing mapped yet)");
    ImGui::TextDisabled("%llu LBAs on this drive; unmapped LBAs aren't listed", (unsigned long long)r.ftl->logicalPages());

    ImGui::SeparatorText("Counters");
    ImGui::Text("host writes %llu   NAND writes %llu   WAF %.2f   GC runs %llu   pages migrated by GC %llu",
                (unsigned long long)s.hostWrites, (unsigned long long)s.nandWrites, s.waf(),
                (unsigned long long)s.gcRuns, (unsigned long long)s.migratedPages);
    if (r.cfg.slcBlocks)
        ImGui::Text("SLC writes %llu   QLC writes %llu   folds %llu   absorbed %llu   QLC writes per host write %.2f",
                    (unsigned long long)s.slcWrites, (unsigned long long)s.qlcWrites, (unsigned long long)s.folds,
                    (unsigned long long)s.absorbedPages, s.qlcPerHost());
    ImGui::Text("simulated time %.3f ms", double(s.simTimeUs) / 1000.0);
}

// Full-window test-case view.
static void window(Runner& r) {
    if (!r.ftl) r.select(0);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("NAND FTL Simulator: test cases, one NVMe command at a time###testcases", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Left column: scenario buttons
    ImGui::BeginChild("list", ImVec2(290, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Pick a test case");
    for (size_t i = 0; i < r.scenarios.size(); ++i) {
        const bool sel = int(i) == r.current;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(r.scenarios[i].name, ImVec2(-1, 0))) r.select(int(i));
        if (sel) ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", r.scenarios[i].summary);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected scenario
    ImGui::BeginChild("detail", ImVec2(0, 0));
    const Scenario& sc = r.scenarios[size_t(r.current)];
    const int total = int(sc.steps.size());
    ImGui::Text("%s", sc.name);
    ImGui::SameLine();
    ImGui::TextDisabled("  drive: %s", sc.drive == Drive::Tiny ? "4 blocks x 4 pages, 8 LBAs (TC1-TC7)"
                                                               : "8 blocks x 8 pages, B0-B1 SLC, 36 LBAs (TC-S / TC-N)");
    if (ImGui::Button("Restart")) r.runTo(0);
    ImGui::SameLine();
    ImGui::BeginDisabled(r.done == 0);
    if (ImGui::Button("< Back")) r.runTo(r.done - 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(r.done >= total);
    if (ImGui::Button("Next step >")) r.runTo(r.done + 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("   step %d of %d", r.done, total);

    // What happened in the last step
    ImGui::SeparatorText("What just happened");
    if (r.done == 0) {
        ImGui::TextWrapped("Empty drive. Press \"Next step\" to send the first NVMe command. Upcoming: %s.", sc.steps[0].title);
    } else {
        const Step& st = sc.steps[size_t(r.done - 1)];
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "Step %d: %s", r.done, st.title);
        for (const Result& res : r.lastResults) {
            const ImVec4 col = res.status != NvmeStatus::Success ? ImVec4(1, 0.45f, 0.4f, 1)
                             : res.latencyUs > 1000 ? ImVec4(1, 0.7f, 0.3f, 1) : ImVec4(0.6f, 0.85f, 1, 1);
            if (res.cmd.opc == NvmeOpcode::Write || res.cmd.opc == NvmeOpcode::Read || res.cmd.opc == NvmeOpcode::DatasetMgmt)
                ImGui::TextColored(col, "  NVMe %-20s cid %-3u slba %-3llu nlb %u  (LBAs %llu-%llu)  ->  status 0x%02X, %llu us",
                                   opName(res.cmd.opc), res.cid, (unsigned long long)res.cmd.slba, res.cmd.nlb,
                                   (unsigned long long)res.cmd.slba, (unsigned long long)(res.cmd.slba + res.cmd.nlb),
                                   res.status, (unsigned long long)res.latencyUs);
            else
                ImGui::TextColored(col, "  NVMe %-20s cid %-3u  ->  status 0x%02X, %llu us",
                                   opName(res.cmd.opc), res.cid, res.status, (unsigned long long)res.latencyUs);
        }
        if (st.idleUs) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "  host idle -> background folding used %llu us",
                                          (unsigned long long)r.lastIdleSpent);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(st.explain);
        ImGui::PopTextWrapPos();
        if (r.done < total) ImGui::TextDisabled("Next: %s", sc.steps[size_t(r.done)].title);
        else ImGui::TextDisabled("End of this test case.");
    }

    ImGui::SeparatorText("The flash: every block and page");
    drawDrive(r);
    drawMappingAndStats(r);
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace scn
