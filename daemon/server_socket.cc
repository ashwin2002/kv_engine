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

#include "server_socket.h"

#include "connections.h"
#include "memcached.h"
#include "network_interface.h"
#include "settings.h"
#include "stats.h"

#include <logger/logger.h>
#include <nlohmann/json.hpp>
#include <platform/socket.h>
#include <platform/strerror.h>
#include <exception>
#include <memory>
#include <string>
#ifndef WIN32
#include <sys/resource.h>
#endif

ServerSocket::ServerSocket(SOCKET fd,
                           event_base* b,
                           in_port_t port,
                           sa_family_t fam,
                           const NetworkInterface& interf)
    : sfd(fd),
      listen_port(port),
      family(fam),
      sockname(cb::net::getsockname(fd)),
      ev(event_new(b,
                   sfd,
                   EV_READ | EV_PERSIST,
                   listen_event_handler,
                   reinterpret_cast<void*>(this))) {
    if (!ev) {
        throw std::bad_alloc();
    }

    enable();
}

ServerSocket::~ServerSocket() {
    disable();
}

void ServerSocket::enable() {
    if (!registered_in_libevent) {
        LOG_INFO("{} Listen on {}", sfd, sockname);
        if (cb::net::listen(sfd, backlog) == SOCKET_ERROR) {
            LOG_WARNING("{}: Failed to listen on {}: {}",
                        sfd,
                        sockname,
                        cb_strerror(cb::net::get_socket_error()));
        }

        if (event_add(ev.get(), nullptr) == -1) {
            LOG_WARNING("Failed to add connection to libevent: {}",
                        cb_strerror());
        } else {
            registered_in_libevent = true;
        }
    }
}

void ServerSocket::disable() {
    if (registered_in_libevent) {
        if (sfd != INVALID_SOCKET) {
            /*
             * Try to reduce the backlog length so that clients
             * may get ECONNREFUSED instead of blocking. Note that the
             * backlog parameter is a hint, so the actual value being
             * used may be higher than what we try to set it.
             */
            if (cb::net::listen(sfd, 1) == SOCKET_ERROR) {
                LOG_WARNING("{}: Failed to set backlog to 1 on {}: {}",
                            sfd,
                            sockname,
                            cb_strerror(cb::net::get_socket_error()));
            }
        }
        if (event_del(ev.get()) == -1) {
            LOG_WARNING("Failed to remove connection to libevent: {}",
                        cb_strerror());
        } else {
            registered_in_libevent = false;
        }
    }
}

void ServerSocket::acceptNewClient() {
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(addr);
    auto client = cb::net::accept(
            sfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);

    if (client == INVALID_SOCKET) {
        auto error = cb::net::get_socket_error();
        if (cb::net::is_emfile(error)) {
#if defined(WIN32)
            LOG_WARNING("Too many open files.");
#else
            struct rlimit limit = {0};
            getrlimit(RLIMIT_NOFILE, &limit);
            LOG_WARNING("Too many open files. Current limit: {}",
                        limit.rlim_cur);
#endif
            disable_listen();
        } else if (!cb::net::is_blocking(error)) {
            LOG_WARNING("Failed to accept new client: {}", cb_strerror(error));
        }

        return;
    }

    int port_conns;
    ListeningPort* port_instance;
    int curr_conns = stats.curr_conns.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(stats_mutex);
        port_instance = get_listening_port_instance(listen_port);
        cb_assert(port_instance);
        port_conns = ++port_instance->curr_conns;
    }

    if (curr_conns >= settings.getMaxconns() ||
        port_conns >= port_instance->maxconns) {
        {
            std::lock_guard<std::mutex> guard(stats_mutex);
            --port_instance->curr_conns;
        }
        stats.rejected_conns++;
        LOG_WARNING(
                "Too many open connections. Current/Limit for port "
                "{}: {}/{}; total: {}/{}",
                port_instance->port,
                port_conns,
                port_instance->maxconns,
                curr_conns,
                settings.getMaxconns());

        safe_close(client);
        return;
    }

    if (cb::net::set_socket_noblocking(client) == -1) {
        {
            std::lock_guard<std::mutex> guard(stats_mutex);
            --port_instance->curr_conns;
        }
        LOG_WARNING("Failed to make socket non-blocking. closing it");
        safe_close(client);
        return;
    }

    dispatch_conn_new(client, listen_port);
}

nlohmann::json ServerSocket::toJson() const {
    nlohmann::json ret;

    {
        std::lock_guard<std::mutex> guard(stats_mutex);
        const auto* instance = get_listening_port_instance(listen_port);
        if (!instance) {
            throw std::runtime_error(
                    R"(ServerSocket::toJson: Failed to look up instance for port: )" +
                    std::to_string(listen_port));
        }

        ret["ssl"] = instance->getSslSettings().get() != nullptr;
    }
    ret["protocol"] = "memcached";

    if (family == AF_INET) {
        ret["family"] = "AF_INET";
    } else {
        ret["family"] = "AF_INET6";
    }

    ret["name"] = sockname;
    ret["port"] = listen_port;

    return ret;
}
