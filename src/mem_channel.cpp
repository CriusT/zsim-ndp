#include "mem_channel.h"
#include "contention_sim.h"
#include "timing_event.h"
#include "zsim.h"

//#define DEBUG(args...) info(args)
#define DEBUG(args...)

// Weave phase event for one memory access request.
class MemChannelAccEvent : public TimingEvent {
    private:
        MemChannel* mem;
        Address addr;
        bool write;

    public:
        MemChannelAccEvent(MemChannel* _mem, bool _isWrite, Address _addr, int32_t domain,
                uint32_t preDelay, uint32_t postDelay)
            : TimingEvent(preDelay, postDelay, domain), mem(_mem), addr(_addr), write(_isWrite) {}

        Address getAddr() const { return addr; }
        bool isWrite() const { return write; }

        void simulate(uint64_t startCycle) {
            mem->acceptAccEvent(this, startCycle);
        }
};

// Weave phase event for memory system tick.
class MemChannelTickEvent : public TimingEvent, public GlobAlloc {
    private:
        MemChannel* const mem;
        enum State { IDLE, QUEUED, RUNNING, ANNULLED } state;

    public:
        MemChannelTickEvent(MemChannel* _mem, int32_t domain)
            : TimingEvent(0, 0, domain), mem(_mem)
        {
            setMinStartCycle(0);
            setRunning();
            state = IDLE;
            hold();
        }

        void parentDone(uint64_t startCycle) {
            panic("This is queued directly");
        }

        void simulate(uint64_t startCycle) {
            assert(state == QUEUED || state == ANNULLED);
            if (state == QUEUED) {
                state = RUNNING;
                uint64_t nextCycle = mem->tick(startCycle);
                if (nextCycle) {
                    assert(nextCycle >= startCycle);
                    requeue(nextCycle);
                    state = QUEUED;
                    return;
                }
            }
            // Recycle this event.
            state = IDLE;
            hold();
            mem->recycleTickEvent(this);
        }

        void enqueue(uint64_t cycle) {
            assert(state == IDLE);
            state = QUEUED;
            // Enqueue happens in weave phase
            requeue(cycle);
        }

        void annul() {
            assert(state == QUEUED);
            state = ANNULLED;
        }

        // Use glob mem
        using GlobAlloc::operator new;
        using GlobAlloc::operator delete;
};

MemChannel::MemChannel(MemChannelBackend* _be, const uint32_t _sysFreqMHz, const uint32_t _controllerSysDelay,
        const uint32_t _domain, const g_string& _name)
    : name(_name), domain(_domain), be(_be), controllerSysDelay(_controllerSysDelay), sysFreqKHz(_sysFreqMHz * 1000),
      tickCycle(-1uL), tickEvent(nullptr), freeTickEvent(nullptr)
{
    memFreqKHz = be->getMemFreqKHz();
    minRdDelay = memToSysCycle(be->getMinLatency(false));
    minWrDelay = memToSysCycle(be->getMinLatency(true));
    // Memory ctrl delay is pre the contention.
    preRdDelay = preWrDelay = controllerSysDelay;
    postRdDelay = minRdDelay - preRdDelay;
    postWrDelay = minWrDelay - preWrDelay;
}

void MemChannel::initStats(AggregateStat* parentStat) {
}

uint64_t MemChannel::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL) ? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }

    if (req.type == PUTS) {
        return req.cycle; // zero latency
    } else {
        bool isWrite = (req.type == PUTX);
        uint64_t respCycle = req.cycle + (isWrite ? minWrDelay : minRdDelay);
        if (zinfo->eventRecorders[req.srcId]) {
            MemChannelAccEvent* memEv = new (zinfo->eventRecorders[req.srcId])
                MemChannelAccEvent(this, isWrite, req.lineAddr, domain,
                        isWrite ? preWrDelay : preRdDelay,
                        isWrite ? postWrDelay : postRdDelay);
            memEv->setMinStartCycle(req.cycle);
            TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
            zinfo->eventRecorders[req.srcId]->pushRecord(tr);
        }
        return respCycle;
    }
}

void MemChannel::acceptAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle) {
    DEBUG("Accept AccEvent to 0x%lx at sysCycle %lu", ev->getAddr(), sysCycle);

    // Hold the access event for further scheduling/issuing. Release when responding.
    ev->hold();

    // Request queue overflow check.
    if (be->queueOverflow(ev->isWrite())) {
        overflowQueue.emplace_back(ev, sysCycle);
        return;
    }

    // Schedule.
    uint64_t memCycle = sysToMemCycle(sysCycle);
    DEBUG("Schedule new request (0x%lx,%c) at %lu", ev->getAddr(), ev->isWrite()?'w':'r', memCycle);
    uint64_t estTickCycle = schedule(ev, sysCycle, memCycle);

    // Adjust the tick cycle since the new request may be able to issued earlier.
    if (estTickCycle < tickCycle) {
        tickCycle = estTickCycle;

        // Annul the current tick event.
        if (tickEvent) tickEvent->annul();
        // Pick and set the new tick event.
        if (freeTickEvent != nullptr) {
            tickEvent = freeTickEvent;
            freeTickEvent = nullptr;
        } else {
            tickEvent = new MemChannelTickEvent(this, domain);
        }
        uint64_t enqSysCycle = matchingSysCycle(tickCycle);
        assert(enqSysCycle >= sysCycle);
        tickEvent->enqueue(enqSysCycle);
        DEBUG("Tick cycle shifted to %lu by new request", tickCycle);
    }
}

void MemChannel::respondAccEvent(MemChannelAccEvent* ev, uint64_t sysCycle) {
    bool isWrite = ev->isWrite();
    ev->release();
    DEBUG("Respond AccEvent to 0x%lx at sysCycle %lu", ev->getAddr(), sysCycle);
    // Exclude the post delay from the done cycle.
    ev->done(sysCycle - (isWrite ? postWrDelay : postRdDelay));
}

uint64_t MemChannel::tick(uint64_t sysCycle) {
    uint64_t memCycle = sysToMemCycle(sysCycle);
    assert_msg(memCycle == tickCycle, "Tick at wrong time %lu (should be %lu)", memCycle, tickCycle);
    DEBUG("Tick at %lu", memCycle);

    // Try issue and get the next tick cycle.
    uint64_t nextTickCycle = issue(memCycle);
    assert(nextTickCycle > memCycle);

    // Try to schedule an overflow request.
    if (!overflowQueue.empty()) {
        auto& oe = overflowQueue.front();
        MemChannelAccEvent* ev = oe.first;
        uint64_t startCycle = oe.second;
        if (!be->queueOverflow(ev->isWrite())) {
            DEBUG("Schedule overflow request (0x%lu,%c) at %lu", ev->getAddr(), ev->isWrite()?'w':'r', memCycle);
            uint64_t estTickCycle = schedule(ev, startCycle, memCycle);
            overflowQueue.pop_front();
            if (estTickCycle < nextTickCycle) {
                // The new scheduled request could make tick cycle earlier.
                nextTickCycle = estTickCycle;
                DEBUG("Tick event shifted to %lu by overflow request", nextTickCycle);
            }
        }
    }

    // Update tick cycle.
    tickCycle = nextTickCycle;
    if (tickCycle == -1uL) {
        tickEvent = nullptr;
        return 0; // 0 means no more event.
    }
    return std::max(sysCycle, matchingSysCycle(tickCycle)); // make sure no go back in time.
}

void MemChannel::recycleTickEvent(MemChannelTickEvent* tev) {
    assert(tev != tickEvent);
    if (freeTickEvent == nullptr) freeTickEvent = tev;
    else delete tev;
}

uint64_t MemChannel::schedule(MemChannelAccEvent* ev, uint64_t startCycle, uint64_t memCycle) {
    auto respEv = ev;
    if (ev->isWrite()) {
        // Respond write request immediately when scheduling.
        respondAccEvent(ev, memToSysCycle(memCycle));
        respEv = nullptr;
    }
    return be->enqueue(ev->getAddr(), ev->isWrite(), startCycle, memCycle, respEv);
}

uint64_t MemChannel::issue(uint64_t memCycle) {
    uint64_t minTickCycle = -1uL;
    MemChannelAccReq r;
    if (!be->dequeue(memCycle, &r, &minTickCycle)) {
        // If no request is ready, return minimum tick cycle as the next tick cycle.
        return minTickCycle;
    }
    auto req = &r;
    DEBUG("Issue AccReq to 0x%lx at %lu", req->addr, memCycle);

    // Process the request.
    uint64_t respCycle = be->process(req);
    uint64_t sysRespCycle = memToSysCycle(respCycle);

    // Respond read requests. Writes have been responded at schedule time.
    if (req->ev) {
        assert(!req->isWrite && req->ev != nullptr);
        respondAccEvent(req->ev, sysRespCycle);
        req->ev = nullptr;
    }

    return be->getTickCycleLowerBound();
}

