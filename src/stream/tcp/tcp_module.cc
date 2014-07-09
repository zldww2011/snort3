/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

// tcp_module.cc author Russ Combs <rucombs@cisco.com>

#include "tcp_module.h"

#include <string>
using namespace std;

#include "stream_tcp.h"
#include "main/snort_config.h"
#include "stream/stream.h"

//-------------------------------------------------------------------------
// stream_tcp module
//-------------------------------------------------------------------------

#define STREAM_TCP_SYN_ON_EST_STR \
    "(stream_tcp) Syn on established session"
#define STREAM_TCP_DATA_ON_SYN_STR \
    "(stream_tcp) Data on SYN packet"
#define STREAM_TCP_DATA_ON_CLOSED_STR \
    "(stream_tcp) Data sent on stream not accepting data"
#define STREAM_TCP_BAD_TIMESTAMP_STR \
    "(stream_tcp) TCP Timestamp is outside of PAWS window"
#define STREAM_TCP_BAD_SEGMENT_STR \
    "(stream_tcp) Bad segment, adjusted size <= 0"
#define STREAM_TCP_WINDOW_TOO_LARGE_STR \
    "(stream_tcp) Window size (after scaling) larger than policy allows"
#define STREAM_TCP_EXCESSIVE_TCP_OVERLAPS_STR \
    "(stream_tcp) Limit on number of overlapping TCP packets reached"
#define STREAM_TCP_DATA_AFTER_RESET_STR \
    "(stream_tcp) Data sent on stream after TCP Reset sent"
#define STREAM_TCP_SESSION_HIJACKED_CLIENT_STR \
    "(stream_tcp) TCP Client possibly hijacked, different Ethernet Address"
#define STREAM_TCP_SESSION_HIJACKED_SERVER_STR \
    "(stream_tcp) TCP Server possibly hijacked, different Ethernet Address"
#define STREAM_TCP_DATA_WITHOUT_FLAGS_STR \
    "(stream_tcp) TCP Data with no TCP Flags set"
#define STREAM_TCP_SMALL_SEGMENT_STR \
    "(stream_tcp) Consecutive TCP small segments exceeding threshold"
#define STREAM_TCP_4WAY_HANDSHAKE_STR \
    "(stream_tcp) 4-way handshake detected"
#define STREAM_TCP_NO_TIMESTAMP_STR \
    "(stream_tcp) TCP Timestamp is missing"
#define STREAM_TCP_BAD_RST_STR \
    "(stream_tcp) Reset outside window"
#define STREAM_TCP_BAD_FIN_STR \
    "(stream_tcp) FIN number is greater than prior FIN"
#define STREAM_TCP_BAD_ACK_STR \
    "(stream_tcp) ACK number is greater than prior FIN"
#define STREAM_TCP_DATA_AFTER_RST_RCVD_STR \
    "(stream_tcp) Data sent on stream after TCP Reset received"
#define STREAM_TCP_WINDOW_SLAM_STR \
    "(stream_tcp) TCP window closed before receiving data"
#define STREAM_TCP_NO_3WHS_STR \
    "(stream_tcp) TCP session without 3-way handshake"

static const char* policies =
    "first | last | bsd | linux | old-linux | windows | win-2003 | vista "
    "solaris | hpux | hpux10 | irix | macos";

static const char* client_ports =
    "21 23 25 42 53 80 110 111 135 136 137 139 143 445 513 514 1433 1521 "
    "2401 3306";

static const char* client_protos =
    "ftp telnet smtp nameserver dns http pop3 sunrpc dcerpc netbios-ssn imap "
    "login shell mssql oracle cvs mysql";

static const Parameter stream_tcp_small_params[] =
{
    { "count", Parameter::PT_INT, "0:2048", "0",
      "limit number of small segments queued" },

    { "maximum_size", Parameter::PT_INT, "0:2048", "0",
      "limit number of small segments queued" },

    { "ignore_ports", Parameter::PT_BIT_LIST, "65535", "2621",
      "limit number of small segments queued" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter stream_queue_limit_params[] =
{
    { "max_bytes", Parameter::PT_INT, "0:", "1048576",
      "don't queue more than given bytes per session and direction" },

    { "max_segments", Parameter::PT_INT, "0:", "2621",
      "don't queue more than given segments per session and direction" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter stream_tcp_params[] =
{
    { "both_ports", Parameter::PT_BIT_LIST, "65535", nullptr,
      "reassemble data in both directions for given server ports" },

    { "both_protocols", Parameter::PT_STRING, nullptr, nullptr,
      "reassemble data in both directions for given services" },

    { "client_ports", Parameter::PT_BIT_LIST, "65535", client_ports,
      "reassemble data from client to given server ports" },

    { "client_protocols", Parameter::PT_STRING, nullptr, client_protos,
      "reassemble data from client for given services" },

    { "flush_factor", Parameter::PT_INT, "0:", "0",
      "flush upon seeing a drop in segment size after given number of non-decreasing segments" },

    { "ignore_any_rules", Parameter::PT_BOOL, nullptr, "false",
      "process tcp content rules w/o ports only if rules with ports are present" },

    { "max_window", Parameter::PT_INT, "0:1073725440", "0",
      "maximum allowed tcp window" },

    { "overlap_limit", Parameter::PT_INT, "0:255", "0",
      "maximum number of allowed overlapping segments per session" },

    { "paf_max", Parameter::PT_INT, "1460:63780", "16384",
      "maximum reassembled PDU size" },

    { "policy", Parameter::PT_ENUM, policies, "linux",
      "determines operating system characteristics like reassembly" },

    { "reassemble_async", Parameter::PT_BOOL, nullptr, "true",
      "queue data for reassembly before traffic is seen in both directions" },

    { "require_3whs", Parameter::PT_INT, "-1:86400", "-1",
      "don't track midstream sessions after given seconds from start up; -1 tracks all" },

    { "server_ports", Parameter::PT_BIT_LIST, "65535", nullptr,
      "reassemble data from server for given server ports" },

    { "server_protocols", Parameter::PT_STRING, nullptr, nullptr,
      "reassemble data from server for given services" },

    { "show_rebuilt_packets", Parameter::PT_BOOL, nullptr, "false",
      "enable cmg like output of reassembled packets" },

    { "queue_limit", Parameter::PT_TABLE, stream_queue_limit_params, nullptr,
      "limit amount of segment data queued" },

    { "small_segments", Parameter::PT_TABLE, stream_tcp_small_params, nullptr,
      "limit number of small segments queued" },

    { "session_timeout", Parameter::PT_INT, "1:86400", "30",
      "session tracking timeout" },

    { "footprint", Parameter::PT_INT, "0:", "false",
      "use zero for production, non-zero for testing at given size" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const RuleMap stream_tcp_rules[] =
{
    { STREAM_TCP_SYN_ON_EST, STREAM_TCP_SYN_ON_EST_STR },
    { STREAM_TCP_DATA_ON_SYN, STREAM_TCP_DATA_ON_SYN_STR },
    { STREAM_TCP_DATA_ON_CLOSED, STREAM_TCP_DATA_ON_CLOSED_STR },
    { STREAM_TCP_BAD_TIMESTAMP, STREAM_TCP_BAD_TIMESTAMP_STR },
    { STREAM_TCP_BAD_SEGMENT, STREAM_TCP_BAD_SEGMENT_STR },
    { STREAM_TCP_WINDOW_TOO_LARGE, STREAM_TCP_WINDOW_TOO_LARGE_STR },
    { STREAM_TCP_EXCESSIVE_TCP_OVERLAPS, STREAM_TCP_EXCESSIVE_TCP_OVERLAPS_STR },
    { STREAM_TCP_DATA_AFTER_RESET, STREAM_TCP_DATA_AFTER_RESET_STR },
    { STREAM_TCP_SESSION_HIJACKED_CLIENT, STREAM_TCP_SESSION_HIJACKED_CLIENT_STR },
    { STREAM_TCP_SESSION_HIJACKED_SERVER, STREAM_TCP_SESSION_HIJACKED_SERVER_STR },
    { STREAM_TCP_DATA_WITHOUT_FLAGS, STREAM_TCP_DATA_WITHOUT_FLAGS_STR },
    { STREAM_TCP_SMALL_SEGMENT, STREAM_TCP_SMALL_SEGMENT_STR },
    { STREAM_TCP_4WAY_HANDSHAKE, STREAM_TCP_4WAY_HANDSHAKE_STR },
    { STREAM_TCP_NO_TIMESTAMP, STREAM_TCP_NO_TIMESTAMP_STR },
    { STREAM_TCP_BAD_RST, STREAM_TCP_BAD_RST_STR },
    { STREAM_TCP_BAD_FIN, STREAM_TCP_BAD_FIN_STR },
    { STREAM_TCP_BAD_ACK, STREAM_TCP_BAD_ACK_STR },
    { STREAM_TCP_DATA_AFTER_RST_RCVD, STREAM_TCP_DATA_AFTER_RST_RCVD_STR },
    { STREAM_TCP_WINDOW_SLAM, STREAM_TCP_WINDOW_SLAM_STR },
    { STREAM_TCP_NO_3WHS, STREAM_TCP_NO_3WHS_STR },

    { 0, nullptr }
};

StreamTcpModule::StreamTcpModule() :
    Module(MOD_NAME, stream_tcp_params, stream_tcp_rules)
{
    config = nullptr;
}

StreamTcpModule::~StreamTcpModule()
{
    for ( auto p : protos )
        delete p;
}

StreamTcpConfig* StreamTcpModule::get_data()
{
    StreamTcpConfig* temp = config;
    config = nullptr;
    return temp;
}

ServiceReassembly::ServiceReassembly(string& s, bool cs, bool sc)
{
    name = s;
    c2s = cs;
    s2c = sc;
}

void StreamTcpModule::add_protos(Value& v, bool c2s, bool s2c)
{
    string tok;
    v.set_first_token();

    while ( v.get_next_token(tok) )
        protos.push_back(new ServiceReassembly(tok, c2s, s2c));
}

const ServiceReassembly* StreamTcpModule::get_proto(unsigned idx)
{
    if ( idx < protos.size() )
        return protos[idx];
    else
        return nullptr;
}

void StreamTcpModule::get_port(
    Port p, bool& c2s, bool& s2c)
{
    c2s = ports_client[p] || ports_both[p];
    s2c = ports_server[p] || ports_both[p];
}

bool StreamTcpModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("count") )
        config->max_consec_small_segs = v.get_long();

    else if ( v.is("maximum_size") )
        config->max_consec_small_seg_size = v.get_long();

    else if ( v.is("ignore_ports") )
        v.get_bits(config->small_seg_ignore);

    else if ( v.is("flush_factor") )
        config->flush_factor = v.get_long();

    else if ( v.is("footprint") )
        config->footprint = v.get_long();

    else if ( v.is("ignore_any_rules") )
        config->flags |= STREAM5_CONFIG_IGNORE_ANY;

    else if ( v.is("max_bytes") )
        config->max_queued_bytes = v.get_long();

    else if ( v.is("max_segments") )
        config->max_queued_segs = v.get_long();

    else if ( v.is("max_window") )
        config->max_window = v.get_long();

    else if ( v.is("paf_max") )
        config->paf_max = v.get_long();

    else if ( v.is("policy") )
        config->policy = v.get_long() + 1;

    else if ( v.is("overlap_limit") )
        config->overlap_limit = v.get_long();

    else if ( v.is("session_timeout") )
        config->session_timeout = v.get_long();

    else if ( v.is("client_ports") )
        v.get_bits(ports_client);

    else if ( v.is("server_ports") )
        v.get_bits(ports_server);

    else if ( v.is("both_ports") )
        v.get_bits(ports_both);

    else if ( v.is("client_protocols") )
    {
        add_protos(v, true, false);
        client_protos_set = true;
    }
    else if ( v.is("server_protocols") )
        add_protos(v, false, true);

    else if ( v.is("both_protocols") )
        add_protos(v, true, true);

    else if ( v.is("reassemble_async") )
    {
        if ( v.get_bool() )
            config->flags &= ~STREAM5_CONFIG_NO_ASYNC_REASSEMBLY;
        else
            config->flags |= STREAM5_CONFIG_NO_ASYNC_REASSEMBLY;
    }
    else if ( v.is("require_3whs") )
    {
        config->hs_timeout = v.get_long();
    }
    else if ( v.is("show_rebuilt_packets") )
    {
        if ( v.get_bool() )
            config->flags |= STREAM5_CONFIG_SHOW_PACKETS;
        else
            config->flags &= ~STREAM5_CONFIG_SHOW_PACKETS;
    }

    else
        return false;

    return true;
}

bool StreamTcpModule::begin(const char*, int, SnortConfig*)
{
    if ( config )
        return false;

    config = new StreamTcpConfig;

    ports_client.reset();
    ports_server.reset();
    ports_both.reset();

    for ( auto p : protos )
        delete p;

    protos.clear();
    client_protos_set = false;

    // set default ports here
    // if set by user, these are cleared
    //Value v(client_ports);     // FIXIT need to convert to bit string
    //v.get_bits(ports_client);  // before converting to bitset

    return true;
}

bool StreamTcpModule::end(const char*, int, SnortConfig*)
{
    // these cases are all handled properly
    // (stream_tcp handles protos == 'all'):
    //
    // ports client none -> client_ports = ''
    // ports client all -> client_ports = '1:65535'
    // protocol server none -> server_protocols = ''
    // protocol server all -> server_protocols = 'all'

    // set default protos here if not set by user
    if ( !client_protos_set )
    {
        Value v(client_protos);
        add_protos(v, true, false);
    }
    return true;
}

