/*
 *  IXHttpServer.h
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone, Inc. All rights reserved.
 */

#pragma once

#include "IXHttp.h"
#include "IXWebSocket.h"
#include "IXWebSocketServer.h"
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility> // pair

namespace ix
{
    // What to do with a request's body, decided once its headers are in
    struct HttpBodyPolicy
    {
        // Write the body to this file in chunks instead of holding it in memory
        std::string streamToFilePath;
        // Read and drop the body, for a request the server already knows it refuses
        bool discardBody = false;
        // Answer with this once the body is consumed, without the connection callback
        HttpResponsePtr earlyResponse;
        // Cut a streamed or discarded body when no byte arrives for this long. The
        // whole-request timeout applies when 0.
        int idleTimeoutSecs = 0;
    };

    class HttpServer final : public WebSocketServer
    {
    public:
        using OnConnectionCallback =
            std::function<HttpResponsePtr(HttpRequestPtr, std::shared_ptr<ConnectionState>)>;
        // Called on the connection thread after the headers and before the body,
        // with request->body empty. Absent, every body is read into memory.
        using OnRequestHeadersCallback = std::function<HttpBodyPolicy(HttpRequestPtr)>;

        HttpServer(int port = SocketServer::kDefaultPort,
                   const std::string& host = SocketServer::kDefaultHost,
                   int backlog = SocketServer::kDefaultTcpBacklog,
                   size_t maxConnections = SocketServer::kDefaultMaxConnections,
                   int addressFamily = SocketServer::kDefaultAddressFamily,
                   int timeoutSecs = HttpServer::kDefaultTimeoutSecs,
                   int handshakeTimeoutSecs = WebSocketServer::kDefaultHandShakeTimeoutSecs);

        void setOnConnectionCallback(const OnConnectionCallback& callback);
        void setOnRequestHeadersCallback(const OnRequestHeadersCallback& callback);

        void makeRedirectServer(const std::string& redirectUrl);

        void makeDebugServer();

        int getTimeoutSecs();

    private:
        // Member variables
        OnConnectionCallback _onConnectionCallback;
        OnRequestHeadersCallback _onRequestHeadersCallback;

        const static int kDefaultTimeoutSecs;
        int _timeoutSecs;

        // Methods
        virtual void handleConnection(std::unique_ptr<Socket>,
                                      std::shared_ptr<ConnectionState> connectionState) final;

        void setDefaultConnectionCallback();
    };
} // namespace ix
