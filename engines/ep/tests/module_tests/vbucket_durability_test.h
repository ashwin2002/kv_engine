/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
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

#pragma once

#include "vbucket_test.h"

class CheckpointManager;
class MockDurabilityMonitor;

/*
 * VBucket unit tests related to durability.
 */
class VBucketDurabilityTest : public VBucketTest {
public:
    void SetUp() override;

    void TearDown() override {
        VBucketTest::TearDown();
    }

protected:
    /**
     * Store the given Sync mutations into VBucket
     *
     * @param seqnos the mutations to be added
     * @return the number of stored SyncWrites
     */
    size_t storeSyncWrites(const std::vector<int64_t>& seqnos);

    /**
     * Tests the baseline progress of a set of SyncWrites in Vbucket:
     * 1) mutations added to VBucket
     * 2) mutations in state "pending" in both HashTable and CheckpointManager
     * 3) VBucket receives a SeqnoAck that satisfies the DurReqs for all SWs
     * 4) mutations in state "committed" in both HashTable and CheckpointManager
     *
     * @param seqnos the set of mutations to test
     */
    void testSyncWrites(const std::vector<int64_t>& seqnos);

    // All owned by VBucket
    HashTable* ht;
    CheckpointManager* ckptMgr;
    MockDurabilityMonitor* monitor;

    // @todo: This is hard-coded in DcpProducer::seqno_acknowledged. Remove
    //     when we switch to use the real name of the Consumer.
    const std::string replica = "replica";
};
