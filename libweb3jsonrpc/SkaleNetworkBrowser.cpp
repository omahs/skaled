#include "SkaleNetworkBrowser.h"

#include <json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <sstream>

#include <stdlib.h>

#include <skutils/console_colors.h>
#include <skutils/dispatch.h>
#include <skutils/eth_utils.h>
#include <skutils/multithreading.h>
#include <skutils/rest_call.h>
#include <skutils/task_performance.h>
#include <skutils/utils.h>

#include <libdevcore/Common.h>
#include <libdevcore/CommonJS.h>
#include <libdevcore/Log.h>
#include <libdevcore/SHA3.h>

namespace skale {
namespace network {
namespace browser {

// see: https://docs.soliditylang.org/en/develop/abi-spec.html#abi
// see: https://docs.soliditylang.org/en/develop/internals/layout_in_memory.html

struct item256_t {
    std::string strRaw;  // without 0x prefix
    dev::u256 u256;
    size_t n;
};  // struct item256_t

typedef std::vector< item256_t > vec256_t;

static vec256_t stat_split_raw_answer( const std::string& strIn ) {
    std::string s = skutils::tools::to_lower( skutils::tools::trim_copy( strIn ) );
    size_t n = s.length();
    if ( n > 2 && ( s[0] == '0' && s[1] == 'x' ) )
        s = s.substr( 2, n - 2 );
    n = s.length();
    size_t cnt = n / 64;
    vec256_t vec;
    for ( size_t i = 0; i < cnt; ++i ) {
        item256_t item;
        item.strRaw = s.substr( i * 64, 64 );
        item.u256 = dev::u256( "0x" + item.strRaw );
        std::stringstream ss;
        ss << std::hex << item.strRaw;
        ss >> item.n;
        vec.push_back( item );
    }
    return vec;
}

static std::string stat_extract_string( const vec256_t& vec, size_t i ) {
    size_t offset = vec[i].n;
    i = offset / 32;
    size_t len = vec[i].n;
    ++i;
    size_t accumulated = 0;
    std::string s;
    for ( ; accumulated < len; ) {
        size_t cntPart = len - accumulated;
        if ( cntPart > 32 )
            cntPart = 32;
        for ( size_t j = 0; j < cntPart; ++j ) {
            std::string str1 = vec[i].strRaw.substr( j * 2, 2 );
            char* pEnd = nullptr;
            char c = ( char ) ::strtol( str1.c_str(), &pEnd, 16 );
            s += c;
        }
        accumulated += cntPart;
        ++i;
    }
    return s;
}

static std::string stat_extract_ipv4( const vec256_t& vec, size_t i ) {
    std::string ip;
    for ( size_t j = 0; j < 4; ++j ) {
        std::string s = vec[i].strRaw.substr( j * 2, 2 );
        char* pEnd = nullptr;
        unsigned int n = ( unsigned int ) ::strtol( s.c_str(), &pEnd, 16 );
        if ( ip.length() > 0 )
            ip += ".";
        ip += skutils::tools::format( "%d", int( uint8_t( n ) ) );
    }
    return ip;
}

vec256_t stat_extract_vector( const vec256_t& vec, size_t i ) {
    size_t offset = vec[i].n;
    i = offset / 32;
    size_t len = vec[i].n;
    ++i;
    vec256_t vecOut;
    for ( size_t j = 0; j < len; ++j, ++i ) {
        vecOut.push_back( vec[i] );
    }
    return vecOut;
}

static dev::u256 stat_compute_chain_id_from_schain_name( const std::string& name ) {
    dev::h256 schain_id = dev::sha3( name );
    std::string s = skutils::tools::to_lower( skutils::tools::trim_copy( schain_id.hex() ) );
    size_t n = s.length();
    if ( n > 2 && ( s[0] == '0' && s[1] == 'x' ) )
        s = s.substr( 2, n - 2 );
    while ( s.length() < 64 )
        s = "0" + s;
    s = s.substr( 0, 14 );
    dev::h256 chainId( "0x" + s );
    return chainId;
}

static nlohmann::json stat_create_basic_call() {
    nlohmann::json joCall = nlohmann::json::object();
    joCall["jsonrpc"] = "2.0";
    joCall["method"] = "eth_call";
    joCall["params"] = nlohmann::json::array();
    return joCall;
}

static std::string stat_to_appendable_string( std::string s ) {
    s = skutils::tools::to_lower( skutils::tools::trim_copy( s ) );
    size_t n = s.length();
    if ( n > 2 && ( s[0] == '0' && s[1] == 'x' ) )
        s = s.substr( 2, n - 2 );
    while ( s.length() < 64 )
        s = "0" + s;
    return s;
}

static std::string stat_to_appendable_string( const dev::u256& val ) {
    std::string s = skutils::tools::to_lower( skutils::tools::trim_copy( dev::toJS( val ) ) );
    return stat_to_appendable_string( s );
}

static std::string stat_to_appendable_string( const dev::h256& val ) {
    std::string s = skutils::tools::to_lower( skutils::tools::trim_copy( val.hex() ) );
    return stat_to_appendable_string( s );
}

// static std::string stat_to_appendable_string( size_t val ) {
//    return stat_to_appendable_string( dev::u256( val ) );
//}

static std::string stat_to_0x_string( const dev::u256& val ) {
    return "0x" + stat_to_appendable_string( val );
}
static std::string stat_to_0x_string( const dev::h256& val ) {
    return "0x" + stat_to_appendable_string( val );
}
// static std::string stat_to_0x_string( size_t val ) {
//    return "0x" + stat_to_appendable_string( val );
//}

const size_t PORTS_PER_SCHAIN = 64;

static int stat_calc_schain_base_port( int node_base_port, int schain_index ) {
    return node_base_port + schain_index * PORTS_PER_SCHAIN;
}

static std::string stat_list_ids( const vec256_t& schains_ids_on_node ) {
    std::string s;
    const size_t cnt = schains_ids_on_node.size();
    for ( size_t i = 0; i < cnt; ++i ) {
        const item256_t& schain_id_on_node = schains_ids_on_node[i];
        if ( i > 0 )
            s += ", ";
        s += dev::toJS( schain_id_on_node.u256 );
    }
    return s;
}

static int stat_get_schain_index_in_node(
    dev::h256 schain_id, const vec256_t& schains_ids_on_node ) {
    const size_t cnt = schains_ids_on_node.size();
    for ( size_t i = 0; i < cnt; ++i ) {
        const item256_t& schain_id_on_node = schains_ids_on_node[i];
        if ( stat_to_appendable_string( schain_id ) ==
             stat_to_appendable_string( schain_id_on_node.u256 ) )
            return i;
        ++i;
    }
    throw new std::runtime_error(
        "S-Chain " + dev::toJS( schain_id ) +
        " is not found in the list: " + stat_list_ids( schains_ids_on_node ) );
}

static int stat_get_schain_base_port_on_node(
    dev::h256 schain_id, const vec256_t& schains_ids_on_node, int node_base_port ) {
    int schain_index = stat_get_schain_index_in_node( schain_id, schains_ids_on_node );
    return stat_calc_schain_base_port( node_base_port, schain_index );
}

// function compose_endpoints( jo_schain, node_dict, endpoint_type ) {
//    node_dict["http_endpoint_" + endpoint_type] =
//        "http://" + node_dict[endpoint_type] + ":" + jo_schain.data.computed.ports.httpRpcPort;
//    node_dict["https_endpoint_" + endpoint_type] =
//        "https://" + node_dict[endpoint_type] + ":" + jo_schain.data.computed.ports.httpsRpcPort;
//    node_dict["ws_endpoint_" + endpoint_type] =
//        "ws://" + node_dict[endpoint_type] + ":" + jo_schain.data.computed.ports.wsRpcPort;
//    node_dict["wss_endpoint_" + endpoint_type] =
//        "wss://" + node_dict[endpoint_type] + ":" + jo_schain.data.computed.ports.wssRpcPort;
//    node_dict["info_http_endpoint_" + endpoint_type] =
//        "http://" + node_dict[endpoint_type] + ":" +
//        jo_schain.data.computed.ports.infoHttpRpcPort;
//}

void stat_compute_endpoints( node_t& node ) {
    std::string ip = node.ip, domain = node.domainName;
    node.http_endpoint_ip =
        skutils::url( "http://" + ip + ":" + skutils::tools::format( "%d", node.httpRpcPort ) );
    node.http_endpoint_domain =
        skutils::url( "http://" + domain + ":" + skutils::tools::format( "%d", node.httpRpcPort ) );
    node.https_endpoint_ip =
        skutils::url( "https://" + ip + ":" + skutils::tools::format( "%d", node.httpsRpcPort ) );
    node.https_endpoint_domain = skutils::url(
        "https://" + domain + ":" + skutils::tools::format( "%d", node.httpsRpcPort ) );
    node.ws_endpoint_ip = "ws://" + ip + ":" + skutils::tools::format( "%d", node.wsRpcPort );
    node.ws_endpoint_domain =
        skutils::url( "ws://" + domain + ":" + skutils::tools::format( "%d", node.wsRpcPort ) );
    node.wss_endpoint_ip =
        skutils::url( "wss://" + ip + ":" + skutils::tools::format( "%d", node.wssRpcPort ) );
    node.wss_endpoint_domain =
        skutils::url( "wss://" + domain + ":" + skutils::tools::format( "%d", node.wssRpcPort ) );
    node.info_http_endpoint_ip =
        skutils::url( "http://" + ip + ":" + skutils::tools::format( "%d", node.infoHttpRpcPort ) );
    node.info_http_endpoint_domain = skutils::url(
        "https://" + domain + ":" + skutils::tools::format( "%d", node.infoHttpRpcPort ) );
}

enum SkaledPorts : int {
    PROPOSAL = 0,
    CATCHUP = 1,
    WS_JSON = 2,
    HTTP_JSON = 3,
    BINARY_CONSENSUS = 4,
    ZMQ_BROADCAST = 5,
    IMA_MONITORING = 6,
    WSS_JSON = 7,
    HTTPS_JSON = 8,
    INFO_HTTP_JSON = 9
};
static void stat_calc_ports( node_t& node ) {
    node.httpRpcPort = node.schain_base_port + SkaledPorts::HTTP_JSON;
    node.httpsRpcPort = node.schain_base_port + SkaledPorts::HTTPS_JSON;
    node.wsRpcPort = node.schain_base_port + SkaledPorts::WS_JSON;
    node.wssRpcPort = node.schain_base_port + SkaledPorts::WSS_JSON;
    node.infoHttpRpcPort = node.schain_base_port + SkaledPorts::INFO_HTTP_JSON;
}

dev::u256 get_schains_count(
    const skutils::url& u, const dev::u256& addressFrom, const dev::u256& addressSchainsInternal ) {
    static const char g_strContractMethodName[] = "numberOfSchains()";
    // 0x77ad87c1a3f5c981edbb22216a0b27bcf0b6c20e34df970e44c43bc8d7952fc6
    // 0x77ad87c1
    nlohmann::json joCall = stat_create_basic_call();
    nlohmann::json joParamsItem = nlohmann::json::object();
    joParamsItem["from"] = dev::toJS( addressFrom );
    joParamsItem["to"] = dev::toJS( addressSchainsInternal );
    joParamsItem["data"] = "0x77ad87c1";
    joCall["params"].push_back( joParamsItem );
    skutils::rest::client cli;
    // cli.optsSSL_ = optsSSL;
    cli.open( u );
    skutils::rest::data_t d = cli.call( joCall );
    if ( !d.err_s_.empty() )
        throw std::runtime_error(
            std::string( "Failed call to \"" ) + g_strContractMethodName + "\": " + d.err_s_ );
    if ( d.empty() )
        throw std::runtime_error( std::string( "Failed call to \"" ) + g_strContractMethodName +
                                  "\", EMPTY data received" );
    nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
    const nlohmann::json& joResult_numberOfSchains = joAnswer["result"];
    dev::u256 cntSChains( joResult_numberOfSchains.get< std::string >() );
    return cntSChains;
}

s_chain_t load_schain( const skutils::url& u, const dev::u256& addressFrom,
    const dev::u256& idxSChain, const dev::u256& /*cntSChains*/,
    const dev::u256& addressSchainsInternal, const dev::u256& addressNodes ) {
    //
    // load s-chain
    //
    s_chain_t s_chain;
    dev::u256 hash;
    {  // block
        static const char g_strContractMethodName[] = "schainsAtSystem(uint256)";
        // bytes32[] public schainsAtSystem;
        // 0xec79b50186e75f6719f28a07047b7e7cd13f13eb7b11a87b480887fe5f2df5aa
        // 0xec79b501
        nlohmann::json joCall = stat_create_basic_call();
        nlohmann::json joParamsItem = nlohmann::json::object();
        joParamsItem["from"] = dev::toJS( addressFrom );
        joParamsItem["to"] = dev::toJS( addressSchainsInternal );
        joParamsItem["data"] = "0xec79b501" + stat_to_appendable_string( idxSChain );
        joCall["params"].push_back( joParamsItem );
        skutils::rest::client cli;
        // cli.optsSSL_ = optsSSL;
        cli.open( u );
        skutils::rest::data_t d = cli.call( joCall );
        if ( !d.err_s_.empty() )
            throw std::runtime_error(
                std::string( "Failed call to \"" ) + g_strContractMethodName + "\": " + d.err_s_ );
        if ( d.empty() )
            throw std::runtime_error( std::string( "Failed call to \"" ) + g_strContractMethodName +
                                      "\", EMPTY data received" );
        nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
        const nlohmann::json& joResult = joAnswer["result"];
        hash = dev::u256( joResult.get< std::string >() );
    }  // block
    //
    {  // block
        static const char g_strContractMethodName[] = "schains(bytes32)";
        // mapping (bytes32 => Schain) public schains;
        // 0xb340c4b3db50a45804480331de552947d2c3df932cbfbc1edeacea1073b13f03
        // 0xb340c4b3
        // out:
        // struct Schain {
        //     string name;
        //     address owner;
        //     uint indexInOwnerList;
        //     uint8 partOfNode;
        //     uint lifetime;
        //     uint startDate;
        //     uint startBlock;
        //     uint deposit;
        //     uint64 index;
        //     uint generation;
        //     address originator;
        // }
        // 0000 0000000000000000000000000000000000000000000000000000000000000160 "name" offset = 352
        // 0020 0000000000000000000000007aa5e36aa15e93d10f4f26357c30f052dacdde5f
        // "owner":"0x7aa5E36AA15E93D10F4F26357C30F052DacDde5F" 0040
        // 0000000000000000000000000000000000000000000000000000000000000000 "indexInOwnerList":"0"
        // 0060 0000000000000000000000000000000000000000000000000000000000000000 "partOfNode":"0"
        // 0080 0000000000000000000000000000000000000000000000000000000000000005 "lifetime":"5"
        // 00A0 0000000000000000000000000000000000000000000000000000000061e6d984 "startDate"
        // 00C0 000000000000000000000000000000000000000000000000000000000000006b "startBlock"
        // 00E0 0000000000000000000000000000000000000000000000056bc75e2d63100000
        // "deposit":"100000000000000000000" 0100
        // 0000000000000000000000000000000000000000000000000000000000000000 index":"0" 0120
        // 0000000000000000000000000000000000000000000000000000000000000000 "generation":"0" 0140
        // 0000000000000000000000000000000000000000000000000000000000000000
        // "originator":"0x0000000000000000000000000000000000000000" 0160
        // 0000000000000000000000000000000000000000000000000000000000000007 length of "name" 0180
        // 426f623130303000000000000000000000000000000000000000000000000000 "name":"Bob1000"
        nlohmann::json joCall = stat_create_basic_call();
        nlohmann::json joParamsItem = nlohmann::json::object();
        joParamsItem["from"] = dev::toJS( addressFrom );
        joParamsItem["to"] = dev::toJS( addressSchainsInternal );
        joParamsItem["data"] = "0xb340c4b3" + stat_to_appendable_string( hash );
        joCall["params"].push_back( joParamsItem );
        skutils::rest::client cli;
        // cli.optsSSL_ = optsSSL;
        cli.open( u );
        skutils::rest::data_t d = cli.call( joCall );
        if ( !d.err_s_.empty() )
            throw std::runtime_error(
                std::string( "Failed call to \"" ) + g_strContractMethodName + "\": " + d.err_s_ );
        if ( d.empty() )
            throw std::runtime_error( std::string( "Failed call to \"" ) + g_strContractMethodName +
                                      "\", EMPTY data received" );
        nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
        const nlohmann::json& joResult = joAnswer["result"];
        std::string strResult = joResult.get< std::string >();
        vec256_t vec = stat_split_raw_answer( strResult );
        size_t i = 0;
        s_chain.name = stat_extract_string( vec, i++ );
        s_chain.schain_id = dev::sha3( s_chain.name );
        s_chain.chainId = stat_compute_chain_id_from_schain_name( s_chain.name );
        s_chain.owner = vec[i++].u256;          // address
        s_chain.indexInOwnerList = vec[i++].n;  // uint
        s_chain.partOfNode = vec[i++].n;        // uint8
        s_chain.lifetime = vec[i++].n;          // uint
        s_chain.startDate = vec[i++].n;         // uint
        s_chain.startBlock = vec[i++].u256;     // uint
        s_chain.deposit = vec[i++].u256;        // uint
        s_chain.index = vec[i++].n;             // uint64
        s_chain.generation = vec[i++].n;        // uint
        s_chain.originator = vec[i++].u256;     // address
    }                                           // block
    //
    // load s-chain parts
    //
    {  // block
        static const char g_strContractMethodName[] = "getNodesInGroup(bytes32)";
        // 0xb70a4223305cdb661a25301e0dd3a7d6dce139327a8f2e1ffeea696adcf2f42e
        // 0xb70a4223
        // out:
        // 0000 0000000000000000000000000000000000000000000000000000000000000020 // array offset
        // 0020 0000000000000000000000000000000000000000000000000000000000000002 // array length = 2
        // 0040 0000000000000000000000000000000000000000000000000000000000000001 // item[0]
        // 0060 0000000000000000000000000000000000000000000000000000000000000000 // item[0]
        nlohmann::json joCall = stat_create_basic_call();
        nlohmann::json joParamsItem = nlohmann::json::object();
        joParamsItem["from"] = dev::toJS( addressFrom );
        joParamsItem["to"] = dev::toJS( addressSchainsInternal );
        joParamsItem["data"] = "0xb70a4223" + stat_to_appendable_string( s_chain.schain_id );
        joCall["params"].push_back( joParamsItem );
        skutils::rest::client cli;
        // cli.optsSSL_ = optsSSL;
        cli.open( u );
        skutils::rest::data_t d = cli.call( joCall );
        if ( !d.err_s_.empty() )
            throw std::runtime_error(
                std::string( "Failed call to \"" ) + g_strContractMethodName + "\": " + d.err_s_ );
        if ( d.empty() )
            throw std::runtime_error( std::string( "Failed call to \"" ) + g_strContractMethodName +
                                      "\", EMPTY data received" );
        nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
        const nlohmann::json& joResult = joAnswer["result"];
        std::string strResult = joResult.get< std::string >();
        vec256_t vec = stat_split_raw_answer( strResult );
        vec256_t vecNodeIds = stat_extract_vector( vec, 0 );
        size_t idxNode, cntNodes = vecNodeIds.size();
        for ( idxNode = 0; idxNode < cntNodes; ++idxNode ) {
            const item256_t& node_id = vecNodeIds[idxNode];
            node_t node;
            {  // block
                static const char g_strContractMethodName[] = "nodes(uint256)";
                // Node[] public nodes;
                // 0x1c53c280643e9644acc64db8c4ceeb5d6e7c3ed526c08b82d73b7a30b16b3c27
                // 0x1c53c280
                // out:
                // struct Node {
                //    string name;
                //    bytes4 ip;
                //    bytes4 publicIP;
                //    uint16 port;
                //    bytes32[2] publicKey;
                //    uint startBlock;
                //    uint lastRewardDate;
                //    uint finishTime;
                //    NodeStatus status;
                //    uint validatorId;
                //}
                // 0000 0000000000000000000000000000000000000000000000000000000000000120 offset
                // "name" 0020 7f00000200000000000000000000000000000000000000000000000000000000 ip
                // 0040 7f00000200000000000000000000000000000000000000000000000000000000 publicIP
                // 0060 00000000000000000000000000000000000000000000000000000000000008d5 port 2261
                // 0080 000000000000000000000000000000000000000000000000000000000000006a ???
                // publicKey 00A0 0000000000000000000000000000000000000000000000000000000061e6d983
                // ??? startBlock 00C0
                // 0000000000000000000000000000000000000000000000000000000000000000 ???
                // lastRewardDate 00E0
                // 0000000000000000000000000000000000000000000000000000000000000000
                // ??? lastRewardDate 0100
                // 0000000000000000000000000000000000000000000000000000000000000001 ??? finishTime
                // 0120 0000000000000000000000000000000000000000000000000000000000000004 length
                // "name" 0140 4265617200000000000000000000000000000000000000000000000000000000
                // "name" value "Bear"
                nlohmann::json joCall = stat_create_basic_call();
                nlohmann::json joParamsItem = nlohmann::json::object();
                joParamsItem["from"] = dev::toJS( addressFrom );
                joParamsItem["to"] = dev::toJS( addressNodes );
                joParamsItem["data"] = "0x1c53c280" + stat_to_appendable_string( node_id.u256 );
                joCall["params"].push_back( joParamsItem );
                skutils::rest::client cli;
                // cli.optsSSL_ = optsSSL;
                cli.open( u );
                skutils::rest::data_t d = cli.call( joCall );
                if ( !d.err_s_.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\": " + d.err_s_ );
                if ( d.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\", EMPTY data received" );
                nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
                const nlohmann::json& joResult = joAnswer["result"];
                std::string strResult = joResult.get< std::string >();
                vec256_t vec = stat_split_raw_answer( strResult );
                size_t i = 0;
                node.node_id = node_id.u256;
                node.name = stat_extract_string( vec, i++ );
                node.ip = stat_extract_ipv4( vec, i++ );
                node.publicIP = stat_extract_ipv4( vec, i++ );
                node.nPort = ( int ) vec[i++].n;
            }  // block
            {  // block
                static const char g_strContractMethodName[] = "getNodeDomainName(uint256)";
                // function getNodeDomainName(uint nodeIndex)
                // 0xd31c48ede05d5ff086a9eedf48e201b69f7a1a854adcf2ac2d8af92bb7e848c2
                // 0xd31c48ed
                // out:
                // 0000 0000000000000000000000000000000000000000000000000000000000000020 // string
                // offset 0020 0000000000000000000000000000000000000000000000000000000000000015 //
                // string lengs 0040
                // 746573742e646f6d61696e2e6e616d652e686572650000000000000000000000 // string data
                nlohmann::json joCall = stat_create_basic_call();
                nlohmann::json joParamsItem = nlohmann::json::object();
                joParamsItem["from"] = dev::toJS( addressFrom );
                joParamsItem["to"] = dev::toJS( addressNodes );
                joParamsItem["data"] = "0xd31c48ed" + stat_to_appendable_string( node_id.u256 );
                joCall["params"].push_back( joParamsItem );
                skutils::rest::client cli;
                // cli.optsSSL_ = optsSSL;
                cli.open( u );
                skutils::rest::data_t d = cli.call( joCall );
                if ( !d.err_s_.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\": " + d.err_s_ );
                if ( d.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\", EMPTY data received" );
                nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
                const nlohmann::json& joResult = joAnswer["result"];
                std::string strResult = joResult.get< std::string >();
                vec256_t vec = stat_split_raw_answer( strResult );
                size_t i = 0;
                node.domainName = stat_extract_string( vec, i++ );
            }  // block
            {  // block
                static const char g_strContractMethodName[] = "isNodeInMaintenance(uint256)";
                // function isNodeInMaintenance(uint nodeIndex)
                // 0x5990e3cb693783f2ae688ffdd7d57079fc68f1648db65fd344b99e64a5c7fedf
                // 0x5990e3cb
                // out:
                // 0000 0000000000000000000000000000000000000000000000000000000000000000 bool
                nlohmann::json joCall = stat_create_basic_call();
                nlohmann::json joParamsItem = nlohmann::json::object();
                joParamsItem["from"] = dev::toJS( addressFrom );
                joParamsItem["to"] = dev::toJS( addressNodes );
                joParamsItem["data"] = "0x5990e3cb" + stat_to_appendable_string( node_id.u256 );
                joCall["params"].push_back( joParamsItem );
                skutils::rest::client cli;
                // cli.optsSSL_ = optsSSL;
                cli.open( u );
                skutils::rest::data_t d = cli.call( joCall );
                if ( !d.err_s_.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\": " + d.err_s_ );
                if ( d.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\", EMPTY data received" );
                nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
                const nlohmann::json& joResult = joAnswer["result"];
                std::string strResult = joResult.get< std::string >();
                vec256_t vec = stat_split_raw_answer( strResult );
                node.isMaintenance = ( vec[0].u256.is_zero() ) ? false : true;
            }  // block
            vec256_t vecSChainIds;
            {  // block
                static const char g_strContractMethodName[] = "getSchainIdsForNode(uint256)";
                // function getSchainIdsForNode(uint nodeIndex)
                // 0xe6695e68be45d5fd8ac911d1cca0faed4ebe398c5b26fdef14a65f9c2d91294d
                // 0xe6695e68
                // out:
                // 0000 0000000000000000000000000000000000000000000000000000000000000020 // array
                // offset 0020 0000000000000000000000000000000000000000000000000000000000000001 //
                // array length 0040
                // 975a4814cff8b9fd85b48879dade195028650b0a23f339ca81bd3b1231f72974
                // // array item[0]
                nlohmann::json joCall = stat_create_basic_call();
                nlohmann::json joParamsItem = nlohmann::json::object();
                joParamsItem["from"] = dev::toJS( addressFrom );
                joParamsItem["to"] = dev::toJS( addressSchainsInternal );
                joParamsItem["data"] = "0xe6695e68" + stat_to_appendable_string( node_id.u256 );
                joCall["params"].push_back( joParamsItem );
                skutils::rest::client cli;
                // cli.optsSSL_ = optsSSL;
                cli.open( u );
                skutils::rest::data_t d = cli.call( joCall );
                if ( !d.err_s_.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\": " + d.err_s_ );
                if ( d.empty() )
                    throw std::runtime_error( std::string( "Failed call to \"" ) +
                                              g_strContractMethodName + "\", EMPTY data received" );
                nlohmann::json joAnswer = nlohmann::json::parse( d.s_ );
                const nlohmann::json& joResult = joAnswer["result"];
                std::string strResult = joResult.get< std::string >();
                vec256_t vec = stat_split_raw_answer( strResult );
                vecSChainIds = stat_extract_vector( vec, 0 );
            }  // block
            node.schain_base_port = stat_get_schain_base_port_on_node( s_chain.schain_id,
                vecSChainIds,  // schain_ids
                node.nPort     // node_dict.base_port
            );
            stat_calc_ports( node );
            stat_compute_endpoints( node );
            s_chain.vecNodes.push_back( node );
        }  // for( idxNode = 0; idxNode < cntNodes; ++ idxNode )
    }      // block
    return s_chain;
}

vec_s_chains_t load_schains( const skutils::url& u, const dev::u256& addressFrom,
    const dev::u256& addressSchainsInternal, const dev::u256& addressNodes ) {
    vec_s_chains_t vec;
    dev::u256 cntSChains = get_schains_count( u, addressFrom, addressSchainsInternal );
    for ( dev::u256 idxSChain; idxSChain < cntSChains; ++idxSChain ) {
        s_chain_t s_chain = load_schain(
            u, addressFrom, idxSChain, cntSChains, addressSchainsInternal, addressNodes );
        vec.push_back( s_chain );
    }
    return vec;
}

nlohmann::json to_json( const node_t& node ) {
    nlohmann::json jo = nlohmann::json::object();
    jo["id"] = stat_to_0x_string( node.node_id );
    jo["name"] = node.name;
    jo["ip"] = node.ip;
    jo["publicIP"] = node.publicIP;
    jo["base_port"] = node.nPort;
    jo["domain"] = node.domainName;
    jo["isMaintenance"] = node.isMaintenance;
    jo["schain_base_port"] = node.schain_base_port;
    jo["http_endpoint_ip"] = node.http_endpoint_ip.str();
    jo["http_endpoint_domain"] = node.http_endpoint_domain.str();
    jo["https_endpoint_ip"] = node.https_endpoint_ip.str();
    jo["https_endpoint_domain"] = node.https_endpoint_domain.str();
    jo["ws_endpoint_ip"] = node.ws_endpoint_ip.str();
    jo["ws_endpoint_domain"] = node.ws_endpoint_domain.str();
    jo["wss_endpoint_ip"] = node.wss_endpoint_ip.str();
    jo["wss_endpoint_domain"] = node.wss_endpoint_domain.str();
    jo["info_http_endpoint_ip"] = node.info_http_endpoint_ip.str();
    jo["info_http_endpoint_domain"] = node.info_http_endpoint_domain.str();
    return jo;
}

static nlohmann::json stat_to_json( const vec_nodes_t& vecNodes ) {
    nlohmann::json jarr = nlohmann::json::array();
    vec_nodes_t::const_iterator itWalk = vecNodes.cbegin(), itEnd = vecNodes.cend();
    for ( ; itWalk != itEnd; ++itWalk ) {
        const node_t& node = ( *itWalk );
        jarr.push_back( to_json( node ) );
    }
    return jarr;
}

nlohmann::json to_json( const s_chain_t& s_chain ) {
    nlohmann::json jo = nlohmann::json::object();
    jo["name"] = s_chain.name;
    jo["owner"] = stat_to_0x_string( s_chain.owner );            // address
    jo["indexInOwnerList"] = s_chain.indexInOwnerList;           // uint
    jo["partOfNode"] = s_chain.partOfNode;                       // uint8
    jo["lifetime"] = s_chain.lifetime;                           // uint
    jo["startDate"] = s_chain.startDate;                         // uint
    jo["startBlock"] = stat_to_0x_string( s_chain.startBlock );  // uint
    jo["deposit"] = stat_to_0x_string( s_chain.deposit );        // uint
    jo["index"] = s_chain.index;                                 // uint64
    jo["generation"] = s_chain.generation;                       // uint
    jo["originator"] = stat_to_0x_string( s_chain.originator );  // address
    jo["computed"] = nlohmann::json::object();
    jo["computed"]["schain_id"] = stat_to_0x_string( s_chain.schain_id );  // keccak256(name)
    jo["computed"]["chainId"] = stat_to_0x_string( s_chain.chainId );      // part of schain_id
    jo["computed"]["nodes"] = stat_to_json( s_chain.vecNodes );
    return jo;
}

nlohmann::json to_json( const vec_s_chains_t& vec ) {
    nlohmann::json jarr = nlohmann::json::array();
    vec_s_chains_t::const_iterator itWalk = vec.cbegin(), itEnd = vec.cend();
    for ( ; itWalk != itEnd; ++itWalk ) {
        const s_chain_t& s_chain = ( *itWalk );
        jarr.push_back( to_json( s_chain ) );
    }
    return jarr;
}

static std::recursive_mutex g_mtx;
static const char g_queue_id[] = "skale-network-browser";
static skutils::dispatch::job_id_t g_idDispatchJob;
static std::shared_ptr< skutils::json_config_file_accessor > g_json_config_file_accessor;
static skutils::dispatch::job_t g_dispatch_job;
static vec_s_chains_t g_last_cached;

vec_s_chains_t refreshing_cached() {
    vec_s_chains_t vec;
    {  // block
        std::lock_guard lock( g_mtx );
        vec = g_last_cached;
    }  // block
    return vec;
}

bool stat_refresh_now( const skutils::url& u, const dev::u256& addressFrom,
    const dev::u256& addressSchainsInternal, const dev::u256& addressNodes ) {
    try {
        vec_s_chains_t vec = load_schains( u, addressFrom, addressSchainsInternal, addressNodes );
        nlohmann::json jarr = to_json( vec );
        std::lock_guard lock( g_mtx );
        g_last_cached = vec;
        clog( dev::VerbosityDebug, "snb" ) << ( cc::info( "SKALE NETWORK BROWSER" ) +
                                                cc::debug( " cached data: " ) + cc::j( jarr ) );
        return true;
    } catch ( std::exception& ex ) {
        std::string strErrorDescription = ex.what();
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Failed to download " ) + cc::note( "SKALE NETWORK" ) +
                   cc::error( " browsing data: " ) + cc::warn( strErrorDescription ) );
    } catch ( ... ) {
        std::string strErrorDescription = "unknown exception";
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Failed to download " ) + cc::note( "SKALE NETWORK" ) +
                   cc::error( " browsing data: " ) + cc::warn( strErrorDescription ) );
    }
    std::lock_guard lock( g_mtx );
    return false;
}

bool refreshing_start( const std::string& configPath ) {
    std::lock_guard lock( g_mtx );
    refreshing_stop();
    g_json_config_file_accessor.reset( new skutils::json_config_file_accessor( configPath ) );
    if ( skutils::json_config_file_accessor::g_strImaMainNetURL.empty() ) {
        clog( dev::VerbosityError, "snb" ) << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                                                cc::error( " Main Net URL is unknown" ) );
        return false;
    }
    nlohmann::json joConfig = g_json_config_file_accessor->getConfigJSON();
    if ( joConfig.count( "skaleConfig" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find \"skaleConfig\"" ) );
        return false;
    }
    const nlohmann::json& joSkaleConfig = joConfig["skaleConfig"];
    if ( joSkaleConfig.count( "nodeInfo" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error(
                       " Error in config.json file, cannot find \"skaleConfig\"/\"nodeInfo\"" ) );
        return false;
    }
    const nlohmann::json& joSkaleConfig_nodeInfo = joSkaleConfig["nodeInfo"];
    if ( joSkaleConfig_nodeInfo.count( "skale-manager" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"" ) );
        return false;
    }
    const nlohmann::json& joSkaleConfig_nodeInfo_sm = joSkaleConfig_nodeInfo["skale-manager"];

    if ( joSkaleConfig_nodeInfo_sm.count( "SchainsInternal" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error(
                       " Error in config.json file, cannot find "
                       "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"SchainsInternal\"" ) );
        return false;
    }
    const nlohmann::json& joSkaleConfig_nodeInfo_sm_SchainsInternal =
        joSkaleConfig_nodeInfo_sm["SchainsInternal"];
    if ( !joSkaleConfig_nodeInfo_sm_SchainsInternal.is_string() ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"SchainsInternal\" "
                              "as string value" ) );
        return false;
    }
    if ( joSkaleConfig_nodeInfo_sm.count( "Nodes" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"Nodes\"" ) );
        return false;
    }
    const nlohmann::json& joSkaleConfig_nodeInfo_sm_NodesInternal =
        joSkaleConfig_nodeInfo_sm["Nodes"];
    if ( !joSkaleConfig_nodeInfo_sm_NodesInternal.is_string() ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"Nodes\" as string "
                              "value" ) );
        return false;
    }
    if ( joSkaleConfig.count( "sChain" ) == 0 ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error(
                       " Error in config.json file, cannot find \"skaleConfig\"/\"sChain\"" ) );
        return false;
    }
    std::string strAddressFrom;
    const nlohmann::json& joSkaleConfig_sChain = joSkaleConfig["sChain"];
    if ( joSkaleConfig_sChain.count( "schainOwner" ) != 0 ) {
        const nlohmann::json& joSkaleConfig_sChain_schainOwner =
            joSkaleConfig_sChain["schainOwner"];
        if ( joSkaleConfig_sChain_schainOwner.is_string() )
            strAddressFrom =
                skutils::tools::trim_copy( joSkaleConfig_sChain_schainOwner.get< std::string >() );
    }
    size_t nIntervalSeconds = 15 * 60;
    if ( joSkaleConfig_nodeInfo.count( "skale-network-browser-refresh" ) > 0 ) {
        const nlohmann::json& joSkaleConfig_nodeInfo_refresh =
            joSkaleConfig_nodeInfo["skale-network-browser-refresh"];
        if ( joSkaleConfig_nodeInfo_refresh.is_number() )
            nIntervalSeconds = joSkaleConfig_nodeInfo_refresh.get< size_t >();
    }
    const std::string strAddressSchainsInternal =
        skutils::tools::trim_copy( joSkaleConfig_nodeInfo_sm_SchainsInternal.get< std::string >() );
    const std::string strAddressNodes =
        skutils::tools::trim_copy( joSkaleConfig_nodeInfo_sm_NodesInternal.get< std::string >() );
    if ( strAddressFrom.empty() ) {
        strAddressFrom = "0xaa0f3d9f62271ef8d668947af98e51487ba3f26b";
        clog( dev::VerbosityWarning, "snb" )
            << ( cc::warn( "SKALE NETWORK BROWSER WARNING:" ) +
                   cc::debug( "Using static address " ) + cc::info( strAddressFrom ) +
                   cc::debug( " for contract calls because no " ) + cc::info( "skaleConfig" ) +
                   cc::debug( "/" ) + cc::info( "sChain" ) + cc::debug( "/" ) +
                   cc::info( "schainOwner" ) + cc::debug( " value is provided" ) );
    }
    if ( strAddressSchainsInternal.empty() ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"SchainsInternal\" "
                              "as non-empty string value" ) );
        return false;
    }
    if ( strAddressNodes.empty() ) {
        clog( dev::VerbosityError, "snb" )
            << ( cc::fatal( "SKALE NETWORK BROWSER FAILURE:" ) +
                   cc::error( " Error in config.json file, cannot find "
                              "\"skaleConfig\"/\"nodeInfo\"/\"skale-manager\"/\"Nodes\" as "
                              "non-empty string value" ) );
        return false;
    }
    const skutils::url u( skutils::json_config_file_accessor::g_strImaMainNetURL );
    const dev::u256 addressFrom( strAddressFrom );
    const dev::u256 addressSchainsInternal( strAddressSchainsInternal );
    const dev::u256 addressNodes( strAddressNodes );
    stat_refresh_now( u, addressFrom, addressSchainsInternal, addressNodes );
    g_dispatch_job = [=]() -> void {
        stat_refresh_now( u, addressFrom, addressSchainsInternal, addressNodes );
    };
    skutils::dispatch::repeat( g_queue_id, g_dispatch_job,
        skutils::dispatch::duration_from_seconds( nIntervalSeconds ), &g_idDispatchJob );
    return true;
}

void refreshing_stop() {
    std::lock_guard lock( g_mtx );
    if ( !g_idDispatchJob.empty() ) {
        skutils::dispatch::stop( g_idDispatchJob );
        g_idDispatchJob.clear();
    }
    if ( g_json_config_file_accessor )
        g_json_config_file_accessor.reset();
    g_dispatch_job = skutils::dispatch::job_t();  // clear
}

vec_s_chains_t refreshing_do_now() {
    std::lock_guard lock( g_mtx );
    if ( ( !g_idDispatchJob.empty() ) && g_json_config_file_accessor && g_dispatch_job )
        g_dispatch_job();
    return refreshing_cached();
}

skutils::url refreshing_pick_s_chain_url( const std::string& strSChainName ) {
    if ( strSChainName.empty() )
        throw std::runtime_error(
            "SKALE NETWORK BROWSER FAILURE: Cannot pick S-Chain URL by empty S-Chain name" );
    vec_s_chains_t vec = refreshing_cached();
    if ( vec.empty() )
        throw std::runtime_error( "SKALE NETWORK BROWSER FAILURE: Cannot pick S-Chain \"" +
                                  strSChainName + "\" URL from empty cache" );
    const size_t cnt = vec.size();
    for ( size_t i = 0; i < cnt; ++i ) {
        const s_chain_t& s_chain = vec[i];
        if ( s_chain.name == strSChainName ) {
            const size_t cntNodes = s_chain.vecNodes.size();
            if ( cntNodes == 0 )
                throw std::runtime_error( "SKALE NETWORK BROWSER FAILURE: Cannot pick S-Chain \"" +
                                          strSChainName +
                                          "\" URL because there are no nodes in cache" );
            const size_t idxNode = rand() % cntNodes;
            const node_t& node = s_chain.vecNodes[idxNode];
            return node.http_endpoint_ip;
        }
    }
    throw std::runtime_error( "SKALE NETWORK BROWSER FAILURE: Cannot pick S-Chain \"" +
                              strSChainName + "\" URL because it's not in cache" );
}

}  // namespace browser
}  // namespace network
}  // namespace skale