#if ( !defined SKUTILS_HTTP_PG_H )
#define SKUTILS_HTTP_PG_H 1

#include <atomic>

#include <folly/Memory.h>
#include <proxygen/httpserver/RequestHandler.h>

#include <folly/io/async/EventBaseManager.h>
//#include <folly/portability/GFlags.h>
#include <folly/portability/Unistd.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>

//#include <nlohmann/json.hpp>
#include <json.hpp>

#include <skutils/http.h>

namespace proxygen {
class ResponseHandler;
}

namespace skutils {
namespace http_pg {

class server_side_request_handler;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class request_sink {
    std::atomic_uint64_t reqCount_{0};

public:
    request_sink();
    virtual ~request_sink();
    virtual void OnRecordRequestCountIncrement();
    virtual uint64_t getRequestCount();
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class request_site : public proxygen::RequestHandler {
    request_sink& sink_;
    std::unique_ptr< folly::IOBuf > body_;
    server_side_request_handler* pSSRQ_;
    static std::atomic_uint64_t g_instance_counter;
    uint64_t nInstanceNumber_;
    std::string strLogPrefix_;

public:
    std::string strHttpMethod_, strOrigin_, strPath_;
    int ipVer_ = -1;

    explicit request_site( request_sink& a_sink, server_side_request_handler* pSSRQ );
    ~request_site() override;

    void onRequest( std::unique_ptr< proxygen::HTTPMessage > headers ) noexcept override;
    void onBody( std::unique_ptr< folly::IOBuf > body ) noexcept override;
    void onEOM() noexcept override;
    void onUpgrade( proxygen::UpgradeProtocol proto ) noexcept override;
    void requestComplete() noexcept override;
    void onError( proxygen::ProxygenError err ) noexcept override;
};  /// class request_site


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class request_site_factory : public proxygen::RequestHandlerFactory {
    folly::ThreadLocalPtr< request_sink > sink_;
    server_side_request_handler* pSSRQ_ = nullptr;

public:
    request_site_factory( server_side_request_handler* pSSRQ );
    ~request_site_factory() override;
    void onServerStart( folly::EventBase* /*evb*/ ) noexcept override;
    void onServerStop() noexcept override;
    proxygen::RequestHandler* onRequest(
        proxygen::RequestHandler*, proxygen::HTTPMessage* ) noexcept override;
};  /// class request_site_factory

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class server_side_request_handler {
public:
    server_side_request_handler();
    virtual ~server_side_request_handler();
    static nlohmann::json json_from_error_text(
        const char* strErrorDescription, const nlohmann::json& joID );
    static std::string answer_from_error_text(
        const char* strErrorDescription, const nlohmann::json& joID );
    virtual nlohmann::json onRequest(
        const nlohmann::json& joIn, const std::string& strOrigin, int ipVer ) = 0;
};  /// class server_side_request_handler

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class server : public server_side_request_handler {
    std::thread thread_;
    std::unique_ptr< proxygen::HTTPServer > server_;
    pg_on_request_handler_t h_;
    std::string strLogPrefix_;

public:
    server( pg_on_request_handler_t h );
    ~server() override;
    bool start();
    void stop();
    nlohmann::json onRequest(
        const nlohmann::json& joIn, const std::string& strOrigin, int ipVer ) override;
};  /// class server

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

};  // namespace http_pg
};  // namespace skutils

#endif  /// SKUTILS_HTTP_PG_H
