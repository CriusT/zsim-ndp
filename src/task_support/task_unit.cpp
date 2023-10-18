#include "task_support/task_unit.h"
#include "numa_map.h"
#include "log.h"
#include "zsim.h"
#include "core.h"
#include "task_support/task_timing_core.h"

using namespace task_support;

TaskUnit::TaskUnit(const std::string& _name, uint32_t _tuId, TaskUnitManager* _tum)
    : name(_name), taskUnitId(_tuId), tum(_tum), endTask(nullptr), 
      isFinished(false), minTimeStamp(0) {
    futex_init(&tuLock);
};

TaskUnit::~TaskUnit() {
    delete taskUnit1;
    delete taskUnit2;
}

// actually enter the task queue
void TaskUnit::taskEnqueue(TaskPtr t, int available) {
    DEBUG_TASK_BEHAVIOR_O("task enqueue: unit: %u, id: %lu, ts: %lu, addr: %lu", 
        taskUnitId, t->taskId, t->timeStamp, 
        zinfo->numaMap->getLbPageAddress(t->hint->dataPtr));
    uint64_t ts = t->timeStamp;
    futex_lock(&tuLock);
    if (ts == tum->getAllowedTimeStamp()) {
        if (this->curTaskUnit->isEmpty()) {
            checkTimeStampChange(t->timeStamp);
        }
        this->curTaskUnit->taskEnqueueKernel(t, available);
    } else {
        assert(t->timeStamp == tum->getAllowedTimeStamp() + 1);
        checkTimeStampChange(t->timeStamp);
        this->nxtTaskUnit->taskEnqueueKernel(t, available);
    }
    this->s_EnqueueTasks.inc(1);
    futex_unlock(&tuLock);
}

// endTask != actually empty: because maybe not ready task
// actually empty != isEmpty() : because maybe out packet 
// endTask && empty: 
TaskPtr TaskUnit::taskDequeue() {
    if (this->isFinished) {
        return endTask;
    }
    futex_lock(&tuLock); 
    TaskPtr ret = curTaskUnit->taskDequeueKernel();
    if (!ret->isEndTask) {
        DEBUG_TASK_BEHAVIOR_O("task dequeue: unit: %u, id: %lu, ts: %lu, addr: %lu", 
            taskUnitId, ret->taskId, ret->timeStamp, 
            zinfo->numaMap->getLbPageAddress(ret->hint->dataPtr));
        this->s_DequeueTasks.inc(1);
        futex_unlock(&tuLock); 
        return ret;
    } else if (!curTaskUnit->isEmpty()) {
        futex_unlock(&tuLock); 
        return ret;
    } else {
        if (!nxtTaskUnit->isEmpty()) {
            this->minTimeStamp = tum->getAllowedTimeStamp() + 1;
        } else {
            this->isFinished = true;
            this->minTimeStamp = -1;
            DEBUG_UNIT_BEHAVIOR_O("unit %u report finish", taskUnitId);
            tum->reportFinish(this->taskUnitId);
        }
        futex_unlock(&tuLock); 
        tum->reportChangeAllowedTimestamp(taskUnitId);
        return this->endTask;
    }
}

void TaskUnit::taskFinish(TaskPtr t) {
    this->s_FinishTasks.atomicInc(1);
}

void TaskUnit::beginNewTimeStamp(uint64_t newTs) {
    futex_lock(&tuLock);
    if (newTs == 1) {
        assert(this->minTimeStamp == 0);
        this->minTimeStamp = newTs;
    } else {
        assert((int)this->minTimeStamp == -1 || this->minTimeStamp == newTs);
    }
    if (useQ1) {
        curTaskUnit = taskUnit2;
        nxtTaskUnit = taskUnit1;
        useQ1 = false;
    } else {
        curTaskUnit = taskUnit1;
        nxtTaskUnit = taskUnit2;
        useQ1 = true;
    }
    curTaskUnit->setCurTs(newTs);
    nxtTaskUnit->setCurTs(newTs+1);
    futex_unlock(&tuLock);
}



void TaskUnit::checkTimeStampChange(uint64_t newTs) {
    DEBUG_UNIT_BEHAVIOR_O("task unit %u checkChange, new ts: %lu, origin: %d",
        taskUnitId, newTs, (int)this->minTimeStamp);
    if ((int)this->minTimeStamp == -1) {;
        this->minTimeStamp = newTs;
        this->tum->reportChangeAllowedTimestamp(this->taskUnitId);
        if (this->isFinished) {
            DEBUG_UNIT_BEHAVIOR_O("task unit %u restart, new ts:  %lu", 
                taskUnitId, newTs);
            this->isFinished = false;
            tum->reportRestart();
        }
    } else if (this->minTimeStamp > newTs) {
        // assert_msg(this->minTimeStamp == newTs + 1 && newTs == tum->getAllowedTimeStamp() &&
        //     this->curTaskQueue->empty(), "unit: %u, queue size: %lu", taskUnitId, this->curTaskQueue->size());
        // info("task unit %d change stamp to %d because schedule", taskUnitId, this->minTimeStamp);
        this->minTimeStamp = newTs;
        tum->reportChangeAllowedTimestamp(this->taskUnitId);
    } 
}

void TaskUnit::setEndTask(TaskPtr t) { 
    this->endTask = t;
    this->taskUnit1->endTask = t;
    this->taskUnit2->endTask = t;  
}

void TaskUnit::initStats(AggregateStat* parentStat) {
    AggregateStat* tuStat = new AggregateStat();
    tuStat->init(name.c_str(), "Task unit stats");

    s_EnqueueTasks.init("enqueueTasks", "Number of enqueued tasks");
    tuStat->append(&s_EnqueueTasks);
    s_DequeueTasks.init("dequeueTasks", "Number of dequeued tasks");
    tuStat->append(&s_DequeueTasks);
    s_FinishTasks.init("finishTasks", "Number of finish tasks");
    tuStat->append(&s_FinishTasks);

    parentStat->append(tuStat);
}

void TaskUnit::computeExecuteSpeed() {
    uint64_t numTask = this->s_FinishTasks.get();
    uint64_t numCycle = ((TaskTimingCore*)zinfo->cores[taskUnitId])->getCurWorkCycle();
    this->executeSpeed = (double)numTask / numCycle;
}