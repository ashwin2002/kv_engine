/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

/*
 * Unit test for stats
 */

#include "stats_test.h"
#include "dcp/dcpconnmap.h"
#include "dcp/producer.h"
#include "dcp/stream.h"
#include "evp_store_single_threaded_test.h"
#include "tasks.h"
#include "test_helpers.h"
#include "tests/mock/mock_synchronous_ep_engine.h"
#include "thread_gate.h"
#include "tracing/trace_helpers.h"

#include <gmock/gmock.h>
#include <memcached/server_cookie_iface.h>
#include <programs/engine_testapp/mock_server.h>

#include <functional>
#include <thread>

void StatTest::SetUp() {
    SingleThreadedEPBucketTest::SetUp();
    store->setVBucketState(vbid, vbucket_state_active);
}

std::map<std::string, std::string> StatTest::get_stat(const char* statkey) {
    // Define a lambda to use as the ADD_STAT callback. Note we cannot use
    // a capture for the statistics map (as it's a C-style callback), so
    // instead pass via the cookie.
    struct StatMap : cb::tracing::Traceable {
        std::map<std::string, std::string> map;
    };
    StatMap stats;
    auto add_stats = [](const char* key,
                        const uint16_t klen,
                        const char* val,
                        const uint32_t vlen,
                        gsl::not_null<const void*> cookie) {
        auto* stats =
                reinterpret_cast<StatMap*>(const_cast<void*>(cookie.get()));
        std::string k(key, klen);
        std::string v(val, vlen);
        stats->map[k] = v;
    };

    EXPECT_EQ(
            ENGINE_SUCCESS,
            engine->get_stats(&stats,
                              {statkey, statkey == NULL ? 0 : strlen(statkey)},
                              add_stats))
            << "Failed to get stats.";

    return stats.map;
}

class DatatypeStatTest : public StatTest,
                         public ::testing::WithParamInterface<std::string> {
protected:
    void SetUp() override {
        config_string += std::string{"item_eviction_policy="} + GetParam();
        StatTest::SetUp();
    }
};

TEST_F(StatTest, vbucket_seqno_stats_test) {
    using namespace testing;
    const std::string vbucket = "vb_" + std::to_string(vbid.get());
    auto vals = get_stat("vbucket-seqno");

    EXPECT_THAT(vals, UnorderedElementsAre(
            Key(vbucket + ":uuid"),
            Pair(vbucket + ":high_seqno", "0"),
            Pair(vbucket + ":abs_high_seqno", "0"),
            Pair(vbucket + ":last_persisted_seqno", "0"),
            Pair(vbucket + ":purge_seqno", "0"),
            Pair(vbucket + ":last_persisted_snap_start", "0"),
            Pair(vbucket + ":last_persisted_snap_end", "0")));
}

// Test that if we request takeover stats for stream that does not exist we
// return does_not_exist.
TEST_F(StatTest, vbucket_takeover_stats_no_stream) {
    // Create a new Dcp producer, reserving its cookie.
    get_mock_server_api()->cookie->reserve(cookie);
    engine->getDcpConnMap().newProducer(cookie,
                                        "test_producer",
                                        /*flags*/ 0);

    const std::string stat =
            "dcp-vbtakeover " + std::to_string(vbid.get()) + " test_producer";
    ;
    auto vals = get_stat(stat.c_str());
    EXPECT_EQ("does_not_exist", vals["status"]);
    EXPECT_EQ(0, std::stoi(vals["estimate"]));
    EXPECT_EQ(0, std::stoi(vals["backfillRemaining"]));
}

// Test that if we request takeover stats for stream that is not active we
// return does_not_exist.
TEST_F(StatTest, vbucket_takeover_stats_stream_not_active) {
    // Create a new Dcp producer, reserving its cookie.
    get_mock_server_api()->cookie->reserve(cookie);
    DcpProducer* producer = engine->getDcpConnMap().newProducer(
            cookie,
            "test_producer",
            cb::mcbp::request::DcpOpenPayload::Notifier);

    uint64_t rollbackSeqno;
    const std::string stat = "dcp-vbtakeover " + std::to_string(vbid.get()) +
            " test_producer";;
    ASSERT_EQ(ENGINE_SUCCESS,
              producer->streamRequest(/*flags*/ 0,
                                      /*opaque*/ 0,
                                      /*vbucket*/ vbid,
                                      /*start_seqno*/ 0,
                                      /*end_seqno*/ 0,
                                      /*vb_uuid*/ 0,
                                      /*snap_start*/ 0,
                                      /*snap_end*/ 0,
                                      &rollbackSeqno,
                                      fakeDcpAddFailoverLog,
                                      {}));

    // Ensure its a notifier connection - this means that streams requested will
    // not be active
    ASSERT_EQ("notifier", std::string(producer->getType()));
    auto vals = get_stat(stat.c_str());
    EXPECT_EQ("does_not_exist", vals["status"]);
    EXPECT_EQ(0, std::stoi(vals["estimate"]));
    EXPECT_EQ(0, std::stoi(vals["backfillRemaining"]));
    producer->closeStream(/*opaque*/ 0, vbid);
}


TEST_P(DatatypeStatTest, datatypesInitiallyZero) {
    // Check that the datatype stats initialise to 0
    auto vals = get_stat(nullptr);
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_snappy"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_snappy,json"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_snappy,xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json,xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_raw"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_snappy,json,xattr"]));

    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_snappy"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_snappy,json"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_snappy,xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_json"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_json,xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_raw"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_replica_datatype_snappy,json,xattr"]));
}

void setDatatypeItem(KVBucket* store,
                     const void* cookie,
                     protocol_binary_datatype_t datatype,
                     std::string name, std::string val = "[0]") {
    Item item(make_item(
            Vbid(0), {name, DocKeyEncodesCollectionId::No}, val, 0, datatype));
    store->set(item, cookie);
}

TEST_P(DatatypeStatTest, datatypeJsonToXattr) {
    setDatatypeItem(store, cookie, PROTOCOL_BINARY_DATATYPE_JSON, "jsonDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json"]));

    // Check that updating an items datatype works
    setDatatypeItem(store, cookie, PROTOCOL_BINARY_DATATYPE_XATTR, "jsonDoc");
    vals = get_stat(nullptr);

    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_xattr"]));
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json"]));
}

TEST_P(DatatypeStatTest, datatypeRawStatTest) {
    setDatatypeItem(store, cookie, 0, "rawDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_raw"]));
}

TEST_P(DatatypeStatTest, datatypeXattrStatTest) {
    setDatatypeItem(store, cookie, PROTOCOL_BINARY_DATATYPE_XATTR, "xattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_xattr"]));
    // Update the same key with a different value. The datatype stat should
    // stay the same
    setDatatypeItem(store, cookie, PROTOCOL_BINARY_DATATYPE_XATTR,
                    "xattrDoc", "[2]");
    vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_xattr"]));
}

TEST_P(DatatypeStatTest, datatypeCompressedStatTest) {
    setDatatypeItem(store,
                    cookie,
                    PROTOCOL_BINARY_DATATYPE_SNAPPY,
                    "compressedDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_snappy"]));
}

TEST_P(DatatypeStatTest, datatypeCompressedJson) {
    setDatatypeItem(
            store,
            cookie,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_SNAPPY,
            "jsonCompressedDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_snappy,json"]));
}

TEST_P(DatatypeStatTest, datatypeCompressedXattr) {
    setDatatypeItem(store,
                    cookie,
                    PROTOCOL_BINARY_DATATYPE_XATTR |
                            PROTOCOL_BINARY_DATATYPE_SNAPPY,
                    "xattrCompressedDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_snappy,xattr"]));
}

TEST_P(DatatypeStatTest, datatypeJsonXattr) {
    setDatatypeItem(
            store,
            cookie,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR,
            "jsonXattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
}

TEST_P(DatatypeStatTest, datatypeDeletion) {
    setDatatypeItem(
            store,
            cookie,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR,
            "jsonXattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
    uint64_t cas = 0;
    mutation_descr_t mutation_descr;
    store->deleteItem({"jsonXattrDoc", DocKeyEncodesCollectionId::No},
                      cas,
                      Vbid(0),
                      cookie,
                      nullptr,
                      mutation_descr);
    vals = get_stat(nullptr);
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json,xattr"]));
}

TEST_P(DatatypeStatTest, datatypeCompressedJsonXattr) {
    setDatatypeItem(store,
                    cookie,
                    PROTOCOL_BINARY_DATATYPE_JSON |
                            PROTOCOL_BINARY_DATATYPE_SNAPPY |
                            PROTOCOL_BINARY_DATATYPE_XATTR,
                    "jsonCompressedXattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_snappy,json,xattr"]));
}

TEST_P(DatatypeStatTest, datatypeExpireItem) {
    Item item(make_item(Vbid(0),
                        {"expiryDoc", DocKeyEncodesCollectionId::No},
                        "[0]",
                        1,
                        PROTOCOL_BINARY_DATATYPE_JSON));
    store->set(item, cookie);
    store->get({"expiryDoc", DocKeyEncodesCollectionId::No},
               Vbid(0),
               cookie,
               NONE);
    auto vals = get_stat(nullptr);

    //Should be 0, becuase the doc should have expired
    EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json"]));
}


TEST_P(DatatypeStatTest, datatypeEviction) {
    const DocKey key = {"jsonXattrDoc", DocKeyEncodesCollectionId::No};
    Vbid vbid = Vbid(0);
    setDatatypeItem(
            store,
            cookie,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR,
            "jsonXattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
    getEPBucket().flushVBucket(vbid);
    const char* msg;
    store->evictKey(key, vbid, &msg);
    vals = get_stat(nullptr);
    if (GetParam() == "value_only"){
        // Should still be 1 as only value is evicted
        EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
    } else {
        // Should be 0 as everything is evicted
        EXPECT_EQ(0, std::stoi(vals["ep_active_datatype_json,xattr"]));
    }

    store->get(key, vbid, cookie, QUEUE_BG_FETCH);
    if (GetParam() == "full_eviction") {
        // Run the bgfetch to restore the item from disk
        runBGFetcherTask();
    }
    vals = get_stat(nullptr);
    // The item should be restored to memory, hence added back to the stats
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
}

TEST_P(DatatypeStatTest, MB23892) {
    // This test checks that updating a document with a different datatype is
    // safe to do after an eviction (where the blob is now null)
    const DocKey key = {"jsonXattrDoc", DocKeyEncodesCollectionId::No};
    Vbid vbid = Vbid(0);
    setDatatypeItem(
            store,
            cookie,
            PROTOCOL_BINARY_DATATYPE_JSON | PROTOCOL_BINARY_DATATYPE_XATTR,
            "jsonXattrDoc");
    auto vals = get_stat(nullptr);
    EXPECT_EQ(1, std::stoi(vals["ep_active_datatype_json,xattr"]));
    getEPBucket().flushVBucket(vbid);
    const char* msg;
    store->evictKey(key, vbid, &msg);
    getEPBucket().flushVBucket(vbid);
    setDatatypeItem(store, cookie, PROTOCOL_BINARY_DATATYPE_JSON, "jsonXattrDoc", "[1]");
}

INSTANTIATE_TEST_CASE_P(FullAndValueEviction, DatatypeStatTest,
                        ::testing::Values("value_only", "full_eviction"), []
                                (const ::testing::TestParamInfo<std::string>&
                                info) {return info.param;});

class TestEpStat : public EPStats {
public:
    void setMemUsedMergeThreshold(int64_t value) {
        memUsedMergeThreshold = value;
    }
};

class EpStatsTest : public ::testing::Test {
public:
};

TEST_F(EpStatsTest, memoryNegative) {
    TestEpStat stats;
    stats.memoryTrackerEnabled = true;

    stats.memDeallocated(100);
    EXPECT_EQ(0, stats.getEstimatedTotalMemoryUsed());
    EXPECT_EQ(0, stats.getPreciseTotalMemoryUsed());
    // getPrecise will have merged, check we really have negative
    EXPECT_EQ(-100, stats.estimatedTotalMemory->load());
}

TEST_F(EpStatsTest, memoryNegativeUntracked) {
    TestEpStat stats;
    stats.memoryTrackerEnabled = false;

    stats.coreLocal.get()->memOverhead.fetch_sub(100);
    ASSERT_EQ(-100, stats.coreLocal.get()->memOverhead.load());

    EXPECT_EQ(0, stats.getEstimatedTotalMemoryUsed());
}

// Create n threads who all allocate the same amount of memory in very different
// orders
TEST_F(EpStatsTest, memoryAllocated) {
    TestEpStat stats;
    stats.memoryTrackerEnabled = true;
    stats.setMemUsedMergeThreshold(100);

    const int nThreads = 4;
    ThreadGate tg(nThreads);
    std::vector<std::thread> workers;
    for (int i = 0; i < nThreads; i++) {
        workers.push_back(std::thread([i, &tg, &stats]() {
            std::mt19937 generator(i);
            const int nAllocs = 250;
            std::vector<int> inputs1(nAllocs);
            std::vector<int> inputs2(nAllocs);
            std::iota(inputs1.begin(), inputs1.end(), 1);
            std::iota(inputs2.begin(), inputs2.end(), 1);

            // Shuffle this threads order of updates
            std::shuffle(inputs1.begin(), inputs1.end(), generator);
            std::shuffle(inputs2.begin(), inputs2.end(), generator);

            // Bind to the functions of interest
            std::function<void(size_t)> f1 = std::bind(
                    &EPStats::memAllocated, &stats, std::placeholders::_1);
            std::function<void(size_t)> f2 = std::bind(
                    &EPStats::memDeallocated, &stats, std::placeholders::_1);

            // Reorder if thread id is odd
            if (i & 1) {
                f1.swap(f2);
            }

            tg.threadUp();

            // Now run f1 then f2
            for (size_t i = 0; i < inputs1.size(); i++) {
                f1(inputs1.at(i));
            }
            for (size_t i = 0; i < inputs2.size(); i++) {
                f2(inputs2.at(i));
            }
        }));
    }

    for (int i = 0; i < nThreads; i++) {
        workers.at(i).join();
    }

    EXPECT_EQ(0, stats.getPreciseTotalMemoryUsed());
}
