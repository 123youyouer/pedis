#pragma once
#include "storage.hh"
#include "base.hh"

namespace redis {
using item_ptr = foreign_ptr<lw_shared_ptr<item>>;
class misc_storage : public storage {
public:
    misc_storage(const sstring& name, dict* store) : storage(name, store)
    {
    }
    virtual ~misc_storage()
    {
    }
    struct stats {
        uint64_t key_count_;
    };

    /** COUNTER API **/
    template <typename origin = local_origin_tag>
    uint64_t counter_by(sstring& key, uint64_t step, bool incr)
    {
        redis_key rk{key};
        auto it = _store->fetch_raw(rk);
        if (it) {
            if (it->type() != REDIS_RAW_UINT64) {
                return REDIS_ERR;
            }
            return it->incr(incr ? step : -step);
        } else {
            auto new_item = item::create(key, step);
            if (_store->set(rk, new_item) != REDIS_OK)
                return REDIS_ERR;
            return step;
        }
    }

    /** STRING API **/
    template <typename origin = local_origin_tag>
    int set(const redis_key& rk, sstring& val, long expire, uint32_t flag)
    {
        _store->remove(rk);
        auto new_item = item::create(rk, origin::move_if_local(val));
        return _store->set(rk, new_item);
    }

    template <typename origin = local_origin_tag>
    int append(sstring& key, sstring& val)
    {
        redis_key rk{key};
        size_t current_size = -1;
        auto it = _store->fetch_raw(rk);
        if (it) {
            auto exist_val = it->value();
            current_size = exist_val.size() + val.size();
            auto new_item = item::create(key,
                    origin::move_if_local(exist_val),
                    origin::move_if_local(val));
            if (_store->replace(rk, new_item) != 0) {
                return -1;
            }
        }
        else {
            current_size = val.size();
            auto new_item = item::create(key, origin::move_if_local(val));
            if (_store->set(rk, new_item) != 0) {
                return -1;
            }
        }
        return current_size;
    }

    int del(sstring& key)
    {
        redis_key rk{key};
        return _store->remove(rk) == REDIS_OK ? 1 : 0;
    }

    int exists(sstring& key)
    {
        redis_key rk{key};
        return _store->exists(rk);
    }

    item_ptr get(sstring& key)
    {
        redis_key rk{key};
        return _store->fetch(rk);
    }

    int strlen(sstring& key)
    {
        redis_key rk{key};
        auto i = _store->fetch(rk);
        if (i) {
            return i->value_size();
        }
        return 0;
    }

    int expire(sstring& key, long expired)
    {
        redis_key rk{key};
        auto it = _store->fetch_raw(rk);
        if (!it) {
            return REDIS_ERR;
        }
        return REDIS_OK;
    }
protected:
    stats _stats;
};
}
