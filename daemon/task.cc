/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "task.h"
#include "executor.h"

void Task::makeRunnable() {
    if (executor == nullptr) {
        throw std::logic_error("task need to be scheduled");
    }
    executor->makeRunnable(this);
}

void Task::makeRunnable(std::chrono::steady_clock::time_point time) {
    if (executor == nullptr) {
        throw std::logic_error("task need to be scheduled");
    }
    scheduledTime = time;
    executor->makeRunnable(*this, time);
}

Task::Status PeriodicTask::execute() {
    auto next_time = next();
    Status status = periodicExecute();
    if (status == Status::Continue) {
        makeRunnable(next_time);
    }
    return status;
}

std::chrono::steady_clock::time_point PeriodicTask::next() {
    if (scheduledTime == std::chrono::steady_clock::time_point()) {
        return std::chrono::steady_clock::now() + period;
    }
    // This reduces jitter / clock drift
    return scheduledTime + period;
}
