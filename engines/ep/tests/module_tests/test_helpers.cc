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

#include "test_helpers.h"

#include <platform/compress.h>
#include <programs/engine_testapp/mock_server.h>
#include <string_utilities.h>
#include <xattr/blob.h>
#include <memory>
#include <thread>

#include "checkpoint_manager.h"
#include "vbucket.h"

StoredDocKey makeStoredDocKey(const std::string& string, CollectionID ns) {
    return StoredDocKey(string, ns);
}

Item make_item(Vbid vbid,
               const DocKey& key,
               const std::string& value,
               uint32_t exptime,
               protocol_binary_datatype_t datatype) {
    Item item(key,
              /*flags*/ 0,
              /*exp*/ exptime,
              value.c_str(),
              value.size(),
              datatype);
    item.setVBucketId(vbid);
    return item;
}

queued_item makeCommittedItem(StoredDocKey key, std::string value) {
    queued_item qi{new Item(key, 0, 0, value.data(), value.size())};
    return qi;
}

queued_item makeCommittedviaPrepareItem(StoredDocKey key, std::string value) {
    queued_item qi{new Item(key, 0, 0, value.data(), value.size())};
    qi->setCommittedviaPrepareSyncWrite();
    return qi;
}

queued_item makePendingItem(StoredDocKey key, std::string value) {
    queued_item qi{new Item(key, 0, 0, value.data(), value.size())};
    qi->setPendingSyncWrite({cb::durability::Level::Majority, 0});
    return qi;
}

std::unique_ptr<Item> makeCompressibleItem(Vbid vbid,
                                           const DocKey& key,
                                           const std::string& value,
                                           protocol_binary_datatype_t datatype,
                                           bool shouldCompress,
                                           bool makeXattrBody) {
    protocol_binary_datatype_t itemDataType = datatype;
    std::string v = value;
    if (makeXattrBody) {
        v = createXattrValue(value, true, false);
        itemDataType |= PROTOCOL_BINARY_DATATYPE_XATTR;
    }
    if (shouldCompress) {
        cb::compression::Buffer output;
        cb::compression::deflate(cb::compression::Algorithm::Snappy, v, output);
        itemDataType |= PROTOCOL_BINARY_DATATYPE_SNAPPY;
        return std::make_unique<Item>(key, /*flags*/0, /*exp*/0,
                                      output.data(), output.size(),
                                      itemDataType);
    }

    return std::make_unique<Item>(
            key, /*flags*/ 0, /*exp*/ 0, v.c_str(), v.length(), itemDataType);
}

bool queueNewItem(VBucket& vbucket, const std::string& key) {
    queued_item qi{new Item(makeStoredDocKey(key),
                            vbucket.getId(),
                            queue_op::mutation,
                            /*revSeq*/ 0,
                            /*bySeq*/ 0)};
    return vbucket.checkpointManager->queueDirty(vbucket,
                                                 qi,
                                                 GenerateBySeqno::Yes,
                                                 GenerateCas::Yes,
                                                 /*preLinkDocCtx*/ nullptr);
}

std::chrono::microseconds decayingSleep(std::chrono::microseconds uSeconds) {
    /* Max sleep time is slightly over a second */
    static const std::chrono::microseconds maxSleepTime(0x1 << 20);
    std::this_thread::sleep_for(uSeconds);
    return std::min(uSeconds * 2, maxSleepTime);
}

std::string createXattrValue(const std::string& body,
                             bool withSystemKey,
                             bool makeItSnappy) {
    cb::xattr::Blob blob;

    // Add enough XATTRs to be sure we would compress it if required
    blob.set("ABCuser1", "{\"author\":\"bubba\"}");
    blob.set("ABCuser2", "{\"author\":\"bubba\"}");
    blob.set("ABCuser3", "{\"author\":\"bubba\"}");
    blob.set("ABCuser4", "{\"author\":\"bubba\"}");
    blob.set("ABCuser5", "{\"author\":\"bubba\"}");
    blob.set("ABCuser6", "{\"author\":\"bubba\"}");

    if (withSystemKey) {
        blob.set("_sync", "{\"cas\":\"0xdeadbeefcafefeed\"}");
    }
    blob.set("meta", "{\"content-type\":\"text\"}");

    auto xattr_value = blob.finalize();

    // append body to the xattrs and store in data
    std::string data;
    std::copy(xattr_value.buf,
              xattr_value.buf + xattr_value.len,
              std::back_inserter(data));
    std::copy(
            body.c_str(), body.c_str() + body.size(), std::back_inserter(data));

    if (makeItSnappy) {
        cb::compression::Buffer output;
        cb::compression::deflate(
                cb::compression::Algorithm::Snappy, data, output);
        return {output.data(), output.size()};
    }
    return data;
}

TimeTraveller::TimeTraveller(int by) : by(by) {
    mock_time_travel(by);
}

TimeTraveller::~TimeTraveller() {
    // restore original timeline.
    mock_time_travel(-by);
}
