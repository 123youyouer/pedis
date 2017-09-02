/*
* Pedis is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* You may obtain a copy of the License at
*
*     http://www.gnu.org/licenses
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*
*  Copyright (c) 2016-2026, Peng Jian, pstack@163.com. All rights reserved.
*
*/
#pragma once
#include "core/sharded.hh"
#include "core/distributed.hh"
#include "core/sstring.hh"
#include <experimental/optional>
#include <unordered_map>
#include <vector>
#include "gms/inet_address.hh"
#include "common.hh"
#include "token.hh"
#include "keys.hh"
namespace redis {
class ring final {
public:
    ring() {}
    ~ring() {}

    ring(const ring&) = delete;
    ring& operator = (const ring&) = delete;
    ring(ring&&) = delete;
    ring& operator = (ring&&) = delete;

    const std::vector<gms::inet_address> get_replica_nodes_for_write(const redis_key& rk);
    const gms::inet_address get_replica_node_for_read(const redis_key& rk);
    const size_t get_replica_count() const { return _replica_count; }
    void set_sorted_tokens(const std::vector<token>& tokens, const std::unordered_map<token, gms::inet_address>& token_to_endpoint);

    bool is_member(const gms::inet_address& endpoint) const { return true; }
    std::chrono::milliseconds get_ring_delay() const;
    std::vector<token> get_tokens(const gms::inet_address& endpoint) const { return {}; }
    future<> start();
    future<> stop();
private:
    size_t _replica_count = 1;
    size_t _vnode_count = 1023;
    std::vector<token> _sorted_tokens {};
    std::unordered_map<token, gms::inet_address> _token_to_endpoint {};

    // FIXME: need a timer to evict elements by LRU?
    std::unordered_map<token, std::vector<gms::inet_address>> _token_write_targets_endpoints_cache {};
    std::unordered_map<token, gms::inet_address>              _token_read_targets_endpoints_cache {};

    const std::vector<gms::inet_address> get_replica_nodes_internal(const redis_key& rk);
    size_t token_to_index(const token& t) const;
};
}
