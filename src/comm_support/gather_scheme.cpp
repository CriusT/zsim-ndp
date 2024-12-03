#include <cstdint>
#include "zsim.h"
#include "comm_support/gather_scheme.h"

using namespace pimbridge;

void GatherScheme::setCommModule(CommModule* _commModule) {
    this->commModule = _commModule; 
    this->bandwidth = (commModule->childEndId - commModule->childBeginId) * packetSize;
}

bool IntervalGather::shouldTrigger() {
    return (zinfo->numPhases % this->interval == 0);
}

bool OnDemandGather::shouldTrigger() {
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        if (commModule->childTransferSize[i-commModule->childBeginId] >= this->threshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastGatherPhase() >= 
            this->maxInterval) {
        return true;
    }
    return false;
}

bool OnDemandOfAllGather::shouldTrigger() {
    uint64_t allPackets = 0;
    for (uint32_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        allPackets += commModule->childTransferSize[i-commModule->childBeginId];
        if (allPackets >= this->threshold) {
            return true;
        }
    }
    if (zinfo->numPhases - commModule->getLastGatherPhase() >= 
            this->maxInterval) {
        return true;
    }
    return false;
}

bool DynamicGather::enoughTransferPacket() {
    uint64_t curTransferSize = 0;
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        uint64_t curSize = commModule->childTransferSize[i-commModule->childBeginId];
        if (curSize > this->packetSize) {
            return true;
        }
        // curTransferSize += curSize >= this->packetSize ? packetSize : curSize;
    }
    return false;
    // return curTransferSize >= highBwUtil * bandwidth;
}

bool DynamicGather::isSafe() {
    for (uint32_t i = 0; i < commModule->bankEndId - commModule->bankBeginId; ++i) {
        uint64_t curSize = commModule->bankQueueReadyLength[i];
        if (curSize <= zinfo->commModuleManager->getExecuteSpeedPerPhase() * 2) {
            return false;
        }
    }
    return true;
}

bool DynamicIntervalGather::shouldTrigger() {
    if (isSafe()) {
        return this->enoughTransferPacket();
    }
    return (zinfo->numPhases - commModule->getLastGatherPhase() >= this->interval);
}

bool TaskGenerationTrackGather::shouldTrigger() {
    // Triggered by high bandwidth utilization
    uint64_t curTransferSize = 0;
    for (size_t i = commModule->childBeginId; i < commModule->childEndId; ++i) {
        // uint64_t curSize = zinfo->commModules[commModule->level-1][i]
        //     ->getParentPackets()->getSize();
        uint64_t curSize = commModule->childTransferSize[i-commModule->childBeginId];
        curTransferSize += curSize >= this->packetSize ? packetSize : curSize;
    }
    assert(curTransferSize <= bandwidth);
    if (curTransferSize == bandwidth) {
        return true;
    }
    // Triggered by low task generation bandwidth
    assert(curTransferSize >= lastTransferSize);
    uint64_t deltaSize = curTransferSize - lastTransferSize;
    if (deltaSize < 0.5 * avgTaskGenBw) {
        return true;
    }
    return false;
}

void TaskGenerationTrackGather::update() {
    // TBY TODO
}

