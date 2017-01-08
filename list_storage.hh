#pragma once
#include "storage.hh"
#include "base.hh"
#include "list.hh"
namespace redis {
class list_storage : public storage {
public:
    list_storage(const sstring& name, dict* store) : storage(name, store)
    {
    }
    virtual ~list_storage()
    {
    }
    struct stats {
        uint64_t list_count_ = 0;
        uint64_t list_node_count_ = 0;
    };
    template<typename origin = local_origin_tag>
    int push(redis_key& key, sstring& value, bool force, bool left)
    {
        list* l = fetch_list(key);
        if (l == nullptr) {
            if (!force) {
                return -1;
            }
            const size_t list_size = item::item_size_for_list(key.size());
            l = new list();
            auto list_item = local_slab().create(list_size, key, l, REDIS_LIST);
            intrusive_ptr_add_ref(list_item);
            if (_store->set(origin::move_if_local(key), list_item) != 0) {
                intrusive_ptr_release(list_item);
                return -1;
            }
        }
        const size_t item_size = item::item_size_for_raw_string(static_cast<size_t>(value.size()));
        auto new_item = local_slab().create(item_size, origin::move_if_local(value));
        intrusive_ptr_add_ref(new_item);
        return (left ? l->add_head(new_item) : l->add_tail(new_item)) == 0 ? static_cast<int>(l->length()) : 0;
    }

    item_ptr pop(redis_key& key, bool left)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            auto it = left ? l->pop_head() : l->pop_tail();
            if (l->length() == 0) {
                _store->remove(key);
            }
            return it;
        } 
        return nullptr;
    }

    int llen(redis_key& key)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            return l->length();
        } 
        return 0;
    }

    item_ptr lindex(redis_key& key, int idx)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            return l->index(idx);
        } 
        return nullptr;
    }
  
    template<typename origin = local_origin_tag>
    int linsert(redis_key& key, sstring& pivot, sstring& value, bool after)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            const size_t item_size = item::item_size_for_raw_string(value.size());
            auto new_item = local_slab().create(item_size, origin::move_if_local(value));
            intrusive_ptr_add_ref(new_item);
            return (after ? l->insert_after(pivot, new_item) : l->insert_before(pivot, new_item)) == 0 ? 1 : 0;
        } 
        return 0;
    }

    std::vector<item_ptr> lrange(redis_key& key, int start, int end)
    {
        list* l = fetch_list(key);
        std::vector<item_ptr> result;
        if (l != nullptr) {
            return l->range(start, end);
        } 
        return std::move(result); 
    }

    template<typename origin = local_origin_tag>
    int lset(redis_key& key, int idx, sstring& value)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            const size_t item_size = item::item_size_for_raw_string(value.size());
            auto new_item = local_slab().create(item_size, origin::move_if_local(value));
            intrusive_ptr_add_ref(new_item);
            if (l->set(idx, new_item) == REDIS_OK)
                return 1;
            else {
                intrusive_ptr_release(new_item);
            }
        } 
        return 0;
    }

    int lrem(redis_key& key, int count, sstring& value)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            return l->trem(count, value);
        }
        return 0;
    }

    int ltrim(redis_key& key, int start, int end)
    {
        list* l = fetch_list(key);
        if (l != nullptr) {
            return l->trim(start, end);
        }
        return 0;
    }
protected:
  inline list* fetch_list(redis_key& key)
  {
    auto it = _store->fetch_raw(key);
    if (it != nullptr && it->type() == REDIS_LIST) {
      return static_cast<list*>(it->ptr());
    }
    return nullptr;
  }
  stats _stats;
};

}
