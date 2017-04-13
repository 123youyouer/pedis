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
#include "db.hh"
namespace redis {
database::database()
{
    using namespace std::chrono;
    _timer.set_callback([this] { expired_items(); });
    _store = &_data_storages[0];
}

database::~database()
{
}

bool database::set(redis_key&& rk, sstring&& val, long expire, uint32_t flag)
{
    return with_allocator(allocator(), [this, &rk, &val] {
        auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), val);
        _cache_store.replace(entry);
        return true;
    });
}

bool database::del(redis_key&& rk)
{
    return with_allocator(allocator(), [this, &rk] {
        return _cache_store.erase(rk);
    });
}

bool database::exists(redis_key&& rk)
{
    return _cache_store.exists(rk);
}

future<scattered_message_ptr> database::append(redis_key&& rk, sstring&& val)
{
    return with_allocator(allocator(), [this, &rk, &val] {
        return _cache_store.with_entry_run(rk, [this, &rk, &val] (cache_entry* e) {
            if (!e) {
                // not exists
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), val);
                _cache_store.replace(entry);
                return reply_builder::build(val.size());
            }
            if (!e->type_of_bytes()) {
                return reply_builder::build(msg_type_err);
            }
            return with_linearized_managed_bytes([this, e, &val] {
                size_t new_size = e->value_bytes_size() + val.size();
                auto data = std::unique_ptr<bytes_view::value_type[]>(new bytes_view::value_type[new_size]);
                std::copy_n(e->value_bytes_data(), e->value_bytes_size(), data.get());
                std::copy_n(val.data(), val.size(), data.get() + e->value_bytes_size());
                auto new_value = current_allocator().construct<managed_bytes>(data.get(), new_size);
                auto& old_value = e->value_bytes();
                current_allocator().destroy<managed_bytes>(&old_value);
                e->value_bytes() = std::move(*new_value);
                return reply_builder::build(new_size);
            });
        });
    });
}

future<scattered_message_ptr> database::get(redis_key&& rk)
{
    return with_linearized_managed_bytes([this, rk = std::move(rk)] {
        return _cache_store.with_entry_run(rk, [] (const cache_entry* e) {
           if (e && e->type_of_bytes() == false) {
               return reply_builder::build(msg_type_err);
           }
           else {
               return reply_builder::build<false, true>(e);
           }
        });
    });
}

future<scattered_message_ptr> database::strlen(redis_key&& rk)
{
    return with_linearized_managed_bytes([this, rk = std::move(rk)] {
        return _cache_store.with_entry_run(rk, [] (const cache_entry* e) {
            if (e) {
                if (e->type_of_bytes()) {
                    return reply_builder::build(e->value_bytes_size());
                }
                return reply_builder::build(msg_type_err);
            }
            return reply_builder::build(msg_zero);
        });
   });
}

int database::type(redis_key&& rk)
{
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return REDIS_ERR;
    }
    return static_cast<int>(it->type());
}

int database::expire(redis_key&& rk, long expired)
{
    auto expiry = expiration(expired);
    auto it = _store->fetch_raw(rk);
    if (it) {
        it->set_expiry(expiry);
        if (_alive.insert(it)) {
            _timer.rearm(it->get_timeout());
            return REDIS_OK;
        }
        it->set_never_expired();
    }
    return REDIS_ERR;
}

int database::persist(redis_key&& rk)
{
    auto it = _store->fetch_raw(rk);
    if (it) {
        it->set_never_expired();
        return REDIS_OK;
    }
    return REDIS_ERR;
}

future<scattered_message_ptr> database::push(redis_key&& rk, sstring&& value, bool force, bool left)
{
    return with_allocator(allocator(), [this, &rk, &value, force, left] () {
        return _cache_store.with_entry_run(rk, [this, &rk, val = std::move(value), force, left] (cache_entry* o) {
            auto e = o;
            if (!e) {
                 if (!force) {
                     return reply_builder::build(msg_err);
                 }
                // create new list object
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::list_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            left ? list.insert_head(val) : list.insert_tail(val);
            return reply_builder::build(list.size());
        });
    });
}

future<scattered_message_ptr> database::push_multi(redis_key&& rk, std::vector<sstring>&& values, bool force, bool left)
{
    return with_allocator(allocator(), [this, &rk, &values, force, left] () {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), vals = std::move(values), force, left] (cache_entry* o) {
            auto e = o;
            if (!e) {
                 if (!force) {
                     return reply_builder::build(msg_err);
                 }
                // create new list object
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::list_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            for (auto& val : vals) {
                left ? list.insert_head(val) : list.insert_tail(val);
            }
            return reply_builder::build(list.size());
        });
    });
}

future<scattered_message_ptr> database::pop(redis_key&& rk, bool left)
{
    return with_allocator(allocator(), [this, &rk, left] () {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), left] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_nil);
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            return with_linearized_managed_bytes([this, &rk, e, left] () {
                auto& list = e->value_list();
                assert(!list.empty());
                auto reply = reply_builder::build(left ? list.front() : list.back());
                left ? list.pop_front() : list.pop_back();
                if (list.empty()) {
                   _cache_store.erase(rk);
                }
                return reply;
            });
        });
    });
}

future<scattered_message_ptr> database::llen(redis_key&& rk)
{
    return _cache_store.with_entry_run(rk, [rk = std::move(rk)] (const cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_zero);
        }
        if (e->type_of_list() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& list = e->value_list();
        return reply_builder::build(list.size());
    });
}

future<scattered_message_ptr> database::lindex(redis_key&& rk, long idx)
{
    return _cache_store.with_entry_run(rk, [rk = std::move(rk), idx] (const cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_nil);
        }
        if (e->type_of_list() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& list = e->value_list();
        auto index = alignment_index_base_on(list.size(), idx);
        if (list.index_out_of_range(index)) {
            return reply_builder::build(msg_nil);
        }
        auto& result = list.at(static_cast<size_t>(index));
        return reply_builder::build(result);
    });
}

future<scattered_message_ptr> database::lrange(redis_key&& rk, long start, long end)
{
    return _cache_store.with_entry_run(rk, [rk = std::move(rk), &start, &end] (const cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_err);
        }
        if (e->type_of_list() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& list = e->value_list();
        start = database::alignment_index_base_on(list.size(), start);
        end = database::alignment_index_base_on(list.size(), end);
        if (start < 0) start = 0;
        if (end >= static_cast<long>(list.size())) end = static_cast<size_t>(list.size()) - 1;
        std::vector<const managed_bytes*> data;
        if (start < end) {
           for (auto i = start; i < end; ++i) {
              const auto& b = list.at(static_cast<size_t>(i));
              data.push_back(&b);
           }
        }
        return reply_builder::build(data);
    });
}

future<scattered_message_ptr> database::lrem(redis_key&& rk, long count, sstring&& value)
{
    return with_allocator(allocator(), [this, &rk, count, &value] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), val = std::move(value), &count] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_err);
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            size_t removed = 0;
            if (count == 0) removed = list.trem<true, true>(val, count);
            else if (count > 0) removed = list.trem<false, true>(val, count);
            else removed = list.trem<false, false>(val, count);
            if (list.empty()) {
                _cache_store.erase(rk);
            }
            return reply_builder::build(removed);
        });
    });
}

future<scattered_message_ptr> database::linsert(redis_key&& rk, sstring&& pivot, sstring&& value, bool after)
{
    return with_allocator(allocator(), [this, &rk, &pivot, &value, after] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), val = std::move(value), pivot = std::move(pivot), after] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_zero);
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            auto index = list.index_of(pivot);
            if (list.index_out_of_range(index)) {
                return reply_builder::build(msg_zero);
            }
            if (after) {
                if (index == list.size()) list.insert_tail(val);
                else  list.insert_at(++index, val);
            }
            else {
               if (index == 0) list.insert_head(val);
               else list.insert_at(--index, val);
            }
            return reply_builder::build(msg_one);
        });
    });
}

future<scattered_message_ptr> database::lset(redis_key&& rk, long idx, sstring&& value)
{
    return with_allocator(allocator(), [this, &rk, idx, &value] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), idx, val = std::move(value)] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_nokey_err);
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            auto nidx = database::alignment_index_base_on(list.size(), idx);
            if (list.index_out_of_range(nidx)) {
                return reply_builder::build(msg_out_of_range_err);
            }
            auto& old = list.at(static_cast<size_t>(nidx));
            current_allocator().destroy<managed_bytes>(&old);
            auto entry = current_allocator().construct<managed_bytes>(bytes_view{reinterpret_cast<const signed char*>(val.data()), val.size()});
            list.at(idx) = std::move(*entry);
            return reply_builder::build(msg_ok);
        });
    });
}

future<scattered_message_ptr> database::ltrim(redis_key&& rk, long start, long end)
{
    return with_allocator(allocator(), [this, &rk, start, end] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), start, end] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_ok);
            }
            if (e->type_of_list() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& list = e->value_list();
            auto nstart = database::alignment_index_base_on(list.size(), start);
            if (nstart < 0) nstart = 0;
            auto nend = database::alignment_index_base_on(list.size(), end);
            if (nstart > nend || nstart > static_cast<long>(list.size()) || nend < 0) {
                list.clear();
            }
            list.trim(static_cast<size_t>(nstart), static_cast<size_t>(nend));
            if (list.empty()) {
                _cache_store.erase(rk);
            }
            return reply_builder::build(msg_ok);
        });
    });
}

future<scattered_message_ptr> database::hset(redis_key&& rk, sstring&& field, sstring&& value)
{
    return with_allocator(allocator(), [this, &rk, &field, &value] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), key = std::move(field), val = std::move(value)] (cache_entry* o) {
            auto e = o;
            if (!e) {
                // the rk was not exists, then create it.
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::dict_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            bool exists = map.exists(key);
            auto entry = current_allocator().construct<dict_entry>(key, val);
            map.insert(entry);
            return reply_builder::build(exists ? msg_zero : msg_one);
        });
    });
}

future<scattered_message_ptr> database::hincrby(redis_key&& rk, sstring&& key, int64_t delta)
{
    return with_allocator(allocator(), [this, &rk, &key, delta] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), key = std::move(key), delta] (cache_entry* o) {
            auto e = o;
            if (!e) {
                // the rk was not exists, then create it.
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::dict_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            return map.with_entry_run(key, [&map, &key, delta] (dict_entry* d) {
                if (!d) {
                    auto entry = current_allocator().construct<dict_entry>(key, delta);
                    map.insert(entry);
                    return reply_builder::build<false, true>(entry);
                }
                if (!d->type_of_integer()) {
                    return reply_builder::build(msg_not_integer_err);
                }
                d->value_integer_incr(delta);
                return reply_builder::build<false, true>(d);
            });
        });
    });
}

future<scattered_message_ptr> database::hincrbyfloat(redis_key&& rk, sstring&& key, double delta)
{
    return with_allocator(allocator(), [this, &rk, &key, delta] {
        return _cache_store.with_entry_run(rk, [this, rk = std::move(rk), key = std::move(key), delta] (cache_entry* o) {
            auto e = o;
            if (!e) {
                // the rk was not exists, then create it.
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::dict_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            return map.with_entry_run(key, [&map, &key, delta] (dict_entry* d) {
                if (!d) {
                    auto entry = current_allocator().construct<dict_entry>(key, delta);
                    map.insert(entry);
                    return reply_builder::build<false, true>(entry);
                }
                if (!d->type_of_float()) {
                    return reply_builder::build(msg_not_float_err);
                }
                d->value_float_incr(delta);
                return reply_builder::build<false, true>(d);
            });
        });
    });
}

future<scattered_message_ptr> database::hmset(redis_key&& rk, std::unordered_map<sstring, sstring>&& kvs)
{
    return with_allocator(allocator(), [this, &rk, &kvs] {
        return _cache_store.with_entry_run(rk, [this, &rk, kvs = std::move(kvs)] (cache_entry* o) {
            auto e = o;
            if (!e) {
                // the rk was not exists, then create it.
                auto entry = current_allocator().construct<cache_entry>(rk.key(), rk.hash(), cache_entry::dict_initializer());
                _cache_store.insert(entry);
                e = entry;
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            bool result = false;
            for (auto& kv : kvs) {
               auto entry = current_allocator().construct<dict_entry>(kv.first, kv.second);
               result = map.insert(entry);
               if (!result) break;
            }
            return reply_builder::build(result ? msg_ok : msg_err);
        });
    });
}

future<scattered_message_ptr> database::hget(redis_key&& rk, sstring&& field)
{
    return _cache_store.with_entry_run(rk, [this, key = std::move(field)] (cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_err);
        }
        if (e->type_of_map() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& map = e->value_map();
        return map.with_entry_run(key, [] (const dict_entry* d) {
            return with_linearized_managed_bytes([d] {
                return reply_builder::build<false, true>(d);
            });
        });
    });
}

future<scattered_message_ptr> database::hdel_multi(redis_key&& rk, std::vector<sstring>&& keys)
{
    return with_allocator(allocator(), [this, &rk, &keys] {
        return _cache_store.with_entry_run(rk, [this, &rk, &keys] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_zero);
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            size_t removed = 0;
            for (auto& key : keys) {
                if (map.erase(key)) {
                    ++ removed;
                }
            }
            if (map.empty()) {
                _cache_store.erase(rk);
            }
            return reply_builder::build(removed);
        });
    });
}


future<scattered_message_ptr> database::hdel(redis_key&& rk, sstring&& key)
{
    return with_allocator(allocator(), [this, &rk, &key] {
        return _cache_store.with_entry_run(key, [this, &rk, &key] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_zero);
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            bool exists = map.erase(key);
            if (map.empty()) {
                _cache_store.erase(rk);
            }
            return reply_builder::build(exists ? msg_ok : msg_err);
        });
    });
}

future<scattered_message_ptr> database::hexists(redis_key&& rk, sstring&& key)
{
    return with_allocator(allocator(), [this, &rk, &key] {
        return _cache_store.with_entry_run(rk, [this, &key] (cache_entry* e) {
            if (!e) {
                return reply_builder::build(msg_zero);
            }
            if (e->type_of_map() == false) {
                return reply_builder::build(msg_type_err);
            }
            auto& map = e->value_map();
            return map.with_entry_run(key, [] (const dict_entry* d) {
                if (!d) {
                    return reply_builder::build(msg_zero);
                }
                return reply_builder::build(msg_one);
            });
        });
    });
}

future<scattered_message_ptr> database::hstrlen(redis_key&& rk, sstring&& key)
{
    return _cache_store.with_entry_run(rk, [this, &key] (cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_zero);
        }
        if (e->type_of_map() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& map = e->value_map();
        return map.with_entry_run(key, [] (const dict_entry* d) {
            if (!d) {
                return reply_builder::build(msg_zero);
            }
            return reply_builder::build(d->value_bytes_size());
        });
    });
}

future<scattered_message_ptr> database::hlen(redis_key&& rk)
{
    return _cache_store.with_entry_run(rk, [this] (cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_zero);
        }
        if (e->type_of_map() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& map = e->value_map();
        return reply_builder::build(map.size());
    });
}

future<scattered_message_ptr> database::hgetall(redis_key&& rk)
{
    return hgetall_impl<true, true>(std::move(rk));
}

future<scattered_message_ptr> database::hgetall_values(redis_key&& rk)
{
    return hgetall_impl<false, true>(std::move(rk));
}

future<scattered_message_ptr> database::hgetall_keys(redis_key&& rk)
{
    return hgetall_impl<true, false>(std::move(rk));
}


future<scattered_message_ptr> database::hmget(redis_key&& rk, std::vector<sstring>&& keys)
{
    return _cache_store.with_entry_run(rk, [this, &keys] (const cache_entry* e) {
        if (!e) {
            return reply_builder::build(msg_err);
        }
        if (e->type_of_map() == false) {
            return reply_builder::build(msg_type_err);
        }
        auto& map = e->value_map();
        std::vector<const dict_entry*> entries;
        map.fetch(keys, entries);
        return reply_builder::build<false, true>(entries);
    });
}

std::pair<size_t, int> database::scard(redis_key&& rk)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    else if(it->type() != REDIS_SET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto set = it->dict_ptr();
    return result_type {set->size(), REDIS_OK};
}

int database::sismember(redis_key&& rk, sstring&& member)
{
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return REDIS_ERR;
    }
    else if(it->type() != REDIS_SET) {
        return REDIS_WRONG_TYPE;
    }
    auto _set = it->dict_ptr();
    redis_key member_data {std::move(member)};
    return _set->exists(member_data);
}

std::pair<std::vector<item_ptr>, int> database::smembers(redis_key&& rk)
{
    using result_type = std::pair<std::vector<item_ptr>, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_ERR};
    }
    if (it->type() == REDIS_SET) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_WRONG_TYPE};
    }
    auto _set = it->dict_ptr();
    return result_type {std::move(_set->fetch()), REDIS_OK};
}

std::pair<item_ptr, int> database::spop(redis_key&& rk)
{
    using result_type = std::pair<item_ptr, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {nullptr, REDIS_ERR};
    }
    if (it->type() == REDIS_SET) {
        return result_type {nullptr, REDIS_WRONG_TYPE};
    }
    auto _set = it->dict_ptr();
    return result_type {std::move(_set->random_fetch_and_remove()), REDIS_OK};
}

int database::srem(redis_key&& rk, sstring&& member)
{
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return REDIS_ERR;
    }
    if (it->type() == REDIS_SET) {
        return REDIS_WRONG_TYPE;
    }
    auto _set = it->dict_ptr();
    redis_key member_data {std::move(member)};
    return _set->remove(member_data);
}

std::pair<size_t, int> database::srems(redis_key&& rk, std::vector<sstring>&& members)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    if (it->type() == REDIS_SET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto _set = it->dict_ptr();
    int count = 0;
    if (_set != nullptr) {
        for (sstring& member : members) {
            redis_key member_data {std::move(member)};
            if (_set->remove(member_data) == REDIS_OK) {
                count++;
            }
        }
        if (_set->size() == 0) {
            _store->remove(rk);
        }
    }
    return result_type {count, REDIS_OK};
}

long database::pttl(redis_key&& rk)
{
    /*
    auto item = get(std::move(rk));
    if (item) {
        if (item->ever_expires() == false) {
            return -1;
        }
        auto duration = item->get_timeout() - clock_type::now();
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    }
    else {
        return -2;
    }
    */
    return -2;
}

long database::ttl(redis_key&& rk)
{
    auto ret = pttl(std::move(rk));
    if (ret > 0) {
        return ret / 1000;
    }
    return ret;
}

std::pair<size_t, int> database::zcard(redis_key&& rk)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {zset->size(), REDIS_OK};
}
std::pair<size_t, int> database::zrem(redis_key&& rk, std::vector<sstring>&& members)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    size_t count = 0;
    for (size_t i = 0; i < members.size(); ++i) {
        redis_key m{std::move(members[i])};
        if (zset->remove(m) == REDIS_OK) {
            count++;
        }
    }
    if (zset->size() == 0) {
        _store->remove(rk);
    }
    return result_type {count, REDIS_OK};
}
std::pair<size_t, int> database::zcount(redis_key&& rk, double min, double max)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {zset->count(min, max), REDIS_OK};
}

std::pair<std::vector<item_ptr>, int> database::zrange(redis_key&& rk, long begin, long end, bool reverse)
{
    using result_type = std::pair<std::vector<item_ptr>, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_OK};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {std::move(zset->range_by_rank(begin, end, reverse)), REDIS_OK};
}

std::pair<std::vector<item_ptr>, int> database::zrangebyscore(redis_key&& rk, double min, double max, bool reverse)
{
    using result_type = std::pair<std::vector<item_ptr>, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_OK};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type {std::move(std::vector<item_ptr>()), REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {std::move(zset->range_by_score(min, max, reverse)), REDIS_OK};
}

std::pair<size_t, int> database::zrank(redis_key&& rk, sstring&& member, bool reverse)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
       return result_type {0, REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    redis_key m{std::move(member)};
    return result_type {zset->rank(m, reverse), REDIS_OK};
}

std::pair<double, int> database::zscore(redis_key&& rk, sstring&& member)
{
    using result_type = std::pair<double, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    if (it->type() != REDIS_ZSET) {
        return result_type{0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    redis_key mk {std::move(member)};
    auto value = zset->fetch(mk);
    if (!value) {
        return result_type{0, REDIS_ERR};
    }
    return result_type{value->Double(), REDIS_OK};
}

std::pair<size_t, int> database::zremrangebyscore(redis_key&& rk, double min, double max)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_OK};
    }
    if (it && it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {zset->remove_range_by_score(min, max), REDIS_OK};
}
std::pair<size_t, int> database::zremrangebyrank(redis_key&& rk, size_t begin, size_t end)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_OK};
    }
    if (it && it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto zset = it->zset_ptr();
    return result_type {zset->remove_range_by_rank(begin, end), REDIS_OK};
}

int database::select(int index)
{
    _store = &_data_storages[index];
    return REDIS_OK;
}

std::pair<double, int> database::geodist(redis_key&& rk, sstring&& lpos, sstring&& rpos, int flag)
{
    using result_type = std::pair<double, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto _zset = it->zset_ptr();
    auto lmember = _zset->fetch(redis_key {std::move(lpos)});
    auto rmember = _zset->fetch(redis_key {std::move(rpos)});
    if (!lmember || !rmember) {
        return result_type {0, REDIS_ERR};
    }
    double factor = 1;
    if (flag & GEODIST_UNIT_M) {
        factor = 1;
    }
    else if (flag & GEODIST_UNIT_KM) {
        factor = 1000;
    }
    else if (flag & GEODIST_UNIT_MI) {
        factor = 0.3048;
    }
    else if (flag & GEODIST_UNIT_FT) {
        factor = 1609.34;
    }
    double lscore = lmember->Double(), rscore = rmember->Double(), dist = 0;
    if (geo::dist(lscore, rscore, dist)) {
        return result_type {dist / factor, REDIS_OK};
    }
    return result_type {0, REDIS_ERR};
}

std::pair<std::vector<sstring>, int> database::geohash(redis_key&& rk, std::vector<sstring>&& members)
{
    using result_type = std::pair<std::vector<sstring>, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {std::move(std::vector<sstring>()), REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return result_type {std::move(std::vector<sstring>()), REDIS_WRONG_TYPE};
    }
    auto _zset = it->zset_ptr();
    auto&& items = _zset->fetch(members);
    std::vector<sstring> geohash_set;
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        sstring hashstr;
        if (geo::encode_to_geohash_string(item->Double(), hashstr) == false) {
            return result_type {std::move(std::vector<sstring>()), REDIS_ERR};
        }
        geohash_set.emplace_back(std::move(hashstr));;
    }
    return result_type {std::move(geohash_set), REDIS_OK};
}

std::pair<std::vector<std::tuple<double, double, bool>>, int> database::geopos(redis_key&& rk, std::vector<sstring>&& members)
{
    using result_type = std::pair<std::vector<std::tuple<double, double, bool>>, int>;
    using result_data_type = std::vector<std::tuple<double, double, bool>>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {std::move(result_data_type()), REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return result_type {std::move(result_data_type()), REDIS_WRONG_TYPE};
    }
    auto _zset = it->zset_ptr();
    auto&& items = _zset->fetch(members);
    result_data_type result;
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        double longitude = 0, latitude = 0;
        if (item) {
            if (geo::decode_from_geohash(item->Double(), longitude, latitude)) {
                result.emplace_back(std::move(std::tuple<double, double, bool>(longitude, latitude, true)));
            }
        }
        else {
            result.emplace_back(std::move(std::tuple<double, double, bool>(0, 0, false)));
        }
    }
    return result_type {std::move(result), REDIS_OK};
}

database::georadius_result_type database::georadius_coord(redis_key&& rk, double longitude, double latitude, double radius, size_t count, int flag)
{
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return georadius_result_type {{}, REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return georadius_result_type {{}, REDIS_WRONG_TYPE};
    }
    auto _zset = it->zset_ptr();
    return georadius(_zset, longitude, latitude, radius, count, flag);
}

database::georadius_result_type database::georadius_member(redis_key&& rk, sstring&& pos, double radius, size_t count, int flag)
{
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return georadius_result_type {{}, REDIS_ERR};
    }
    else if (it->type() != REDIS_ZSET) {
        return georadius_result_type {{}, REDIS_WRONG_TYPE};
    }
    auto _zset = it->zset_ptr();
    auto member = _zset->fetch(redis_key {std::move(pos)});
    if (!member) {
        return georadius_result_type {{}, REDIS_ERR};
    }
    double score = member->Double(), longitude = 0, latitude = 0;
    if (geo::decode_from_geohash(score, longitude, latitude) == false) {
        return georadius_result_type {{}, REDIS_ERR};
    }
    return georadius(_zset, longitude, latitude, radius, count, flag);
}

database::georadius_result_type database::georadius(sorted_set* zset, double longitude, double latitude, double radius, size_t count, int flag)
{
    using data_type = std::vector<std::tuple<sstring, double, double, double, double>>;
    data_type points;
    auto fetch_point = [zset, count] (double min, double max, double log, double lat, double r, data_type& points) -> size_t {
        return zset->range_by_score_if(min, max, count, [&log, &lat, &r, &points] (lw_shared_ptr<item> m) -> bool {
            double score = m->Double(), longitude = 0, latitude = 0, dist = 0;
            if (geo::decode_from_geohash(score, longitude, latitude) == false) {
                return false;
            }
            if (geo::dist(log, lat, longitude, latitude, dist) == false) {
                return false;
            }
            if (dist < r) {
                sstring n(m->key().data(), m->key().size());
                points.emplace_back(std::move(std::tuple<sstring, double, double, double, double>{std::move(n), score, dist, longitude, latitude}));
                return true;
            }
            return false;
        });
    };
    if (geo::fetch_points_from_location(longitude, latitude, radius, std::move(fetch_point), points) == false) {
        points.clear();
        return georadius_result_type{std::move(points), REDIS_ERR};
    }
    if (flag & GEORADIUS_ASC) {
        std::sort(points.begin(), points.end(), [] (const auto& l, const auto& r) { return std::get<2>(l) > std::get<2>(r); });
    }
    else if (flag & GEORADIUS_DESC) {
        std::sort(points.begin(), points.end(), [] (const auto& l, const auto& r) { return std::get<2>(l) < std::get<2>(r); });
    }
    return georadius_result_type {std::move(points), REDIS_OK};
}

std::pair<bool, int> database::setbit(redis_key&& rk, size_t offset, bool value)
{
    using result_type = std::pair<bool, int>;
    if (offset >= BITMAP_MAX_OFFSET) {
        return result_type {false, REDIS_ERR};
    }
    auto it = _store->fetch_raw(rk);
    bitmap* bm = nullptr;
    if (!it) {
        bm = new bitmap();
        auto new_item = item::create(rk, bm);
        _store->set(rk, new_item);
    }
    else if (it->type() != REDIS_BITMAP) {
        return result_type {false, REDIS_WRONG_TYPE};
    }
    else {
        bm = it->bitmap_ptr();
    }
    return result_type {bm->set_bit(offset, value), REDIS_OK};
}

std::pair<bool, int> database::getbit(redis_key&& rk, size_t offset)
{
    using result_type = std::pair<bool, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {false, REDIS_OK};
    }
    else if (it->type() != REDIS_BITMAP) {
        return result_type {false, REDIS_WRONG_TYPE};
    }
    auto bm = it->bitmap_ptr();
    return result_type {bm->get_bit(offset), REDIS_OK};
}

std::pair<size_t, int> database::bitcount(redis_key&& rk, long start, long end)
{
    using result_type = std::pair<size_t, int>;
    auto it = _store->fetch_raw(rk);
    if (!it) {
        return result_type {0, REDIS_OK};
    }
    else if (it->type() != REDIS_BITMAP) {
        return result_type {0, REDIS_WRONG_TYPE};
    }
    auto bm = it->bitmap_ptr();
    return result_type {bm->bit_count(start, end), REDIS_OK};
}

std::pair<size_t, int> database::bitpos(redis_key&& rk, bool bit, long start, long end)
{
    using result_type = std::pair<size_t, int>;
    return result_type {0, REDIS_OK};
}

future<> database::stop()
{
    return make_ready_future<>();
}

void database::expired_items()
{
    using namespace std::chrono;
    auto exp = _alive.expire(clock_type::now());
    while (!exp.empty()) {
        auto it = *exp.begin();
        exp.pop_front();
        // release expired item
        if (it && it->ever_expires()) {
            _store->remove(it);
        }
    }
    _timer.arm(_alive.get_next_timeout());
}

}
