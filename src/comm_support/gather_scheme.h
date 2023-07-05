#pragma once
#include <cstdint>

namespace pimbridge {

class CommModule;

/*
 * Notice that one GatherScheme corresponds to one CommModule. 
 * Therefore, the information of a CommModule can also be maintained and updated by its GatherScheme.
 */

class GatherScheme {
public:
    enum Trigger {
        Whenever,
        Interval,
        OnDemand, 
        OnDemandOfAll, 
        DynamicInterval, 
        DynamicOnDemand, 
        TaskGenerationTrack
    };
    Trigger trigger;
    uint32_t packetSize;
    CommModule* commModule;

    GatherScheme(Trigger _trigger, uint32_t _packetSize) 
        : trigger(_trigger), packetSize(_packetSize) {}
    virtual bool shouldTrigger() = 0;
    virtual void update() {}

    void setCommModule(CommModule* _commModule);
protected:
    uint64_t bandwidth;
};

class WheneverGather : public GatherScheme {
public:
    WheneverGather(uint32_t _packetSize) 
        : GatherScheme(Trigger::Whenever, _packetSize) {}
    bool shouldTrigger() override {
        return true;
    }
};

class IntervalGather : public GatherScheme {
public:
    const uint32_t interval;

    IntervalGather(uint32_t _packetSize, uint32_t _interval) 
        : GatherScheme(Trigger::Interval, _packetSize), 
          interval(_interval) {}

    bool shouldTrigger() override;
};

class OnDemandGather : public GatherScheme {
public:
    uint32_t threshold;
    const uint32_t maxInterval;
    OnDemandGather(uint32_t _packetSize, uint32_t _threshold, uint32_t _maxInterval)
        : GatherScheme(Trigger::OnDemand, _packetSize), 
          threshold(_threshold), maxInterval(_maxInterval) {}

    bool shouldTrigger() override;
};

class OnDemandOfAllGather : public OnDemandGather {
public:
    OnDemandOfAllGather(uint32_t _packetSize, uint32_t _threshold, uint32_t _maxInterval)
        : OnDemandGather(_packetSize, _threshold, _maxInterval) {}
    
    bool shouldTrigger() override;
};

class DynamicGather : public GatherScheme {
public:
    DynamicGather(Trigger _trigger, uint32_t _packetSize)
        : GatherScheme(_trigger, _packetSize) {}
protected:
    const double highBwUtil = 1.0, midBwUtil = 0.5, lowBwUtil = 0.2;
    bool enoughTransferPacket();
    bool isDangerous();
    bool isSafe();
};

class DynamicOnDemandGather : public DynamicGather {
private:
    uint32_t highThreshold;
    uint32_t lowThreshold;
    const uint32_t maxInterval;
public:
    DynamicOnDemandGather(uint32_t _packetSize, uint32_t _highThreshold, 
        uint32_t _lowThreshold, uint32_t _maxInterval)
        : DynamicGather(Trigger::DynamicOnDemand, _packetSize), 
          highThreshold(_highThreshold), lowThreshold(_lowThreshold), 
          maxInterval(_maxInterval) {}
    
    bool shouldTrigger() override;
};

class DynamicIntervalGather : public DynamicGather {
public:
    uint32_t interval;
    DynamicIntervalGather(uint32_t _packetSize, uint32_t _initialInterval) 
        : DynamicGather(Trigger::DynamicInterval, _packetSize), 
          interval(_initialInterval) {}

    bool shouldTrigger() override;
};

class TaskGenerationTrackGather : public GatherScheme {
private:
    uint64_t lastTransferSize;
    double avgTaskGenBw;
public:
    TaskGenerationTrackGather(uint32_t _packetSize)
        : GatherScheme(Trigger::TaskGenerationTrack, _packetSize) {}
    bool shouldTrigger() override;
    void update() override;
};

}