#include "mem_channel_backend_ddr.h"
#include <cstring>          // for strcmp
#include "bithacks.h"
#include "config.h"         // for Tokenize

// TODO: implement
// adaptive close/open
// power-down: XP
// power model: IDD

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

MemChannelBackendDDR::MemChannelBackendDDR(const g_string& _name,
        uint32_t ranksPerChannel, uint32_t banksPerRank, const char* _pagePolicy,
        uint32_t pageSizeBytes, uint32_t burstCount, uint32_t deviceIOBits, uint32_t channelWidthBits,
        uint32_t memFreqMHz, const Timing& _t,
        const char* addrMapping, uint32_t _queueDepth, uint32_t _maxRowHits)
    : name(_name), rankCount(ranksPerChannel), bankCount(banksPerRank),
      pageSize(pageSizeBytes), burstSize(burstCount*deviceIOBits),
      devicesPerRank(channelWidthBits/deviceIOBits), freqKHz(memFreqMHz*1000), t(_t),
      queueDepth(_queueDepth), maxRowHits(_maxRowHits) {

    info("%s: %u ranks x %u banks.", name.c_str(), rankCount, bankCount);
    info("%s: page size %u bytes, %u devices per rank, burst %u bits from each device.",
            name.c_str(), pageSize, devicesPerRank, burstSize);
    info("%s: tBL = %u, tCAS = %u, tCCD = %u, tCWD = %u, tRAS = %u, tRCD = %u",
            name.c_str(), t.BL, t.CAS, t.CCD, t.CWD, t.RAS, t.RCD);
    info("%s: tRP = %u, tRRD = %u, tRTP = %u, tWR = %u, tWTR = %u",
            name.c_str(), t.RP, t.RRD, t.RTP, t.WR, t.WTR);
    info("%s: tRFC = %u, tREFI = %u, tRPab = %u, tFAW = %u, tRTRS = %u, tCMD = %u, tXP = %u",
            name.c_str(), t.RFC, t.REFI, t.RPab, t.FAW, t.RTRS, t.CMD, t.XP);

    assert_msg(channelWidthBits % deviceIOBits == 0,
            "Channel width (%u given) must be multiple of device IO width (%u given).",
            channelWidthBits, deviceIOBits);

    // Page policy.
    if (strcmp(_pagePolicy, "open") == 0) {
        pagePolicy = DDRPagePolicy::OPEN;
    } else if (strcmp(_pagePolicy, "close") == 0) {
        pagePolicy = DDRPagePolicy::CLOSE;
    } else {
        panic("Unrecognized page policy %s", _pagePolicy);
    }

    // Banks.
    banks.clear();
    for (uint32_t r = 0; r < rankCount; r++) {
        auto aw = new DDRActWindow(4);
        for (uint32_t b = 0; b < bankCount; b++) {
            banks.emplace_back(aw);
        }
    }
    assert(banks.size() == rankCount * bankCount);

    // Address mapping.

    assert_msg(isPow2(rankCount), "Only support power-of-2 ranks per channel, %u given.", rankCount);
    assert_msg(isPow2(bankCount), "Only support power-of-2 banks per rank, %u given.", bankCount);
    // One column has the size of deviceIO. LSB bits of column address are for burst. Here we only account for the high
    // bits of column which are in line address, since the low bits are hidden in lineSize.
    uint32_t colHCount = pageSize*8 / burstSize;
    assert_msg(isPow2(colHCount), "Only support power-of-2 column bursts per row, %u given "
            "(page %u Bytes, device IO %u bits, %u bursts).", colHCount, pageSize, deviceIOBits, burstCount);

    uint32_t rankBitCount = ilog2(rankCount);
    uint32_t bankBitCount = ilog2(bankCount);
    uint32_t colHBitCount = ilog2(colHCount);

    std::vector<std::string> tokens;
    Tokenize(addrMapping, tokens, ":");
    if (tokens.size() != 3 && !(tokens.size() == 4 && tokens[0] == "row")) {
        panic("Wrong address mapping %s: row default at MSB, must contain bank, rank, and col.", addrMapping);
    }

    rankMask = bankMask = colHMask = 0;
    uint32_t startBit = 0;
    auto computeShiftMask = [&startBit, &addrMapping](const std::string& t, const uint32_t bitCount,
            uint32_t& shift, uint32_t& mask) {
        if (mask) panic("Repeated field %s in address mapping %s.", t.c_str(), addrMapping);
        shift = startBit;
        mask = (1 << bitCount) - 1;
        startBit += bitCount;
    };

    // Reverse order, from LSB to MSB.
    for (auto it = tokens.crbegin(); it != tokens.crend(); it++) {
        auto t = *it;
        if (t == "row") {}
        else if (t == "rank") computeShiftMask(t, rankBitCount, rankShift, rankMask);
        else if (t == "bank") computeShiftMask(t, bankBitCount, bankShift, bankMask);
        else if (t == "col")  computeShiftMask(t, colHBitCount, colHShift, colHMask);
        else panic("Invalid field %s in address mapping %s.", t.c_str(), addrMapping);
    }
    rowShift = startBit;
    rowMask = -1u;

    info("%s: Address mapping %s row %d:%u rank %u:%u bank %u:%u col %u:%u",
            name.c_str(), addrMapping, 63, rowShift,
            ilog2(rankMask << rankShift), rankMask ? rankShift : 0,
            ilog2(bankMask << bankShift), bankShift ? bankShift : 0,
            ilog2(colHMask << colHShift), colHShift ? colHShift : 0);

    // Schedule and issue.

    reqQueueRd.init(queueDepth);
    reqQueueWr.init(queueDepth);

    prioListsRd.resize(rankCount * bankCount);
    prioListsWr.resize(rankCount * bankCount);

    issueMode = IssueMode::UNKNOWN;
    minBurstCycle = 0;
    lastIsWrite = false;
}

void MemChannelBackendDDR::initStats(AggregateStat* parentStat) {
    // TODO: implement
}

uint64_t MemChannelBackendDDR::enqueue(const Address& addr, const bool isWrite,
        uint64_t startCycle, uint64_t memCycle, MemChannelAccEvent* respEv) {
    // Allocate request.
    auto req = reqQueue(isWrite).alloc();
    req->addr = addr;
    req->isWrite = isWrite;
    req->startCycle = startCycle;
    req->schedCycle = memCycle;
    req->ev = respEv;

    req->loc = mapAddress(addr);

    DEBUG("%s DDR enqueue: %s queue depth %lu", name.c_str(), isWrite ? "write" : "read", reqQueue(isWrite).size());

    // Assign priority.
    assignPriority(req);

    if (req->hasHighestPriority()) {
        return requestHandler(req);
    }
    else return -1uL;
}

bool MemChannelBackendDDR::dequeue(uint64_t memCycle, MemChannelAccReq** req, uint64_t* minTickCycle) {
    // Update read/write issue mode.
    // Without a successful issue, the issue mode decision should not change. This is because the unsuccessful
    // issue trial is a result of the weaving timing model. The decision (issue a read or write) has been made
    // and will be carried out for sure.
    if (issueMode == IssueMode::UNKNOWN) {
        if (reqQueueRd.empty() || reqQueueWr.size() > queueDepth * 3/4 ||
                (lastIsWrite && reqQueueWr.size() > queueDepth * 1/4)) {
            issueMode = IssueMode::WR_QUEUE;
        } else {
            issueMode = IssueMode::RD_QUEUE;
        }
    }
    bool issueWrite = (issueMode == IssueMode::WR_QUEUE);

    // Scan the requests with the highest priority in each list in chronological order.
    // Take the first request (the oldest) that is ready to be issued.
    DDRAccReq* r = nullptr;
    uint64_t mtc = -1uL;
    auto ir = reqQueue(issueWrite).begin();
    for (; ir != reqQueue(issueWrite).end(); ir.inc()) {
        if (!(*ir)->hasHighestPriority())
            continue;

        uint64_t c = requestHandler(*ir);
        if (c <= memCycle) {
            r = *ir;
            break;
        }
        mtc = std::min(mtc, c);
    }

    // If no request is ready, set min tick cycle.
    if (!r) {
        *minTickCycle = mtc;
        return false;
    }

    // Remove priority assignment.
    cancelPriority(r);

    *req = new DDRAccReq(*r);
    reqQueue(issueWrite).remove(ir);
    DEBUG("%s DDR dequeue: %s queue depth %lu", name.c_str(), issueWrite ? "write" : "read", reqQueue(issueWrite).size());

    return true;
}

uint64_t MemChannelBackendDDR::process(const MemChannelAccReq* req) {
    const DDRAccReq* ddrReq = dynamic_cast<const DDRAccReq*>(req);
    assert(ddrReq);

    uint64_t burstCycle = requestHandler(ddrReq, true);
    uint64_t respCycle = burstCycle + t.BL;

    lastIsWrite = req->isWrite;
    lastRankIdx = ddrReq->loc.rank;
    minBurstCycle = respCycle;

    // Reset issue mode.
    issueMode = IssueMode::UNKNOWN;

    return respCycle;
}

bool MemChannelBackendDDR::queueOverflow(const bool isWrite) const {
    return reqQueue(isWrite).full();
}

bool MemChannelBackendDDR::queueEmpty(const bool isWrite) const {
    return reqQueue(isWrite).empty();
}

void MemChannelBackendDDR::periodicalProcess(uint64_t memCycle) {
    refresh(memCycle);
}


uint64_t MemChannelBackendDDR::requestHandler(const DDRAccReq* req, bool update) {
    const auto& loc = req->loc;
    const auto& isWrite = req->isWrite;
    const auto& schedCycle = req->schedCycle;
    auto& bank = banks[loc.rank * bankCount + loc.bank];

    // Bank PRE.
    uint64_t preCycle = 0;
    if (!bank.open) {
        // Bank is closed.
        preCycle = bank.minPRECycle;
    } else if (loc.row != bank.row) {
        // A conflict row is open.
        preCycle = std::max(bank.minPRECycle, schedCycle);
        if (update) {
            bank.recordPRE(preCycle);
        }
    }

    // Bank ACT.
    uint64_t actCycle = 0;
    if (!bank.open || loc.row != bank.row) {
        // Need to open the row.
        actCycle = calcACTCycle(bank, schedCycle, preCycle);
        if (update) {
            bank.recordACT(actCycle, loc.row);
        }
    }
    if (update) {
        assert(bank.open && loc.row == bank.row);
    }

    // RD/WR.
    uint64_t rwCycle = calcRWCycle(bank, schedCycle, actCycle, isWrite, loc.rank);
    if (update) {
        bank.recordRW(rwCycle);
    }

    // Burst data transfer.
    uint64_t burstCycle = calcBurstCycle(bank, rwCycle, isWrite);
    assert(burstCycle >= minBurstCycle);

    // (future) next PRE.
    if (update) {
        // TODO: adaptive close
        uint64_t preCycle = updatePRECycle(bank, rwCycle, isWrite);
        if (pagePolicy == DDRPagePolicy::CLOSE) {
            bank.recordPRE(preCycle);
        }
    }
    DEBUG("%s DDR handler: 0x%lx@%lu -- PRE %lu ACT %lu RW %lu BL %lu", name.c_str(), req->addr, req->schedCycle,
            preCycle, actCycle, rwCycle, burstCycle);

    return burstCycle;
}

void MemChannelBackendDDR::refresh(uint64_t memCycle) {
    uint64_t minREFCycle = memCycle;
    for (const auto& b : banks) {
        // Issue PRE to close all banks before REF.
        minREFCycle = std::max(minREFCycle, b.minPRECycle + t.RPab);
    }

    uint64_t finREFCycle = minREFCycle + t.RFC;
    assert(t.RFC >= t.RP);
    for (auto& b : banks) {
        // Banks are closed after REF.
        // ACT is able to issue right after refresh done, equiv. to PRE tRP earlier.
        b.open = false;
        b.minPRECycle = finREFCycle - t.RP;
    }
}

void MemChannelBackendDDR::assignPriority(DDRAccReq* req) {
    auto& pl = prioLists(req->isWrite)[req->loc.rank * bankCount + req->loc.bank];
    auto& bank = banks[req->loc.rank * bankCount + req->loc.bank];

    // FCFS/FR-FCFS scheduling.
    // Tune maxRowHits to switch between the two scheduling schemes. maxRowHits == 0
    // means FCFS, and maxRowHits == max means FR-FCFS.
    auto m = pl.back();
    while (m) {
        if (m->loc.row == req->loc.row) {
            // Enqueue request after the last same-row request.
            if (m->rowHitSeq + 1 < maxRowHits) {
                req->rowHitSeq = m->rowHitSeq + 1;
                pl.insertAfter(m, req);
            } else {
                // If exceeding the max number of row hits, enqueue at the end.
                req->rowHitSeq = 0;
                pl.push_back(req);
            }
            break;
        }
        m = m->prev;
    }

    if (!m) {
        if (bank.open && bank.row == req->loc.row && bank.rowHitSeq + 1 < maxRowHits) {
            // The new request row hits the current open row in the bank, bypass to the front.
            req->rowHitSeq = bank.rowHitSeq + 1;
            pl.push_front(req);
        } else {
            // Enqueue at the end.
            req->rowHitSeq = 0;
            pl.push_back(req);
        }
    }
}

void MemChannelBackendDDR::cancelPriority(DDRAccReq* req) {
    auto& pl = prioLists(req->isWrite)[req->loc.rank * bankCount + req->loc.bank];
    assert(req == pl.front());
    pl.pop_front();
}

uint64_t MemChannelBackendDDR::calcACTCycle(const Bank& bank, uint64_t schedCycle, uint64_t preCycle) const {
    // Constraints: tRP, tRRD, tFAW, tCMD.
    return maxN<uint64_t>(
            schedCycle,
            preCycle + t.RP,
            bank.lastACTCycle + t.RRD,
            bank.actWindow->minACTCycle() + t.FAW,
            preCycle + t.CMD);
}

uint64_t MemChannelBackendDDR::calcRWCycle(const Bank& bank, uint64_t schedCycle, uint64_t actCycle,
        bool isWrite, uint32_t rankIdx) const {
    // Constraints: tRCD, tWTR, tCCD, bus contention, tRTRS, tCMD.
    int64_t dataOnBus = minBurstCycle;
    if (rankIdx != lastRankIdx) {
        // Switch rank.
        dataOnBus += t.RTRS;
    }
    return maxN<uint64_t>(
            schedCycle,
            actCycle + t.RCD,
            (lastIsWrite && !isWrite) ? minBurstCycle + t.WTR : 0,
            bank.lastRWCycle + t.CCD,
            std::max<int64_t>(0, dataOnBus - (isWrite ? std::max(t.CWD, t.CAS) : t.CAS)), // avoid underflow
            actCycle + t.CMD);
}

uint64_t MemChannelBackendDDR::calcBurstCycle(const Bank& bank, uint64_t rwCycle, bool isWrite) const {
    // Constraints: tCAS, tCWD.
    return rwCycle + (isWrite ? std::max(t.CWD, t.CAS) : t.CAS);
}

uint64_t MemChannelBackendDDR::updatePRECycle(Bank& bank, uint64_t rwCycle, bool isWrite) {
    assert(bank.open);
    // Constraints: tRAS, tWR, tRTP, tCMD.
    bank.minPRECycle = maxN<uint64_t>(
            bank.minPRECycle,
            bank.lastACTCycle + t.RAS,
            isWrite ? minBurstCycle + t.WR : rwCycle + t.RTP,
            rwCycle + t.CMD);
    return bank.minPRECycle;
}

