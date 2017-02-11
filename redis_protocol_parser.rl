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
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

/** This protocol parser was inspired by the memcached app,
  which is the example in Seastar project.
**/

#include "core/ragel.hh"
#include <memory>
#include <iostream>
#include <algorithm>
#include <functional>

%%{

machine redis_resp_protocol;

access _fsm_;

action mark {
    g.mark_start(p);
}

action start_blob {
    g.mark_start(p);
    _size_left = _arg_size;
}
action start_command {
    g.mark_start(p);
    _size_left = _arg_size;
}

action advance_blob {
    auto len = std::min((uint32_t)(pe - p), _size_left);
    _size_left -= len;
    p += len;
    if (_size_left == 0) {
      _args_list.push_back(str());
      p--;
      fret;
    }
    p--;
}

action advance_command {
    auto len = std::min((uint32_t)(pe - p), _size_left);
    _size_left -= len;
    p += len;
    if (_size_left == 0) {
      _command = str();
      p--;
      fret;
    }
    p--;
}


crlf = '\r\n';
u32 = digit+ >{ _u32 = 0;}  ${ _u32 *= 10; _u32 += fc - '0';};
args_count = '*' u32 crlf ${_args_count = _u32;};
blob := any+ >start_blob $advance_blob;
set = "set" ${_command = command::set;};
mset = "mset" ${_command = command::mset;};
get = "get" ${_command = command::get;};
mget = "mget" ${_command = command::mget;};
del = "del" ${_command = command::del;};
echo = "eho" ${_command = command::echo;};
ping = "ping" ${_command = command::ping;};
incr = "incr" ${_command = command::incr;};
decr = "decr" ${_command = command::decr;};
incrby = "incrby" ${_command = command::incrby;};
decrby = "decrby" ${_command = command::decrby;};
command_ = "command" ${_command = command::command;};
exists = "exists" ${_command = command::exists;};
append = "append" ${_command = command::append;};
strlen = "strlen" ${_command = command::strlen;};
lpush = "lpush" ${_command = command::lpush;};
lpushx = "lpushx" ${_command = command::lpushx;};
lpop = "lpop" ${_command = command::lpop;};
llen = "llen" ${_command = command::llen;};
lindex = "lindex" ${_command = command::lindex;};
linsert = "linsert" ${_command = command::linsert;};
lrange = "lrange" ${_command = command::lrange;};
lset = "lset" ${_command = command::lset;};
rpush = "rpush" ${_command = command::rpush;};
rpushx = "rpushx" ${_command = command::rpushx;};
rpop = "rpop" ${_command = command::rpop;};
lrem = "lrem" ${_command = command::lrem;};
ltrim = "ltrim" ${_command = command::ltrim;};
hset = "hset" ${_command = command::hset;};
hdel = "hdel" ${_command = command::hdel;};
hget = "hget" ${_command = command::hget;};
hlen = "hlen" ${_command = command::hlen;};
hexists = "hexists" ${_command = command::hexists;};
hstrlen = "hstrlen" ${_command = command::hstrlen;};
hincrby = "hincrby" ${_command = command::hincrby;};
hincrbyfloat = "hincrbyfloat" ${_command = command::hincrbyfloat;};
hkeys = "hkeys" ${_command = command::hkeys;};
hvals = "hvals" ${_command = command::hvals;};
hmget = "hmget" ${_command = command::hmget;};
hgetall = "hgetall" ${_command = command::hgetall;};
sadd = "sadd" ${_command = command::sadd;};
scard = "scard" ${_command = command::scard;};
sismember = "sismember" ${_command = command::sismember;};
smembers = "smembers" ${_command = command::smembers;};
srem = "srem" ${_command = command::srem;};
sdiff = "sdiff" ${_command = command::sdiff;};
sdiffstore = "sdiffstore" ${_command = command::sdiffstore;};
sinter = "sinter" ${_command = command::sinter;};
sinterstore = "sinterstore" ${_command = command::sinterstore;};
sunion = "sunion" ${_command = command::sunion;};
sunionstore = "sunionstore" ${_command = command::sunionstore;};
smove = "smove" ${_command = command::smove;};
command = (set | get | del | mget | mset | echo | ping | incr | decr | incrby | decrby | command_ | exists | append | strlen | lpush | lpushx | lpop | llen | lindex | linsert | lrange | lset | rpush | rpushx | rpop | lrem | ltrim | hset | hget | hdel | hlen | hexists | hstrlen | hincrby | hincrbyfloat | hkeys | hvals | hmget | hgetall   | sadd | scard | sismember | smembers | srem | sdiff | sdiffstore | sinter | sinterstore | sunion | sunionstore | smove);
arg = '$' u32 crlf ${ _arg_size = _u32;};

main := (args_count (arg (command) crlf) (arg @{fcall blob; } crlf)+) ${_state = state::ok;};

prepush {
    prepush();
}

postpop {
    postpop();
}

}%%

class redis_protocol_parser : public ragel_parser_base<redis_protocol_parser> {
    %% write data nofinal noprefix;
public:
    enum class state {
        error,
        eof,
        ok,
    };
    enum class command {
        set,
        mset,
        get,
        mget,
        del,
        echo,
        ping,
        incr,
        decr,
        incrby,
        decrby,
        command,
        exists,
        append,
        strlen,
        lpush,
        lpushx,
        lpop,
        llen,
        lindex,
        linsert,
        lrange,
        lset,
        rpush,
        rpushx,
        rpop,
        lrem,
        ltrim,
        hset,
        hdel,
        hget,
        hlen,
        hexists,
        hstrlen,
        hincrby,
        hincrbyfloat,
        hkeys,
        hvals,
        hmget,
        hgetall,
        sadd,
        scard,
        sismember,
        smembers,
        srem,
        sdiff,
        sdiffstore,
        sinter,
        sinterstore,
        sunion,
        sunionstore,
        smove,
    };

    state _state;
    command _command;
    uint32_t _u32;
    uint32_t _arg_size;
    uint32_t _args_count;
    uint32_t _size_left;
    std::vector<sstring>  _args_list;
public:
    void init() {
        init_base();
        _state = state::error;
        _args_list.clear();
        _args_count = 0;
        _size_left = 0;
        _arg_size = 0;
        %% write init;
    }

    char* parse(char* p, char* pe, char* eof) {
        sstring_builder::guard g(_builder, p, pe);
        auto str = [this, &g, &p] { g.mark_end(p); return get_str(); };
        %% write exec;
        if (_state != state::error) {
            return p;
        }
        // error ?
        if (p != pe) {
            p = pe;
            return p;
        }
        return nullptr;
    }
    bool eof() const {
        return _state == state::eof;
    }
};
