/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
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
#include "sorted_set.hh"
#include "iterator.hh"
#include "dict.hh"
namespace redis 
{
struct skiplist_node
{
    lw_shared_ptr<item> _value;
    double _score;
    struct next_levels {
        skiplist_node* _next;
        int _span;
    };
    skiplist_node* _prev;
    next_levels _next[];
    skiplist_node(int level, double score, lw_shared_ptr<item> value) : _value(value), _score(score), _prev(nullptr) {
        for (int i = 0; i < level; ++i) {
           _next[i]._next = nullptr;
           _next[i]._span = 0;
        }
    }
    ~skiplist_node()
    {
    }
};

struct range
{
    range(double min, bool minx, double max, bool maxe) : _min(min), _max(max), _min_exclusive(minx), _max_exclusive(maxe) {}
    range(double min, double max) : range(min, true, max, true) {}
    double _min;
    double _max;
    bool   _min_exclusive;
    bool   _max_exclusive;
    bool hit_min(double min) const { return _min_exclusive ? (min > _min) : (min >= _min); }
    bool hit_max(double max) const { return _max_exclusive ? (max > _max) : (max >= _max); }
    bool empty() const { return _min > _max || (_min == _max && (_min_exclusive || _max_exclusive)); }
};

class skiplist_iterator;
class skiplist
{
    friend class skiplist_iterator;
private:
    skiplist_node* _head;
    skiplist_node* _tail;
    int _level;
    size_t _size;
    static const int MAX_LEVEL = 32;
public:
    skiplist();
    ~skiplist();
    int random_level();
    skiplist_node* create_skiplist_node(int level, double score, lw_shared_ptr<item> value);
    skiplist_node* insert(double score, lw_shared_ptr<item> value);
    void remove_node(skiplist_node* x, skiplist_node** update);
    int remove_item(lw_shared_ptr<item> value, skiplist_node** node);
    bool include_range(const range& r);
    skiplist_node* find_first_of_range(const range& r);
    skiplist_node* find_last_of_range(const range& r);
    skiplist_node* find_by_rank(size_t r);
    size_t get_rank_of_item(lw_shared_ptr<item> value);
    size_t get_rank_of_node(skiplist_node* n);
    int item_compare(lw_shared_ptr<item> l, lw_shared_ptr<item> r);
    size_t size() { return _size; }
};

enum
{
    FROM_HEAD_TO_TAIL = 0,
    FROM_TAIL_TO_HEAD = 1
};

class skiplist_iterator : public iterator<skiplist_node>
{
private:
    int _direction;
    skiplist_node* _next;
    skiplist* _rep;
public:
    skiplist_iterator(skiplist* rep, int dir) : _direction(dir), _next(nullptr), _rep(rep) {
        if (dir == FROM_HEAD_TO_TAIL)
            _next = rep->_head->_next[0]._next;
        else
            _next = rep->_tail;
    }
    ~skiplist_iterator() { }

    bool valid() const { return _next != nullptr; }
    void seek_to_first() {
        if (_direction == FROM_HEAD_TO_TAIL) {
            _next = _rep->_head->_next[0]._next;
        } else {
            _next = _rep->_tail;
        }
    }
    void seek_to_last() {
        if (_direction == FROM_HEAD_TO_TAIL) {
            _next = _rep->_tail;
        } else {
            _next = _rep->_head->_next[0]._next;
        }
    }
    void seek(const sstring& key) {
    }
    void seek(size_t rank) {
        _next = _rep->find_by_rank(rank);
    }
    void next() {
        if (_next != nullptr)
            _next = _direction == FROM_HEAD_TO_TAIL ? _next->_next[0]._next : _next->_prev;
    }
    void prev() {
        if (_next != nullptr)
            _next = _direction == FROM_HEAD_TO_TAIL ? _next->_prev : _next->_next[0]._next;
    }
    skiplist_node* value() const {
        if (_next != nullptr) {
            return _next;
        }
        return nullptr;
    }
    int status() const {
        return _next != nullptr ? REDIS_OK : REDIS_ERR;
    }
};

skiplist::skiplist()
    : _head(create_skiplist_node(MAX_LEVEL, 0, nullptr))
    , _tail(nullptr)
    , _level(1)
    , _size(0)
{
}

skiplist::~skiplist()
{
    auto x = _head;
    while (x != nullptr) {
        auto next = x->_next[0]._next;
        delete x;
        x = next;
    }
}

int skiplist::item_compare(lw_shared_ptr<item> l, lw_shared_ptr<item> r)
{
    return 0;
}

int skiplist::random_level()
{
    static const int kBranching = 4;
    int level = 1;
    while (level < MAX_LEVEL && (random() % kBranching == 0)) {
        level++;
    }
    return level;
}

skiplist_node* skiplist::create_skiplist_node(int level, double score, lw_shared_ptr<item> value)
{
   using next_levels = skiplist_node::next_levels;
   char* m = static_cast<char*>(malloc(sizeof(skiplist_node) + level * sizeof(next_levels)));
   auto node = new (m) skiplist_node(level, score, value);
   return node;
}

skiplist_node* skiplist::insert(double score, lw_shared_ptr<item> value)
{
    skiplist_node* update[MAX_LEVEL];
    int rank[MAX_LEVEL];
    auto x = _head;
    for (int level = _level - 1; level >= 0; level--) {
        rank[level] = level == (_level-1) ? 0 : rank[level + 1];
        while (x->_next[level]._next && (x->_next[level]._next->_score < score || (x->_next[level]._next->_score == score && item_compare(x->_next[level]._next->_value, value) < 0))) {
            rank[level] += x->_next[level]._span;
            x = x->_next[level]._next;
        }
        update[level] = x;
    }
    auto level = random_level();
    if (level > _level) {
        for (auto l = _level; l < level; ++l) {
            rank[l] = 0;
            update[l] = _head;
            update[l]->_next[l]._span = _size;
        }
        _level = level;
    }
    x = create_skiplist_node(level, score, value);
    for (int level = 0; level < _level; ++level) {
        x->_next[level]._next = update[level]->_next[level]._next;
        update[level]->_next[level]._next = x;
        x->_next[level]._span = update[level]->_next[level]._span - (rank[0] - rank[level]);
        update[level]->_next[level]._span = (rank[0] - rank[level]) + 1;
    }
    for (int l  = level; l < _level; l++) {
        update[l]->_next[l]._span++;
    }
    x->_prev = (update[0] == _head) ? nullptr : update[0];
    if (x->_next[0]._next)
        x->_next[0]._next->_prev = x;
    else
        _tail = x;
    _size++;
    return x;
}

void skiplist::remove_node(skiplist_node* x, skiplist_node** update)
{
    for (int l = 0; l < _level; l++) {
        if (update[l]->_next[l]._next == x) {
            update[l]->_next[l]._span += x->_next[l]._span - 1;
            update[l]->_next[l]._next = x->_next[l]._next;
        } else {
            update[l]->_next[l]._span -= 1;
        }
    }
    if (x->_next[0]._next) {
        x->_next[0]._next->_prev = x->_prev;
    } else {
        _tail = x->_prev;
    }
    while(_level > 1 && _head->_next[_level-1]._next == nullptr)
        _level--;
    _size--;
}

int skiplist::remove_item(lw_shared_ptr<item> value, skiplist_node** node)
{
    skiplist_node* update[MAX_LEVEL];
    auto score = value->Double();
    auto x = _head;
    for (int level = _level - 1; level >= 0; level--) {
        while (x->_next[level]._next && (x->_next[level]._next->_score < score || (x->_next[level]._next->_score == score && item_compare(x->_next[level]._next->_value, value) < 0))) {
            x = x->_next[level]._next;
        }
        update[level] = x;
    }
    x = x->_next[0]._next;
    if (x && score == x->_score && item_compare(x->_value, value) == 0) {
        remove_node(x, update);
        if (!node)
           delete node;
        else
            *node = x;
        return 1;
    }
    return 0;
}

bool skiplist::include_range(const range& r)
{
    if (r.empty()) {
        return false;
    }
    auto x = _tail;
    if (x == nullptr || r.hit_min(x->_score) == false) {
        return 0;
    }
    x = _head->_next[0]._next;
    if (x == nullptr || r.hit_max(x->_score) == false) {
        return 0;
    }
    return 1;
}

skiplist_node* skiplist::find_first_of_range(const range& r)
{
    if (include_range(r) == false) {
        return nullptr;
    }
    auto x = _head;
    for (auto i = _level-1; i >= 0; i--) {
        while (x->_next[i]._next && r.hit_min(x->_next[i]._next->_score) == false) {
            x = x->_next[i]._next;
        }
    }

    x = x->_next[0]._next;
    if (r.hit_max(x->_score) == false) {
        return nullptr;
    }
    return x;
}

skiplist_node* skiplist::find_last_of_range(const range& r)
{
    if (include_range(r) == false) {
        return nullptr;
    }
    auto x = _head;
    for (auto i = _level-1; i >= 0; i--) {
        while (x->_next[i]._next && r.hit_max(x->_next[i]._next->_score) == false) {
            x = x->_next[i]._next;
        }
    }

    x = x->_next[0]._next;
    if (r.hit_min(x->_score) == false) {
        return nullptr;
    }
    return x;
}

skiplist_node* skiplist::find_by_rank(size_t rank)
{
    size_t traversed = 0;
    auto x = _head;
    for (auto i = _level-1; i >= 0; i--) {
        while (x->_next[i]._next && (traversed + x->_next[i]._span) <= rank)
        {
            traversed += x->_next[i]._span;
            x = x->_next[i]._next;
        }
        if (traversed == rank) {
            return x;
        }
    }
    return nullptr;
}

size_t skiplist::get_rank_of_item(lw_shared_ptr<item> value) 
{
    size_t rank = 0;
    auto x = _head;
    auto score = value->Double();
    for (auto i = _level-1; i >= 0; i--) {
        while (x->_next[i]._next &&
              (x->_next[i]._next->_score < score ||
              (x->_next[i]._next->_score == score &&
              item_compare(x->_next[i]._next->_value, value) <= 0))) {
            rank += x->_next[i]._span;
            x = x->_next[i]._next;
        }

        if (x->_value && item_compare(x->_value, value) == 0) {
            return rank;
        }
    }
    return -1;
}

size_t skiplist::get_rank_of_node(skiplist_node* n) 
{
    size_t rank = 0;
    auto x = _head;
    auto score = n->_value->Double();
    for (auto i = _level-1; i >= 0; i--) {
        while (x->_next[i]._next &&
                (x->_next[i]._next->_score < score ||
                 (x->_next[i]._next->_score == score &&
                  item_compare(x->_next[i]._next->_value, n->_value) <= 0))) {
            rank += x->_next[i]._span;
            x = x->_next[i]._next;
        }

        if (x->_value && item_compare(x->_value, n->_value) == 0) {
            return rank;
        }
    }
    return 0;
}

struct sorted_set::rep {
    rep();
    ~rep();
    dict* _dict;
    skiplist* _list;
    int insert(const redis_key& key, lw_shared_ptr<item> m);
    inline size_t size() { return _dict->size(); } 
    size_t count_in_range(double min, double max);
    size_t rank(const redis_key& key, bool reverse);
    int remove(const redis_key& key);
    double score(const redis_key& key);
    double incr(const redis_key& key, double delta);
    std::vector<item_ptr> range_by_score(double min, double max, bool reverse);
    std::vector<item_ptr> range_by_rank(size_t begin, size_t end, bool reverse);
    lw_shared_ptr<item> fetch(const redis_key& key);
    bool update(lw_shared_ptr<item> value, double score);
};

sorted_set::rep::rep()
    : _dict(new dict())
    , _list(new skiplist())
{
}

sorted_set::rep::~rep()
{
    delete _list;
    delete _dict;
}

lw_shared_ptr<item> sorted_set::rep::fetch(const redis_key& key)
{
    return _dict->fetch_raw(key);
}

bool sorted_set::rep::update(lw_shared_ptr<item> value, double score)
{
    if (value) {
       _list->remove_item(value, nullptr);
       value->set_double(score);
       _list->insert(score, value);
       return true;
    }
    return false;
}

double sorted_set::rep::incr(const redis_key& key, double delta)
{
    auto n = _dict->fetch_raw(key);
    if (n) {
        return n->incr(delta);
    }
    return 0;
}

double sorted_set::rep::score(const redis_key& key)
{
    auto n = _dict->fetch_raw(key);
    if (n) {
        return n->Double();
    }
    return 0;
}

int sorted_set::rep::remove(const redis_key& key)
{
    auto n = _dict->fetch_raw(key);
    if (n) {
        return _list->remove_item(n, nullptr);
    }
    return 0;
}

size_t sorted_set::rep::rank(const redis_key& key, bool reverse)
{
    auto n = _dict->fetch_raw(key);
    if (n) {
        auto rank = _list->get_rank_of_item(n);
        if (!reverse) {
            return _list->size() - rank; 
        }
        return rank;
    }
    return -1;
}

int sorted_set::rep::insert(const redis_key& key, lw_shared_ptr<item> m)
{
    auto node = _list->insert(m->Double(), m);
    if (node != nullptr) {
        return _dict->set(key, m);
    }
    return REDIS_ERR;
}

size_t sorted_set::rep::count_in_range(double min, double max)
{
    struct range r(min, max);
    if (r.empty()) return 0;
    if (_list->include_range(r) == false) {
        return 0;
    }
    size_t count = 0;
    auto n = _list->find_first_of_range(r);
    if (n != nullptr) {
        auto rank = _list->get_rank_of_node(n);
        count = (_list->size() - (rank - 1));
        n = _list->find_last_of_range(r);
        if (n != nullptr) {
            rank = _list->get_rank_of_node(n);
            count -= (_list->size() - rank);
        }
    }
    return count;
}

std::vector<item_ptr> sorted_set::rep::range_by_score(double min, double max, bool reverse)
{
    using return_type = std::vector<item_ptr>;
    struct range r(min, max);
    if (r.empty()) return return_type {};
    if (_list->include_range(r) == false) {
        return return_type {};
    }
    return_type result;
    auto n = !reverse ? _list->find_first_of_range(r) : _list->find_last_of_range(r);
    if (n) {
        while (n) {
            if (!r.hit_min(n->_score)) break;
            if (!r.hit_max(n->_score)) break;
            result.push_back(n->_value);
            if (!reverse) {
                n = n->_next[0]._next;
            }
            else {
                n = n->_prev;
            }
        }
    }
    return std::move(result);
}

std::vector<item_ptr> sorted_set::rep::range_by_rank(size_t begin, size_t end, bool reverse)
{
    if (_list->size() == 0) {
        return std::vector<item_ptr>();
    }
    if (begin < 0) { begin += _list->size(); }
    if (end < 0) { end += _list->size(); }
    if (begin < 0) begin = 0;
    if (begin > end) {
        return std::vector<item_ptr>();
    }
    skiplist_iterator iter(_list, !reverse ? FROM_HEAD_TO_TAIL : FROM_TAIL_TO_HEAD);
    iter.seek(begin);
    std::vector<item_ptr> result;
    size_t count = end - begin + 1;
    while (iter.status() == REDIS_OK && count-- > 0) {
        auto n = iter.value();
        if (n && n->_value) {
            result.emplace_back(item_ptr(n->_value));
        }
        iter.next();
    }
    return std::move(result);
}

sorted_set::sorted_set()
    : _rep(new sorted_set::rep())
{
}
sorted_set::~sorted_set()
{
    delete _rep;
}
int sorted_set::insert(const redis_key& key, lw_shared_ptr<item> m)
{
    return _rep->insert(key, m);
}

size_t sorted_set::size()
{
    return _rep->size();
}

std::vector<item_ptr> sorted_set::range_by_score(double min, double max, bool reverse)
{
    return _rep->range_by_score(min, max, reverse);
}

std::vector<item_ptr> sorted_set::range_by_rank(size_t begin, size_t end, bool reverse)
{
    return _rep->range_by_rank(begin, end, reverse);
}

double sorted_set::incrby(const redis_key& key, double delta)
{
    return _rep->incr(key, delta);
}

size_t sorted_set::count(double min, double max)
{
    return _rep->count_in_range(min, max);
}

bool sorted_set::update(lw_shared_ptr<item> value, double score)
{
    return _rep->update(value, score);
}

lw_shared_ptr<item> sorted_set::fetch(const redis_key& key)
{
    return _rep->fetch(key);
}

int sorted_set::remove(const redis_key& key)
{
    return _rep->remove(key);
}

size_t sorted_set::rank(const redis_key& key, bool reverse)
{
    return _rep->rank(key, reverse);
}
}

