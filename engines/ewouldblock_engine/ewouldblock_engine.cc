/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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
 *                "ewouldblock_engine"
 *
 * The "ewouldblock_engine" allows one to test how memcached responds when the
 * engine returns EWOULDBLOCK instead of the correct response.
 *
 * Motivation:
 *
 * The EWOULDBLOCK response code can be returned from a number of engine
 * functions, and is used to indicate that the request could not be immediately
 * fulfilled, and it "would block" if it tried to. The correct way for
 * memcached to handle this (in general) is to suspend that request until it
 * is later notified by the engine (via notify_io_complete()).
 *
 * However, engines typically return the correct response to requests
 * immediately, only rarely (and from memcached's POV non-deterministically)
 * returning EWOULDBLOCK. This makes testing of the code-paths handling
 * EWOULDBLOCK tricky.
 *
 *
 * Operation:
 * This engine, when loaded by memcached proxies requests to a "real" engine.
 * Depending on how it is configured, it can simply pass the request on to the
 * real engine, or artificially return EWOULDBLOCK back to memcached.
 *
 * See the 'Modes' enum below for the possible modes for a connection. The mode
 * can be selected by sending a `request_ewouldblock_ctl` command
 *  (opcode cb::mcbp::ClientOpcode::EwouldblockCtl).
 *
 * DCP:
 *    There is a special DCP stream named "ewb_internal" which is an
 *    endless stream of items. You may also add a number at the end
 *    e.g. "ewb_internal:10" and it'll create a stream with 10 entries.
 *    It will always send the same K-V pair.
 *    Note that we don't register for disconnect events so you might
 *    experience weirdness if you first try to use the internal dcp
 *    stream, and then later on want to use the one provided by the
 *    engine. The workaround for that is to delete the bucket
 *    in between ;-) (put them in separate test suites and it'll all
 *    be handled for you.
 *
 *    Any other stream name results in proxying the dcp request to
 *    the underlying engine's DCP implementation.
 *
 */

#include "ewouldblock_engine.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <gsl/gsl>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include <logger/logger.h>
#include <memcached/dcp.h>
#include <memcached/engine.h>
#include <memcached/server_callback_iface.h>
#include <memcached/server_cookie_iface.h>
#include <memcached/server_log_iface.h>
#include <platform/cb_malloc.h>
#include <platform/dirutils.h>
#include <platform/thread.h>
#include <xattr/blob.h>

#include "utilities/engine_loader.h"

/* Public API declaration ****************************************************/

extern "C" {
    MEMCACHED_PUBLIC_API
    ENGINE_ERROR_CODE create_instance(GET_SERVER_API gsa, EngineIface** handle);

    MEMCACHED_PUBLIC_API
    void destroy_engine(void);
}


class EWB_Engine;

// Mapping from wrapped handle to EWB handles.
static std::map<EngineIface*, EWB_Engine*> engine_map;

class NotificationThread : public Couchbase::Thread {
public:
    NotificationThread(EWB_Engine& engine_)
        : Thread("ewb:pendingQ"),
          engine(engine_) {}

protected:
    void run() override;

protected:
    EWB_Engine& engine;
};

/**
 * The BlockMonitorThread represents the thread that is
 * monitoring the "lock" file. Once the file is no longer
 * there it will resume the client specified with the given
 * id.
 */
class BlockMonitorThread : public Couchbase::Thread {
public:
    BlockMonitorThread(EWB_Engine& engine_,
                       uint32_t id_,
                       const std::string file_)
        : Thread("ewb:BlockMon"),
          engine(engine_),
          id(id_),
          file(file_) {}

    /**
     * Wait for the underlying thread to reach the zombie state
     * (== terminated, but not reaped)
     */
    ~BlockMonitorThread() {
        waitForState(Couchbase::ThreadState::Zombie);
    }

protected:
    void run() override;

private:
    EWB_Engine& engine;
    const uint32_t id;
    const std::string file;
};

static SERVER_HANDLE_V1 wrapped_api;
static SERVER_HANDLE_V1 *real_api;

struct EwbServerCallbackApi : public ServerCallbackIface {
    void register_callback(EngineIface* engine,
                           ENGINE_EVENT_TYPE type,
                           EVENT_CALLBACK cb,
                           const void* cb_data) override {
        const auto& p = engine_map.find(engine);
        if (p == engine_map.end()) {
            std::cerr << "Can't find EWB corresponding to " << std::hex
                      << engine << std::endl;
            for (const auto& pair : engine_map) {
                std::cerr << "EH: " << std::hex << pair.first
                          << " = EWB: " << std::hex << pair.second << std::endl;
            }
            abort();
        }
        auto wrapped_eh = reinterpret_cast<EngineIface*>(p->second);
        real_api->callback->register_callback(wrapped_eh, type, cb, cb_data);
    }
    void perform_callbacks(ENGINE_EVENT_TYPE type,
                           const void* data,
                           const void* cookie) override {
        wrapped->perform_callbacks(type, data, cookie);
    }

    ServerCallbackIface* wrapped = nullptr;
};

static void init_wrapped_api(GET_SERVER_API fn) {
    static bool init = false;
    if (init) {
        return;
    }

    init = true;
    real_api = fn();
    wrapped_api = *real_api;

    // Overrides
    static EwbServerCallbackApi callback;
    callback.wrapped = real_api->callback;
    wrapped_api.callback = &callback;
}

static SERVER_HANDLE_V1 *get_wrapped_gsa() {
    return &wrapped_api;
}

/** ewouldblock_engine class */
class EWB_Engine : public EngineIface, public DcpIface {
private:
    enum class Cmd {
        NONE,
        GET_INFO,
        ALLOCATE,
        REMOVE,
        GET,
        STORE,
        CAS,
        ARITHMETIC,
        LOCK,
        UNLOCK,
        FLUSH,
        GET_STATS,
        GET_META,
        UNKNOWN_COMMAND
    };

    const char* to_string(Cmd cmd);

public:
    EWB_Engine(GET_SERVER_API gsa_);

    ~EWB_Engine() override;

    // Convert from a handle back to the read object.
    static EWB_Engine* to_engine(EngineIface* handle) {
        return reinterpret_cast<EWB_Engine*> (handle);
    }

    /* Returns true if the next command should have a fake error code injected.
     * @param func Address of the command function (get, store, etc).
     * @param cookie The cookie for the user's request.
     * @param[out] Error code to return.
     */
    bool should_inject_error(Cmd cmd, const void* cookie,
                             ENGINE_ERROR_CODE& err) {

        if (is_connection_suspended(cookie)) {
            err = ENGINE_EWOULDBLOCK;
            return true;
        }

        uint64_t id = real_api->cookie->get_connection_id(cookie);

        std::lock_guard<std::mutex> guard(cookie_map_mutex);

        auto iter = connection_map.find(id);
        if (iter == connection_map.end()) {
            return false;
        }

        if (iter->second.first != cookie) {
            // The cookie is different so it represents a different command
            connection_map.erase(iter);
            return false;
        }

        const bool inject = iter->second.second->should_inject_error(cmd, err);
        const bool add_to_pending_io_ops = iter->second.second->add_to_pending_io_ops();

        if (inject) {
            LOG_DEBUG("EWB_Engine: injecting error:{} for cmd:{}",
                      err,
                      to_string(cmd));

            if (err == ENGINE_EWOULDBLOCK && add_to_pending_io_ops) {
                // The server expects that if EWOULDBLOCK is returned then the
                // server should be notified in the future when the operation is
                // ready - so add this op to the pending IO queue.
                schedule_notification(iter->second.first);
            }
        }

        return inject;
    }

    /* Implementation of all the engine functions. ***************************/

    ENGINE_ERROR_CODE initialize(const char* config_str) override {
        // Extract the name of the real engine we will be proxying; then
        // create and initialize it.
        std::string config(config_str);
        auto seperator = config.find(";");
        std::string real_engine_name(config.substr(0, seperator));
        std::string real_engine_config;
        if (seperator != std::string::npos) {
            real_engine_config = config.substr(seperator + 1);
        }

        if ((real_engine_ref = load_engine(real_engine_name.c_str(), NULL)) ==
            NULL) {
            LOG_CRITICAL(
                    "ERROR: EWB_Engine::initialize(): Failed to load real "
                    "engine '{}'",
                    real_engine_name);
            std::abort();
        }

        if (!create_engine_instance(
                    real_engine_ref, get_wrapped_gsa, &real_engine)) {
            LOG_CRITICAL(
                    "ERROR: EWB_Engine::initialize(): Failed create "
                    "engine instance '{}'",
                    real_engine_name);
            std::abort();
        }

        real_engine_dcp = dynamic_cast<DcpIface*>(real_engine);

        engine_map[real_engine] = this;
        ENGINE_ERROR_CODE res =
                real_engine->initialize(real_engine_config.c_str());

        // Register a callback on DISCONNECT events, so we can delete
        // any stale elements from connection_map when a connection
        // DC's.
        real_api->callback->register_callback(
                this, ON_DISCONNECT, handle_disconnect, this);

        return res;
    }

    void destroy(bool force) override {
        real_engine->destroy(force);
        delete this;
    }

    cb::EngineErrorItemPair allocate(gsl::not_null<const void*> cookie,
                                     const DocKey& key,
                                     const size_t nbytes,
                                     const int flags,
                                     const rel_time_t exptime,
                                     uint8_t datatype,
                                     Vbid vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::ALLOCATE, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc(err));
        } else {
            return real_engine->allocate(
                    cookie, key, nbytes, flags, exptime, datatype, vbucket);
        }
    }

    std::pair<cb::unique_item_ptr, item_info> allocate_ex(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            size_t nbytes,
            size_t priv_nbytes,
            int flags,
            rel_time_t exptime,
            uint8_t datatype,
            Vbid vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::ALLOCATE, cookie, err)) {
            throw cb::engine_error(cb::engine_errc(err), "ewb: injecting error");
        } else {
            return real_engine->allocate_ex(cookie,
                                            key,
                                            nbytes,
                                            priv_nbytes,
                                            flags,
                                            exptime,
                                            datatype,
                                            vbucket);
        }
    }

    ENGINE_ERROR_CODE remove(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint64_t& cas,
            Vbid vbucket,
            boost::optional<cb::durability::Requirements> durability,
            mutation_descr_t& mut_info) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::REMOVE, cookie, err)) {
            return err;
        } else {
            return real_engine->remove(
                    cookie, key, cas, vbucket, durability, mut_info);
        }
    }

    void release(gsl::not_null<item*> item) override {
        LOG_DEBUG("EWB_Engine: release");

        if (item == &dcp_mutation_item) {
            // Ignore the DCP mutation, we own it (and don't track
            // refcounts on it).
        } else {
            return real_engine->release(item);
        }
    }

    cb::EngineErrorItemPair get(gsl::not_null<const void*> cookie,
                                const DocKey& key,
                                Vbid vbucket,
                                DocStateFilter documentStateFilter) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET, cookie, err)) {
            return std::make_pair(
                    cb::engine_errc(err),
                    cb::unique_item_ptr{nullptr, cb::ItemDeleter{this}});
        } else {
            return real_engine->get(cookie, key, vbucket, documentStateFilter);
        }
    }

    cb::EngineErrorItemPair get_if(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            Vbid vbucket,
            std::function<bool(const item_info&)> filter) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc::would_block);
        } else {
            return real_engine->get_if(cookie, key, vbucket, filter);
        }
    }

    cb::EngineErrorItemPair get_and_touch(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            Vbid vbucket,
            uint32_t exptime,
            boost::optional<cb::durability::Requirements> durability) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc::would_block);
        } else {
            return real_engine->get_and_touch(
                    cookie, key, vbucket, exptime, durability);
        }
    }

    cb::EngineErrorItemPair get_locked(gsl::not_null<const void*> cookie,
                                       const DocKey& key,
                                       Vbid vbucket,
                                       uint32_t lock_timeout) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::LOCK, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc(err));
        } else {
            return real_engine->get_locked(cookie, key, vbucket, lock_timeout);
        }
    }

    ENGINE_ERROR_CODE unlock(gsl::not_null<const void*> cookie,
                             const DocKey& key,
                             Vbid vbucket,
                             uint64_t cas) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::UNLOCK, cookie, err)) {
            return err;
        } else {
            return real_engine->unlock(cookie, key, vbucket, cas);
        }
    }

    cb::EngineErrorMetadataPair get_meta(gsl::not_null<const void*> cookie,
                                         const DocKey& key,
                                         Vbid vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET_META, cookie, err)) {
            return std::make_pair(cb::engine_errc(err), item_info());
        } else {
            return real_engine->get_meta(cookie, key, vbucket);
        }
    }

    ENGINE_ERROR_CODE store(
            gsl::not_null<const void*> cookie,
            gsl::not_null<item*> item,
            uint64_t& cas,
            ENGINE_STORE_OPERATION operation,
            boost::optional<cb::durability::Requirements> durability,
            DocumentState document_state) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        Cmd opcode = (operation == OPERATION_CAS) ? Cmd::CAS : Cmd::STORE;
        if (should_inject_error(opcode, cookie, err)) {
            return err;
        } else {
            return real_engine->store(
                    cookie, item, cas, operation, durability, document_state);
        }
    }

    cb::EngineErrorCasPair store_if(
            gsl::not_null<const void*> cookie,
            gsl::not_null<item*> item,
            uint64_t cas,
            ENGINE_STORE_OPERATION operation,
            cb::StoreIfPredicate predicate,
            boost::optional<cb::durability::Requirements> durability,
            DocumentState document_state) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        Cmd opcode = (operation == OPERATION_CAS) ? Cmd::CAS : Cmd::STORE;
        if (should_inject_error(opcode, cookie, err)) {
            return {cb::engine_errc(err), 0};
        } else {
            return real_engine->store_if(cookie,
                                         item,
                                         cas,
                                         operation,
                                         predicate,
                                         durability,
                                         document_state);
        }
    }

    ENGINE_ERROR_CODE flush(gsl::not_null<const void*> cookie) override {
        // Flush is a little different - it often returns EWOULDBLOCK, and
        // notify_io_complete() just tells the server it can issue it's *next*
        // command (i.e. no need to re-flush). Therefore just pass Flush
        // straight through for now.
        return real_engine->flush(cookie);
    }

    ENGINE_ERROR_CODE get_stats(gsl::not_null<const void*> cookie,
                                cb::const_char_buffer key,
                                ADD_STAT add_stat) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET_STATS, cookie, err)) {
            return err;
        } else {
            return real_engine->get_stats(cookie, key, add_stat);
        }
    }

    void reset_stats(gsl::not_null<const void*> cookie) override {
        return real_engine->reset_stats(cookie);
    }

    /* Handle 'unknown_command'. In additional to wrapping calls to the
     * underlying real engine, this is also used to configure
     * ewouldblock_engine itself using he CMD_EWOULDBLOCK_CTL opcode.
     */
    ENGINE_ERROR_CODE unknown_command(const void* cookie,
                                      const cb::mcbp::Request& req,
                                      ADD_RESPONSE response) override {
        const auto opcode = req.getClientOpcode();
        if (opcode == cb::mcbp::ClientOpcode::EwouldblockCtl) {
            using cb::mcbp::request::EWB_Payload;
            auto extras = req.getExtdata();
            const auto* payload =
                    reinterpret_cast<const EWB_Payload*>(extras.data());
            const auto mode = static_cast<EWBEngineMode>(payload->getMode());
            const auto value = payload->getValue();
            const auto injected_error =
                    static_cast<ENGINE_ERROR_CODE>(payload->getInjectError());
            auto k = req.getKey();
            const std::string key(reinterpret_cast<const char*>(k.data()),
                                  k.size());
            std::shared_ptr<FaultInjectMode> new_mode = nullptr;

            // Validate mode, and construct new fault injector.
            switch (mode) {
                case EWBEngineMode::Next_N:
                    new_mode = std::make_shared<ErrOnNextN>(injected_error, value);
                    break;

                case EWBEngineMode::Random:
                    new_mode = std::make_shared<ErrRandom>(injected_error, value);
                    break;

                case EWBEngineMode::First:
                    new_mode = std::make_shared<ErrOnFirst>(injected_error);
                    break;

                case EWBEngineMode::Sequence:
                    new_mode = std::make_shared<ErrSequence>(injected_error, value);
                    break;

                case EWBEngineMode::No_Notify:
                    new_mode = std::make_shared<ErrOnNoNotify>(injected_error);
                    break;

                case EWBEngineMode::CasMismatch:
                    new_mode = std::make_shared<CASMismatch>(value);
                    break;

                case EWBEngineMode::IncrementClusterMapRevno:
                    clustermap_revno++;
                    response(nullptr,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             PROTOCOL_BINARY_RAW_BYTES,
                             cb::mcbp::Status::Success,
                             0,
                             cookie);
                    return ENGINE_SUCCESS;

                case EWBEngineMode::BlockMonitorFile:
                    return handleBlockMonitorFile(cookie, value, key, response);

                case EWBEngineMode::Suspend:
                    return handleSuspend(cookie, value, response);

                case EWBEngineMode::Resume:
                    return handleResume(cookie, value, response);

                case EWBEngineMode::SetItemCas:
                    return setItemCas(cookie, key, value, response);
            }

            if (new_mode == nullptr) {
                LOG_WARNING(
                        "EWB_Engine::unknown_command(): "
                        "Got unexpected mode={} for EWOULDBLOCK_CTL, ",
                        (unsigned int)mode);
                response(nullptr,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         0,
                         PROTOCOL_BINARY_RAW_BYTES,
                         cb::mcbp::Status::Einval,
                         /*cas*/ 0,
                         cookie);
                return ENGINE_FAILED;
            } else {
                try {
                    LOG_DEBUG(
                            "EWB_Engine::unknown_command(): Setting EWB mode "
                            "to "
                            "{} for cookie {}",
                            new_mode->to_string(),
                            cookie);

                    uint64_t id = real_api->cookie->get_connection_id(cookie);

                    {
                        std::lock_guard<std::mutex> guard(cookie_map_mutex);
                        connection_map.erase(id);
                        connection_map.emplace(
                                id, std::make_pair(cookie, new_mode));
                    }

                    response(nullptr,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             PROTOCOL_BINARY_RAW_BYTES,
                             cb::mcbp::Status::Success,
                             /*cas*/ 0,
                             cookie);
                    return ENGINE_SUCCESS;
                } catch (std::bad_alloc&) {
                    return ENGINE_ENOMEM;
                }
            }
        } else {
            ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
            if (should_inject_error(Cmd::UNKNOWN_COMMAND, cookie, err)) {
                return err;
            } else {
                return real_engine->unknown_command(cookie, req, response);
            }
        }
    }

    void item_set_cas(gsl::not_null<item*> item, uint64_t cas) override {
        // function cannot return EWOULDBLOCK, simply call the real_engine's
        // function directly.
        real_engine->item_set_cas(item, cas);
    }

    void item_set_datatype(gsl::not_null<item*> itm,
                           protocol_binary_datatype_t datatype) override {
        // function cannot return EWOULDBLOCK, simply call the real_engine's
        // function directly.
        real_engine->item_set_datatype(itm, datatype);
    }

    bool get_item_info(gsl::not_null<const item*> item,
                       gsl::not_null<item_info*> item_info) override {
        LOG_DEBUG("EWB_Engine: get_item_info");

        // This function cannot return EWOULDBLOCK - just chain to the real
        // engine's function, unless it is a request for our special DCP item.
        if (item == &dcp_mutation_item) {
            item_info->cas = 0;
            item_info->vbucket_uuid = 0;
            item_info->seqno = 0;
            item_info->exptime = 0;
            item_info->nbytes =
                    gsl::narrow<uint32_t>(dcp_mutation_item.value.size());
            item_info->flags = 0;
            item_info->datatype = PROTOCOL_BINARY_DATATYPE_XATTR;
            item_info->key = {dcp_mutation_item.key,
                              DocKeyEncodesCollectionId::No};
            item_info->value[0].iov_base = &dcp_mutation_item.value[0];
            item_info->value[0].iov_len = item_info->nbytes;
            return true;
        } else {
            return real_engine->get_item_info(item, item_info);
        }
    }

    cb::engine::FeatureSet getFeatures() override {
        return real_engine->getFeatures();
    }

    bool isXattrEnabled() override {
        return real_engine->isXattrEnabled();
    }

    BucketCompressionMode getCompressionMode() override {
        return real_engine->getCompressionMode();
    }

    size_t getMaxItemSize() override {
        return real_engine->getMaxItemSize();
    }

    float getMinCompressionRatio() override {
        return real_engine->getMinCompressionRatio();
    }

    ///////////////////////////////////////////////////////////////////////////
    //             All of the methods used in the DCP interface              //
    //                                                                       //
    // We don't support mocking with the DCP interface yet, so all access to //
    // the DCP interface will be proxied down to the underlying engine.      //
    ///////////////////////////////////////////////////////////////////////////
    ENGINE_ERROR_CODE step(
            gsl::not_null<const void*> cookie,
            gsl::not_null<struct dcp_message_producers*> producers) override;

    ENGINE_ERROR_CODE open(gsl::not_null<const void*> cookie,
                           uint32_t opaque,
                           uint32_t seqno,
                           uint32_t flags,
                           cb::const_char_buffer name) override;

    ENGINE_ERROR_CODE add_stream(gsl::not_null<const void*> cookie,
                                 uint32_t opaque,
                                 Vbid vbucket,
                                 uint32_t flags) override;

    ENGINE_ERROR_CODE close_stream(gsl::not_null<const void*> cookie,
                                   uint32_t opaque,
                                   Vbid vbucket,
                                   cb::mcbp::DcpStreamId sid) override;

    ENGINE_ERROR_CODE stream_req(
            gsl::not_null<const void*> cookie,
            uint32_t flags,
            uint32_t opaque,
            Vbid vbucket,
            uint64_t start_seqno,
            uint64_t end_seqno,
            uint64_t vbucket_uuid,
            uint64_t snap_start_seqno,
            uint64_t snap_end_seqno,
            uint64_t* rollback_seqno,
            dcp_add_failover_log callback,
            boost::optional<cb::const_char_buffer> json) override;

    ENGINE_ERROR_CODE get_failover_log(gsl::not_null<const void*> cookie,
                                       uint32_t opaque,
                                       Vbid vbucket,
                                       dcp_add_failover_log callback) override;

    ENGINE_ERROR_CODE stream_end(gsl::not_null<const void*> cookie,
                                 uint32_t opaque,
                                 Vbid vbucket,
                                 uint32_t flags) override;

    ENGINE_ERROR_CODE snapshot_marker(gsl::not_null<const void*> cookie,
                                      uint32_t opaque,
                                      Vbid vbucket,
                                      uint64_t start_seqno,
                                      uint64_t end_seqno,
                                      uint32_t flags) override;

    ENGINE_ERROR_CODE mutation(gsl::not_null<const void*> cookie,
                               uint32_t opaque,
                               const DocKey& key,
                               cb::const_byte_buffer value,
                               size_t priv_bytes,
                               uint8_t datatype,
                               uint64_t cas,
                               Vbid vbucket,
                               uint32_t flags,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               uint32_t expiration,
                               uint32_t lock_time,
                               cb::const_byte_buffer meta,
                               uint8_t nru) override;

    ENGINE_ERROR_CODE deletion(gsl::not_null<const void*> cookie,
                               uint32_t opaque,
                               const DocKey& key,
                               cb::const_byte_buffer value,
                               size_t priv_bytes,
                               uint8_t datatype,
                               uint64_t cas,
                               Vbid vbucket,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               cb::const_byte_buffer meta) override;

    ENGINE_ERROR_CODE deletion_v2(gsl::not_null<const void*> cookie,
                                  uint32_t opaque,
                                  const DocKey& key,
                                  cb::const_byte_buffer value,
                                  size_t priv_bytes,
                                  uint8_t datatype,
                                  uint64_t cas,
                                  Vbid vbucket,
                                  uint64_t by_seqno,
                                  uint64_t rev_seqno,
                                  uint32_t delete_time) override;

    ENGINE_ERROR_CODE expiration(gsl::not_null<const void*> cookie,
                                 uint32_t opaque,
                                 const DocKey& key,
                                 cb::const_byte_buffer value,
                                 size_t priv_bytes,
                                 uint8_t datatype,
                                 uint64_t cas,
                                 Vbid vbucket,
                                 uint64_t by_seqno,
                                 uint64_t rev_seqno,
                                 uint32_t deleteTime) override;

    ENGINE_ERROR_CODE set_vbucket_state(gsl::not_null<const void*> cookie,
                                        uint32_t opaque,
                                        Vbid vbucket,
                                        vbucket_state_t state) override;

    ENGINE_ERROR_CODE noop(gsl::not_null<const void*> cookie,
                           uint32_t opaque) override;

    ENGINE_ERROR_CODE buffer_acknowledgement(gsl::not_null<const void*> cookie,
                                             uint32_t opaque,
                                             Vbid vbucket,
                                             uint32_t buffer_bytes) override;

    ENGINE_ERROR_CODE control(gsl::not_null<const void*> cookie,
                              uint32_t opaque,
                              cb::const_char_buffer key,
                              cb::const_char_buffer value) override;

    ENGINE_ERROR_CODE response_handler(
            gsl::not_null<const void*> cookie,
            const protocol_binary_response_header* response) override;

    ENGINE_ERROR_CODE system_event(gsl::not_null<const void*> cookie,
                                   uint32_t opaque,
                                   Vbid vbucket,
                                   mcbp::systemevent::id event,
                                   uint64_t bySeqno,
                                   mcbp::systemevent::version version,
                                   cb::const_byte_buffer key,
                                   cb::const_byte_buffer eventData) override;

    ENGINE_ERROR_CODE prepare(gsl::not_null<const void*> cookie,
                              uint32_t opaque,
                              const DocKey& key,
                              cb::const_byte_buffer value,
                              size_t priv_bytes,
                              uint8_t datatype,
                              uint64_t cas,
                              Vbid vbucket,
                              uint32_t flags,
                              uint64_t by_seqno,
                              uint64_t rev_seqno,
                              uint32_t expiration,
                              uint32_t lock_time,
                              uint8_t nru,
                              DocumentState document_state,
                              cb::durability::Requirements durability) override;
    ENGINE_ERROR_CODE seqno_acknowledged(gsl::not_null<const void*> cookie,
                                         uint32_t opaque,
                                         Vbid vbucket,
                                         uint64_t in_memory_seqno,
                                         uint64_t on_disk_seqno) override;
    ENGINE_ERROR_CODE commit(gsl::not_null<const void*> cookie,
                             uint32_t opaque,
                             Vbid vbucket,
                             const DocKey& key,
                             uint64_t prepared_seqno,
                             uint64_t commit_seqno) override;
    ENGINE_ERROR_CODE abort(gsl::not_null<const void*> cookie,
                            uint32_t opaque,
                            uint64_t prepared_seqno,
                            uint64_t abort_seqno) override;

    static void handle_disconnect(const void* cookie,
                                  ENGINE_EVENT_TYPE type,
                                  const void* event_data,
                                  const void* cb_data) {
        cb_assert(event_data == NULL);
        EWB_Engine* ewb =
                reinterpret_cast<EWB_Engine*>(const_cast<void*>(cb_data));
        LOG_DEBUG("EWB_Engine::handle_disconnect");

        uint64_t id = real_api->cookie->get_connection_id(cookie);
        {
            std::lock_guard<std::mutex> guard(ewb->cookie_map_mutex);
            ewb->connection_map.erase(id);
        }
    }

    GET_SERVER_API gsa;

    // Actual engine we are proxying requests to.
    EngineIface* real_engine;
    engine_reference* real_engine_ref;

    // Pointer to DcpIface for the underlying engine we are proxying; or
    // nullptr if it doesn't implement DcpIface;
    DcpIface* real_engine_dcp = nullptr;

    std::atomic_int clustermap_revno;

    /**
     * The method responsible for pushing all of the notify_io_complete
     * to the frontend. It is run by notify_io_thread and not intended to
     * be called by anyone else!.
     */
    void process_notifications();
    std::unique_ptr<Couchbase::Thread> notify_io_thread;

protected:
    /**
     * Handle the control message for block monitor file
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier used to represent the cookie
     * @param file The file to monitor
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleBlockMonitorFile(const void* cookie,
                                             uint32_t id,
                                             const std::string& file,
                                             ADD_RESPONSE response);

    /**
     * Handle the control message for suspend
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier used to represent the cookie to resume
     *           (the use of a different id is to allow resume to
     *           be sent on a different connection)
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleSuspend(const void* cookie,
                                    uint32_t id,
                                    ADD_RESPONSE response);

    /**
     * Handle the control message for resume
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier representing the connection to resume
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleResume(const void* cookie,
                                   uint32_t id,
                                   ADD_RESPONSE response);

    /**
     * @param cookie the cookie executing the operation
     * @param key ID of the item whose CAS should be changed
     * @param cas The new CAS
     * @param response Response callback used to send a response to the client
     * @return Standard engine error codes
     */
    ENGINE_ERROR_CODE setItemCas(const void *cookie,
                                 const std::string& key, uint32_t cas,
                                 ADD_RESPONSE response);

private:
    // Shared state between the main thread of execution and the background
    // thread processing pending io ops.
    std::mutex mutex;
    std::condition_variable condvar;
    std::queue<const void*> pending_io_ops;

    std::atomic<bool> stop_notification_thread;

    static cb::engine_errc collections_set_manifest(
            gsl::not_null<EngineIface*> handle,
            gsl::not_null<const void*> cookie,
            cb::const_char_buffer json);

    static cb::engine_errc collections_get_manifest(
            gsl::not_null<EngineIface*> handle,
            gsl::not_null<const void*> cookie,
            ADD_RESPONSE response);

    static cb::EngineErrorGetCollectionIDResult collections_get_collection_id(
            gsl::not_null<EngineIface*> handle,
            gsl::not_null<const void*> cookie,
            cb::const_char_buffer path);

    // Base class for all fault injection modes.
    struct FaultInjectMode {
        virtual ~FaultInjectMode() = default;

        FaultInjectMode(ENGINE_ERROR_CODE injected_error_)
          : injected_error(injected_error_) {}

        virtual bool add_to_pending_io_ops() {
            return true;
        }
        virtual bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) = 0;

        virtual std::string to_string() const = 0;

    protected:
        ENGINE_ERROR_CODE injected_error;
    };

    // Subclasses for each fault inject mode: /////////////////////////////////

    class ErrOnFirst : public FaultInjectMode {
    public:
        ErrOnFirst(ENGINE_ERROR_CODE injected_error_)
          : FaultInjectMode(injected_error_),
            prev_cmd(Cmd::NONE) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            // Block unless the previous command from this cookie
            // was the same - i.e. all of a connections' commands
            // will EWOULDBLOCK the first time they are called.
            bool inject = (prev_cmd != cmd);
            prev_cmd = cmd;
            if (inject) {
                err = injected_error;
            }
            return inject;
        }

        std::string to_string() const {
            return "ErrOnFirst inject_error=" + std::to_string(injected_error);
        }

    private:
        // Last command issued by this cookie.
        Cmd prev_cmd;
    };

    class ErrOnNextN : public FaultInjectMode {
    public:
        ErrOnNextN(ENGINE_ERROR_CODE injected_error_, uint32_t count_)
          : FaultInjectMode(injected_error_),
            count(count_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            if (count > 0) {
                --count;
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("ErrOnNextN") +
                   " inject_error=" + std::to_string(injected_error) +
                   " count=" + std::to_string(count);
        }

    private:
        // The count of commands issued that should return error.
        uint32_t count;
    };

    class ErrRandom : public FaultInjectMode {
    public:
        ErrRandom(ENGINE_ERROR_CODE injected_error_, uint32_t percentage_)
          : FaultInjectMode(injected_error_),
            percentage_to_err(percentage_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<uint32_t> dis(1, 100);
            if (dis(gen) < percentage_to_err) {
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("ErrRandom") +
                   " inject_error=" + std::to_string(injected_error) +
                   " percentage=" + std::to_string(percentage_to_err);
        }

    private:
        // Percentage chance that the specified error should be injected.
        uint32_t percentage_to_err;
    };

    class ErrSequence : public FaultInjectMode {
    public:
        ErrSequence(ENGINE_ERROR_CODE injected_error_, uint32_t sequence_)
            : FaultInjectMode(injected_error_),
              sequence(sequence_),
              pos(0) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            bool inject = false;
            if (pos < 32) {
                inject = (sequence & (1 << pos)) != 0;
                pos++;
            }
            if (inject) {
                err = injected_error;
            }
            return inject;
        }

        std::string to_string() const {
            std::stringstream ss;
            ss << "ErrSequence inject_error=" << injected_error
               << " sequence=0x" << std::hex << sequence
               << " pos=" << pos;
            return ss.str();
        }

    private:
        uint32_t sequence;
        uint32_t pos;
    };

    class ErrOnNoNotify : public FaultInjectMode {
        public:
            ErrOnNoNotify(ENGINE_ERROR_CODE injected_error_)
              : FaultInjectMode(injected_error_),
                issued_return_error(false) {}

            bool add_to_pending_io_ops() {return false;}
            bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
                if (!issued_return_error) {
                    issued_return_error = true;
                    err = injected_error;
                    return true;
                } else {
                    return false;
                }
            }

            std::string to_string() const {
                return std::string("ErrOnNoNotify") +
                       " inject_error=" + std::to_string(injected_error) +
                       " issued_return_error=" +
                       std::to_string(issued_return_error);
            }

        private:
            // Record of whether have yet issued return error.
            bool issued_return_error;
        };

    class CASMismatch : public FaultInjectMode {
    public:
        CASMismatch(uint32_t count_)
          : FaultInjectMode(ENGINE_KEY_EEXISTS),
            count(count_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            if (cmd == Cmd::CAS && (count > 0)) {
                --count;
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("CASMismatch") +
                   " count=" + std::to_string(count);
        }

    private:
        uint32_t count;
    };

    // Map of connections (aka cookies) to their current mode.
    std::map<uint64_t, std::pair<const void*, std::shared_ptr<FaultInjectMode> > > connection_map;
    // Mutex for above map.
    std::mutex cookie_map_mutex;

    // Current DCP mutation `item`. We return the address of this
    // (in the dcp step() function) back to the server, and then in
    // get_item_info we check if the requested item is this one.
    class EwbDcpKey {
    public:
        EwbDcpKey()
            : key("k") {
            cb::xattr::Blob builder;
            builder.set("_ewb", "{\"internal\":true}");
            builder.set("meta", "{\"author\":\"jack\"}");
            const auto blob = builder.finalize();
            std::copy(blob.buf, blob.buf + blob.len, std::back_inserter(value));
            // MB24971 - the body is large as it increases the probability of
            // transit returning TransmitResult::SoftError
            const std::string body(1000, 'x');
            std::copy(body.begin(), body.end(), std::back_inserter(value));
        }

        std::string key;
        std::vector<uint8_t> value;
    } dcp_mutation_item;

    /**
     * The dcp_stream map is used to map a cookie to the count of objects
     * it should send on the stream.
     *
     * Each entry in here constists of a pair containing a boolean specifying
     * if the stream is opened or not, and a count of how many times we should
     * return data
     */
    std::map<const void*, std::pair<bool, uint64_t>> dcp_stream;

    friend class BlockMonitorThread;
    std::map<uint32_t, const void*> suspended_map;
    std::mutex suspended_map_mutex;

    bool suspend(const void* cookie, uint32_t id) {
        {
            std::lock_guard<std::mutex> guard(suspended_map_mutex);
            auto iter = suspended_map.find(id);
            if (iter == suspended_map.cend()) {
                suspended_map[id] = cookie;
                return true;
            }
        }

        return false;
    }

    bool resume(uint32_t id) {
        const void* cookie = nullptr;
        {
            std::lock_guard<std::mutex> guard(suspended_map_mutex);
            auto iter = suspended_map.find(id);
            if (iter == suspended_map.cend()) {
                return false;
            }
            cookie = iter->second;
            suspended_map.erase(iter);
        }


        schedule_notification(cookie);
        return true;
    }

    bool is_connection_suspended(const void* cookie) {
        std::lock_guard<std::mutex> guard(suspended_map_mutex);
        for (const auto c : suspended_map) {
            if (c.second == cookie) {
                LOG_DEBUG(
                        "Connection {} with id {} should be suspended for "
                        "engine {}",
                        c.second,
                        c.first,
                        (void*)this);

                return true;
            }
        }
        return false;
    }

    void schedule_notification(const void* cookie) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            pending_io_ops.push(cookie);
        }
        LOG_DEBUG("EWB_Engine: connection {} should be resumed for engine {}",
                  (void*)cookie,
                  (void*)this);

        condvar.notify_one();
    }

    // Vector to keep track of the threads we've started to ensure
    // we don't leak memory ;-)
    std::mutex threads_mutex;
    std::vector<std::unique_ptr<Couchbase::Thread> > threads;
};

EWB_Engine::EWB_Engine(GET_SERVER_API gsa_)
  : gsa(gsa_),
    real_engine(NULL),
    real_engine_ref(nullptr),
    notify_io_thread(new NotificationThread(*this))
{
    init_wrapped_api(gsa);

    EngineIface::collections = {};
    EngineIface::collections.set_manifest = collections_set_manifest;
    EngineIface::collections.get_manifest = collections_get_manifest;
    EngineIface::collections.get_collection_id = collections_get_collection_id;

    clustermap_revno = 1;

    stop_notification_thread.store(false);
    notify_io_thread->start();
}

EWB_Engine::~EWB_Engine() {
    engine_map.erase(real_engine);
    cb_free(real_engine_ref);
    stop_notification_thread = true;
    condvar.notify_all();
    notify_io_thread->waitForState(Couchbase::ThreadState::Zombie);
}

ENGINE_ERROR_CODE EWB_Engine::step(
        gsl::not_null<const void*> cookie,
        gsl::not_null<struct dcp_message_producers*> producers) {
    auto stream = dcp_stream.find(cookie);
    if (stream != dcp_stream.end()) {
        auto& count = stream->second.second;
        // If the stream is enabled and we have data to send..
        if (stream->second.first && count > 0) {
            // This is using the internal dcp implementation which always
            // send the same item back
            auto ret = producers->mutation(0xdeadbeef /*opqaue*/,
                                           &dcp_mutation_item,
                                           Vbid(0),
                                           0 /*by_seqno*/,
                                           0 /*rev_seqno*/,
                                           0 /*lock_time*/,
                                           nullptr /*meta*/,
                                           0 /*nmeta*/,
                                           0 /*nru*/,
                                           {});
            --count;
            return ret;
        }
        return ENGINE_EWOULDBLOCK;
    }
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    }
    return real_engine_dcp->step(cookie, producers);
}

ENGINE_ERROR_CODE EWB_Engine::open(gsl::not_null<const void*> cookie,
                                   uint32_t opaque,
                                   uint32_t seqno,
                                   uint32_t flags,
                                   cb::const_char_buffer name) {
    std::string nm = cb::to_string(name);
    if (nm.find("ewb_internal") == 0) {
        // Yeah, this is a request for the internal "magic" DCP stream
        // The user could specify the iteration count by adding a colon
        // at the end...
        auto idx = nm.rfind(":");

        if (idx != nm.npos) {
            dcp_stream[cookie] =
                    std::make_pair(false, std::stoull(nm.substr(idx + 1)));
        } else {
            dcp_stream[cookie] =
                    std::make_pair(false, std::numeric_limits<uint64_t>::max());
        }
        return ENGINE_SUCCESS;
    }

    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->open(cookie, opaque, seqno, flags, name);
    }
}

ENGINE_ERROR_CODE EWB_Engine::stream_req(
        gsl::not_null<const void*> cookie,
        uint32_t flags,
        uint32_t opaque,
        Vbid vbucket,
        uint64_t start_seqno,
        uint64_t end_seqno,
        uint64_t vbucket_uuid,
        uint64_t snap_start_seqno,
        uint64_t snap_end_seqno,
        uint64_t* rollback_seqno,
        dcp_add_failover_log callback,
        boost::optional<cb::const_char_buffer> json) {
    auto stream = dcp_stream.find(cookie.get());
    if (stream != dcp_stream.end()) {
        // This is a client of our internal streams.. just let it pass
        if (start_seqno == 1) {
            *rollback_seqno = 0;
            return ENGINE_ROLLBACK;
        }
        // Start the stream
        stream->second.first = true;
        return ENGINE_SUCCESS;
    }

    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->stream_req(cookie,
                                           flags,
                                           opaque,
                                           vbucket,
                                           start_seqno,
                                           end_seqno,
                                           vbucket_uuid,
                                           snap_start_seqno,
                                           snap_end_seqno,
                                           rollback_seqno,
                                           callback,
                                           json);
    }
}

ENGINE_ERROR_CODE EWB_Engine::add_stream(gsl::not_null<const void*> cookie,
                                         uint32_t opaque,
                                         Vbid vbucket,
                                         uint32_t flags) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->add_stream(cookie, opaque, vbucket, flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::close_stream(gsl::not_null<const void*> cookie,
                                           uint32_t opaque,
                                           Vbid vbucket,
                                           cb::mcbp::DcpStreamId sid) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->close_stream(cookie, opaque, vbucket, sid);
    }
}

ENGINE_ERROR_CODE EWB_Engine::get_failover_log(
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        Vbid vbucket,
        dcp_add_failover_log callback) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->get_failover_log(
                cookie, opaque, vbucket, callback);
    }
}

ENGINE_ERROR_CODE EWB_Engine::stream_end(gsl::not_null<const void*> cookie,
                                         uint32_t opaque,
                                         Vbid vbucket,
                                         uint32_t flags) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->stream_end(cookie, opaque, vbucket, flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::snapshot_marker(gsl::not_null<const void*> cookie,
                                              uint32_t opaque,
                                              Vbid vbucket,
                                              uint64_t start_seqno,
                                              uint64_t end_seqno,
                                              uint32_t flags) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->snapshot_marker(
                cookie, opaque, vbucket, start_seqno, end_seqno, flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::mutation(gsl::not_null<const void*> cookie,
                                       uint32_t opaque,
                                       const DocKey& key,
                                       cb::const_byte_buffer value,
                                       size_t priv_bytes,
                                       uint8_t datatype,
                                       uint64_t cas,
                                       Vbid vbucket,
                                       uint32_t flags,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       uint32_t expiration,
                                       uint32_t lock_time,
                                       cb::const_byte_buffer meta,
                                       uint8_t nru) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->mutation(cookie,
                                         opaque,
                                         key,
                                         value,
                                         priv_bytes,
                                         datatype,
                                         cas,
                                         vbucket,
                                         flags,
                                         by_seqno,
                                         rev_seqno,
                                         expiration,
                                         lock_time,
                                         meta,
                                         nru);
    }
}

ENGINE_ERROR_CODE EWB_Engine::deletion(gsl::not_null<const void*> cookie,
                                       uint32_t opaque,
                                       const DocKey& key,
                                       cb::const_byte_buffer value,
                                       size_t priv_bytes,
                                       uint8_t datatype,
                                       uint64_t cas,
                                       Vbid vbucket,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       cb::const_byte_buffer meta) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->deletion(cookie,
                                         opaque,
                                         key,
                                         value,
                                         priv_bytes,
                                         datatype,
                                         cas,
                                         vbucket,
                                         by_seqno,
                                         rev_seqno,
                                         meta);
    }
}

ENGINE_ERROR_CODE EWB_Engine::deletion_v2(gsl::not_null<const void*> cookie,
                                          uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          Vbid vbucket,
                                          uint64_t by_seqno,
                                          uint64_t rev_seqno,
                                          uint32_t delete_time) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->deletion_v2(cookie,
                                            opaque,
                                            key,
                                            value,
                                            priv_bytes,
                                            datatype,
                                            cas,
                                            vbucket,
                                            by_seqno,
                                            rev_seqno,
                                            delete_time);
    }
}

ENGINE_ERROR_CODE EWB_Engine::expiration(gsl::not_null<const void*> cookie,
                                         uint32_t opaque,
                                         const DocKey& key,
                                         cb::const_byte_buffer value,
                                         size_t priv_bytes,
                                         uint8_t datatype,
                                         uint64_t cas,
                                         Vbid vbucket,
                                         uint64_t by_seqno,
                                         uint64_t rev_seqno,
                                         uint32_t deleteTime) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->expiration(cookie,
                                           opaque,
                                           key,
                                           value,
                                           priv_bytes,
                                           datatype,
                                           cas,
                                           vbucket,
                                           by_seqno,
                                           rev_seqno,
                                           deleteTime);
    }
}

ENGINE_ERROR_CODE EWB_Engine::set_vbucket_state(
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        Vbid vbucket,
        vbucket_state_t state) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->set_vbucket_state(
                cookie, opaque, vbucket, state);
    }
}

ENGINE_ERROR_CODE EWB_Engine::noop(gsl::not_null<const void*> cookie,
                                   uint32_t opaque) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->noop(cookie, opaque);
    }
}

ENGINE_ERROR_CODE EWB_Engine::buffer_acknowledgement(
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        Vbid vbucket,
        uint32_t buffer_bytes) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->buffer_acknowledgement(
                cookie, opaque, vbucket, buffer_bytes);
    }
}

ENGINE_ERROR_CODE EWB_Engine::control(gsl::not_null<const void*> cookie,
                                      uint32_t opaque,
                                      cb::const_char_buffer key,
                                      cb::const_char_buffer value) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->control(cookie, opaque, key, value);
    }
}

ENGINE_ERROR_CODE EWB_Engine::response_handler(
        gsl::not_null<const void*> cookie,
        const protocol_binary_response_header* response) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->response_handler(cookie, response);
    }
}

ENGINE_ERROR_CODE EWB_Engine::system_event(gsl::not_null<const void*> cookie,
                                           uint32_t opaque,
                                           Vbid vbucket,
                                           mcbp::systemevent::id event,
                                           uint64_t bySeqno,
                                           mcbp::systemevent::version version,
                                           cb::const_byte_buffer key,
                                           cb::const_byte_buffer eventData) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->system_event(cookie,
                                             opaque,
                                             vbucket,
                                             event,
                                             bySeqno,
                                             version,
                                             key,
                                             eventData);
    }
}

ENGINE_ERROR_CODE EWB_Engine::prepare(gsl::not_null<const void*> cookie,
                                      uint32_t opaque,
                                      const DocKey& key,
                                      cb::const_byte_buffer value,
                                      size_t priv_bytes,
                                      uint8_t datatype,
                                      uint64_t cas,
                                      Vbid vbucket,
                                      uint32_t flags,
                                      uint64_t by_seqno,
                                      uint64_t rev_seqno,
                                      uint32_t expiration,
                                      uint32_t lock_time,
                                      uint8_t nru,
                                      DocumentState document_state,
                                      cb::durability::Requirements durability) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->prepare(cookie,
                                        opaque,
                                        key,
                                        value,
                                        priv_bytes,
                                        datatype,
                                        cas,
                                        vbucket,
                                        flags,
                                        by_seqno,
                                        rev_seqno,
                                        expiration,
                                        lock_time,
                                        nru,
                                        document_state,
                                        durability);
    }
}
ENGINE_ERROR_CODE EWB_Engine::seqno_acknowledged(
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        Vbid vbucket,
        uint64_t in_memory_seqno,
        uint64_t on_disk_seqno) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->seqno_acknowledged(
                cookie, opaque, vbucket, in_memory_seqno, on_disk_seqno);
    }
}
ENGINE_ERROR_CODE EWB_Engine::commit(gsl::not_null<const void*> cookie,
                                     uint32_t opaque,
                                     Vbid vbucket,
                                     const DocKey& key,
                                     uint64_t prepared_seqno,
                                     uint64_t commit_seqno) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->commit(
                cookie, opaque, vbucket, key, prepared_seqno, commit_seqno);
    }
}

ENGINE_ERROR_CODE EWB_Engine::abort(gsl::not_null<const void*> cookie,
                                    uint32_t opaque,
                                    uint64_t prepared_seqno,
                                    uint64_t abort_seqno) {
    if (!real_engine_dcp) {
        return ENGINE_ENOTSUP;
    } else {
        return real_engine_dcp->abort(
                cookie, opaque, prepared_seqno, abort_seqno);
    }
}

cb::engine_errc EWB_Engine::collections_set_manifest(
        gsl::not_null<EngineIface*> handle,
        gsl::not_null<const void*> cookie,
        cb::const_char_buffer json) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->collections.set_manifest == nullptr) {
        return cb::engine_errc::not_supported;
    } else {
        return ewb->real_engine->collections.set_manifest(
                ewb->real_engine, cookie, json);
    }
}

cb::engine_errc EWB_Engine::collections_get_manifest(
        gsl::not_null<EngineIface*> handle,
        gsl::not_null<const void*> cookie,
        ADD_RESPONSE response) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->collections.get_manifest == nullptr) {
        return cb::engine_errc::not_supported;
    } else {
        return ewb->real_engine->collections.get_manifest(
                ewb->real_engine, cookie, response);
    }
}

cb::EngineErrorGetCollectionIDResult EWB_Engine::collections_get_collection_id(
        gsl::not_null<EngineIface*> handle,
        gsl::not_null<const void*> cookie,
        cb::const_char_buffer path) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->collections.get_collection_id == nullptr) {
        return {cb::engine_errc::not_supported, 0, 0};
    } else {
        return ewb->real_engine->collections.get_collection_id(
                ewb->real_engine, cookie, path);
    }
}

ENGINE_ERROR_CODE create_instance(GET_SERVER_API gsa, EngineIface** handle) {
    try {
        EWB_Engine* engine = new EWB_Engine(gsa);
        *handle = reinterpret_cast<EngineIface*>(engine);
        return ENGINE_SUCCESS;

    } catch (std::exception& e) {
        auto logger = gsa()->log->get_spdlogger();
        logger->warn("EWB_Engine: failed to create engine: {}", e.what());
        return ENGINE_FAILED;
    }
}

void destroy_engine(void) {
    // nothing todo.
}

const char* EWB_Engine::to_string(const Cmd cmd) {
    switch (cmd) {
    case Cmd::NONE:
        return "NONE";
    case Cmd::GET_INFO:
        return "GET_INFO";
    case Cmd::GET_META:
        return "GET_META";
    case Cmd::ALLOCATE:
        return "ALLOCATE";
    case Cmd::REMOVE:
        return "REMOVE";
    case Cmd::GET:
        return "GET";
    case Cmd::STORE:
        return "STORE";
    case Cmd::CAS:
        return "CAS";
    case Cmd::ARITHMETIC:
        return "ARITHMETIC";
    case Cmd::FLUSH:
        return "FLUSH";
    case Cmd::GET_STATS:
        return "GET_STATS";
    case Cmd::UNKNOWN_COMMAND:
        return "UNKNOWN_COMMAND";
    case Cmd::LOCK:
        return "LOCK";
    case Cmd::UNLOCK:
        return "UNLOCK";
    }
    throw std::invalid_argument("EWB_Engine::to_string() Unknown command");
}

void EWB_Engine::process_notifications() {
    SERVER_HANDLE_V1* server = gsa();
    LOG_DEBUG("EWB_Engine: notification thread running for engine {}",
              (void*)this);
    std::unique_lock<std::mutex> lk(mutex);
    while (!stop_notification_thread) {
        condvar.wait(lk);
        while (!pending_io_ops.empty()) {
            const void* cookie = pending_io_ops.front();
            pending_io_ops.pop();
            lk.unlock();
            LOG_DEBUG("EWB_Engine: notify {}", cookie);
            server->cookie->notify_io_complete(cookie, ENGINE_SUCCESS);
            lk.lock();
        }
    }

    LOG_DEBUG("EWB_Engine: notification thread stopping for engine {}",
              (void*)this);
}

void NotificationThread::run() {
    setRunning();
    engine.process_notifications();
}

ENGINE_ERROR_CODE EWB_Engine::handleBlockMonitorFile(const void* cookie,
                                                     uint32_t id,
                                                     const std::string& file,
                                                     ADD_RESPONSE response) {
    if (file.empty()) {
        return ENGINE_EINVAL;
    }

    if (!cb::io::isFile(file)) {
        return ENGINE_KEY_ENOENT;
    }

    if (!suspend(cookie, id)) {
        LOG_WARNING(
                "EWB_Engine::handleBlockMonitorFile(): "
                "Id {} already registered",
                id);
        return ENGINE_KEY_EEXISTS;
    }

    try {
        std::unique_ptr<Couchbase::Thread> thread(
                new BlockMonitorThread(*this, id, file));
        thread->start();
        std::lock_guard<std::mutex> guard(threads_mutex);
        threads.emplace_back(thread.release());
    } catch (std::exception& e) {
        LOG_WARNING(
                "EWB_Engine::handleBlockMonitorFile(): Failed to create "
                "block monitor thread: {}",
                e.what());
        return ENGINE_FAILED;
    }

    LOG_DEBUG(
            "Registered connection {} (engine {}) as {} to be"
            " suspended. Monitor file {}",
            cookie,
            (void*)this,
            id,
            file.c_str());

    response(nullptr,
             0,
             nullptr,
             0,
             nullptr,
             0,
             PROTOCOL_BINARY_RAW_BYTES,
             cb::mcbp::Status::Success,
             /*cas*/ 0,
             cookie);
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE EWB_Engine::handleSuspend(const void* cookie,
                                            uint32_t id,
                                            ADD_RESPONSE response) {
    if (suspend(cookie, id)) {
        LOG_DEBUG("Registered connection {} as {} to be suspended", cookie, id);
        response(nullptr,
                 0,
                 nullptr,
                 0,
                 nullptr,
                 0,
                 PROTOCOL_BINARY_RAW_BYTES,
                 cb::mcbp::Status::Success,
                 /*cas*/ 0,
                 cookie);
        return ENGINE_SUCCESS;
    } else {
        LOG_WARNING("EWB_Engine::handleSuspend(): Id {} already registered",
                    id);
        return ENGINE_KEY_EEXISTS;
    }
}

ENGINE_ERROR_CODE EWB_Engine::handleResume(const void* cookie, uint32_t id,
                                           ADD_RESPONSE response) {
    if (resume(id)) {
        LOG_DEBUG("Connection with id {} will be resumed", id);
        response(nullptr,
                 0,
                 nullptr,
                 0,
                 nullptr,
                 0,
                 PROTOCOL_BINARY_RAW_BYTES,
                 cb::mcbp::Status::Success,
                 /*cas*/ 0,
                 cookie);
        return ENGINE_SUCCESS;
    } else {
        LOG_WARNING(
                "EWB_Engine::unknown_command(): No "
                "connection registered with id {}",
                id);
        return ENGINE_EINVAL;
    }
}

ENGINE_ERROR_CODE EWB_Engine::setItemCas(const void *cookie,
                                         const std::string& key,
                                         uint32_t cas,
                                         ADD_RESPONSE response) {
    uint64_t cas64 = cas;
    if (cas == static_cast<uint32_t>(-1)) {
        cas64 = LOCKED_CAS;
    }

    auto rv = real_engine->get(cookie,
                               DocKey{key, DocKeyEncodesCollectionId::No},
                               Vbid(0),
                               DocStateFilter::Alive);
    if (rv.first != cb::engine_errc::success) {
        return ENGINE_ERROR_CODE(rv.first);
    }

    // item_set_cas has no return value!
    real_engine->item_set_cas(rv.second.get(), cas64);
    response(nullptr,
             0,
             nullptr,
             0,
             nullptr,
             0,
             PROTOCOL_BINARY_RAW_BYTES,
             cb::mcbp::Status::Success,
             0,
             cookie);
    return ENGINE_SUCCESS;
}

void BlockMonitorThread::run() {
    setRunning();

    LOG_DEBUG("Block monitor for file {} started", file);

    // @todo Use the file monitoring API's to avoid this "busy" loop
    while (cb::io::isFile(file)) {
        usleep(100);
    }

    LOG_DEBUG("Block monitor for file {} stopping (file is gone)", file);
    engine.resume(id);
}
