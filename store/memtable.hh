/** 
 *  Copy from memtable.{cc, hh} from https://github.com/scylladb/scylla
 * 
 *  Modified by Peng Jian.
 *
 **/
#pragma once
#include <map>
#include <memory>
#include "utils/logalloc.hh"
#include "utils/managed_bytes.hh"
#include "partition.hh"
#include "keys.hh"
#include "seastarx.hh"
#include <experimental/optional>

template<class T>
using optional = std::experimental::optional<T>;

namespace bi = boost::intrusive;
class partition;
namespace store {
class memtable_entry {
    bi::set_member_hook<> _link;
    redis::decorated_key _key;
    partition _partition;
public:
    friend class memtable;

    memtable_entry(redis::decorated_key key, partition&& data)
        : _key(std::move(key))
        , _partition(std::move(data))
    {
    }

    memtable_entry(memtable_entry&& o) noexcept;

    const redis::decorated_key& key() const { return _key; }
    redis::decorated_key& key() { return _key; }

    struct compare {
        bool operator()(const redis::decorated_key& l, const memtable_entry& r) const {
            return l == r.key();
        }

        bool operator()(const memtable_entry& l, const memtable_entry& r) const {

            return l.key() == r.key();
        }

        bool operator()(const memtable_entry& l, const redis::decorated_key& r) const {
            return l.key() == r;
        }
    };
};

class dirty_memory_manager;


class memtable final : public enable_lw_shared_from_this<memtable>, private logalloc::region {
public:
    using partitions_type = bi::set<memtable_entry,
        bi::member_hook<memtable_entry, bi::set_member_hook<>, &memtable_entry::_link>,
        bi::compare<memtable_entry::compare>>;
private:
    dirty_memory_manager& _dirty_mgr;
    logalloc::allocating_section _read_section;
    logalloc::allocating_section _allocating_section;
    partitions_type _partitions;
    uint64_t _flushed_memory = 0;
    bool _write_enabled = true;
private:
    partition& find_or_create_partition(const redis::decorated_key& key);
    void upgrade_entry(memtable_entry&);
    void add_flushed_memory(uint64_t);
    void remove_flushed_memory(uint64_t);
    void clear() noexcept;
    uint64_t dirty_size() const;
public:
    explicit memtable(dirty_memory_manager&);
    explicit memtable();
    ~memtable();
    future<> clear_gently() noexcept;

    static memtable& from_region(logalloc::region& r) {
        return static_cast<memtable&>(r);
    }

    const logalloc::region& region() const {
        return *this;
    }

    logalloc::region_group* region_group() {
        return group();
    }
    bool insert(redis::decorated_key&& key, partition&& data) { return false; /* mock now; */ }
    optional<partition> get(redis::decorated_key key);
    bool remote(redis::decorated_key key);
    void disable_write() { _write_enabled = false; }
    bool write_enabled() const { return _write_enabled; }
public:
    size_t partition_count() const;
    logalloc::occupancy_stats occupancy() const;

    bool empty() const { return _partitions.empty(); }
    bool is_flushed() const;
    void on_detach_from_region_group() noexcept;
    void revert_flushed_memory() noexcept;
    friend class memtable_reader;
};
}
