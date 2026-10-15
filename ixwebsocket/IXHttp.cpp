/*
 *  IXHttp.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include "IXHttp.h"

#include "IXCancellationRequest.h"
#include "IXGzipCodec.h"
#include "IXSocket.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace ix
{
    std::string Http::trim(const std::string& str)
    {
        std::string out;
        for (auto c : str)
        {
            if (c != ' ' && c != '\n' && c != '\r')
            {
                out += c;
            }
        }

        return out;
    }

    std::pair<std::string, int> Http::parseStatusLine(const std::string& line)
    {
        // Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
        std::string token;
        std::stringstream tokenStream(line);
        std::vector<std::string> tokens;

        // Split by ' '
        while (std::getline(tokenStream, token, ' '))
        {
            tokens.push_back(token);
        }

        std::string httpVersion;
        if (tokens.size() >= 1)
        {
            httpVersion = trim(tokens[0]);
        }

        int statusCode = -1;
        if (tokens.size() >= 2)
        {
            std::stringstream ss;
            ss << trim(tokens[1]);
            ss >> statusCode;
        }

        return std::make_pair(httpVersion, statusCode);
    }

    std::tuple<std::string, std::string, std::string> Http::parseRequestLine(
        const std::string& line)
    {
        // Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
        std::string token;
        std::stringstream tokenStream(line);
        std::vector<std::string> tokens;

        // Split by ' '
        while (std::getline(tokenStream, token, ' '))
        {
            tokens.push_back(token);
        }

        std::string method;
        if (tokens.size() >= 1)
        {
            method = trim(tokens[0]);
        }

        std::string requestUri;
        if (tokens.size() >= 2)
        {
            requestUri = trim(tokens[1]);
        }

        std::string httpVersion;
        if (tokens.size() >= 3)
        {
            httpVersion = trim(tokens[2]);
        }

        return std::make_tuple(method, requestUri, httpVersion);
    }

    std::tuple<bool, std::string, HttpRequestPtr> Http::parseRequest(
        std::unique_ptr<Socket>& socket, int timeoutSecs)
    {
        std::atomic<bool> requestInitCancellation(false);

        auto isCancellationRequested =
            makeCancellationRequestWithTimeout(timeoutSecs, requestInitCancellation);

        uint64_t contentLength = 0;
        auto head = parseRequestHead(socket, contentLength, isCancellationRequested);
        if (!std::get<0>(head))
        {
            return head;
        }

        auto request = std::get<2>(head);
        auto body = readRequestBody(socket, request, contentLength, isCancellationRequested);
        if (!body.first)
        {
            return std::make_tuple(false, body.second, HttpRequestPtr());
        }

        return std::make_tuple(true, "", request);
    }

    std::tuple<bool, std::string, HttpRequestPtr> Http::parseRequestHead(
        std::unique_ptr<Socket>& socket,
        uint64_t& outContentLength,
        const CancellationRequest& isCancellationRequested)
    {
        HttpRequestPtr httpRequest;
        outContentLength = 0;

        // Read first line
        auto lineResult = socket->readLine(isCancellationRequested);
        auto lineValid = lineResult.first;
        auto line = lineResult.second;

        if (!lineValid)
        {
            return std::make_tuple(false, "Error reading HTTP request line", httpRequest);
        }

        // Parse request line (GET /foo HTTP/1.1\r\n)
        auto requestLine = Http::parseRequestLine(line);
        auto method = std::get<0>(requestLine);
        auto uri = std::get<1>(requestLine);
        auto httpVersion = std::get<2>(requestLine);

        // Retrieve and validate HTTP headers
        auto result = parseHttpHeaders(socket, isCancellationRequested);
        auto headersValid = result.first;
        auto headers = result.second;

        if (!headersValid)
        {
            return std::make_tuple(false, "Error parsing HTTP headers", httpRequest);
        }

        if (headers.find("Content-Length") != headers.end())
        {
            // 64-bit, so a body past 2 GiB is not refused by the parser
            const std::string& value = headers["Content-Length"];
            const char* p = value.c_str();
            char* p_end{};
            errno = 0;
            unsigned long long val = std::strtoull(p, &p_end, 10);
            if (p_end == p            // invalid argument
                || *p_end != '\0'     // trailing garbage
                || errno == ERANGE    // out of range
                || value.find('-') != std::string::npos)
            {
                return std::make_tuple(
                    false, "Error parsing HTTP Header 'Content-Length'", httpRequest);
            }
            outContentLength = val;
        }

        httpRequest = std::make_shared<HttpRequest>(uri, method, httpVersion, std::string(), headers);
        return std::make_tuple(true, "", httpRequest);
    }

    std::pair<bool, std::string> Http::readRequestBody(
        std::unique_ptr<Socket>& socket,
        HttpRequestPtr request,
        uint64_t contentLength,
        const CancellationRequest& isCancellationRequested)
    {
        if (contentLength > (std::numeric_limits<size_t>::max)())
        {
            return std::make_pair(false, "Error: 'Content-Length' is too large to buffer");
        }

        if (contentLength > 0)
        {
            auto res = socket->readBytes(
                (size_t) contentLength, nullptr, nullptr, isCancellationRequested);
            if (!res.first)
            {
                return std::make_pair(false, std::string("Error reading request: ") + res.second);
            }
            request->body = std::move(res.second);
        }

        // If the content was compressed with gzip, decode it
        if (request->headers["Content-Encoding"] == "gzip")
        {
#ifdef IXWEBSOCKET_USE_ZLIB
            std::string decompressedPayload;
            if (!gzipDecompress(request->body, decompressedPayload))
            {
                return std::make_pair(false,
                                      std::string("Error during gzip decompression of the body"));
            }
            request->body = decompressedPayload;
#else
            return std::make_pair(false, "ixwebsocket was not compiled with gzip support on");
#endif
        }

        return std::make_pair(true, "");
    }

    std::pair<bool, std::string> Http::streamRequestBody(
        std::unique_ptr<Socket>& socket,
        HttpRequestPtr request,
        uint64_t contentLength,
        const std::string& filePath,
        int idleTimeoutSecs,
        const CancellationRequest& isCancellationRequested)
    {
        if (request->headers["Content-Encoding"] == "gzip")
        {
            return std::make_pair(false, "A gzip body cannot be streamed to a file");
        }
        if (contentLength > (std::numeric_limits<size_t>::max)())
        {
            return std::make_pair(false, "Error: 'Content-Length' is too large");
        }

        std::ofstream file;
        if (!filePath.empty())
        {
            file.open(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return std::make_pair(false, "Cannot create the body file " + filePath);
            }
        }

        // The idle clock restarts on every chunk, so a slow upload lives as long as
        // bytes keep arriving while a stalled one is cut
        auto lastActivity = std::chrono::steady_clock::now();
        bool writeFailed = false;
        auto onChunk = [&](const std::string& chunk) {
            lastActivity = std::chrono::steady_clock::now();
            if (file.is_open() && !writeFailed)
            {
                file.write(chunk.data(), (std::streamsize) chunk.size());
                writeFailed = !file;
            }
        };
        auto cancel = [&]() -> bool {
            if (isCancellationRequested && isCancellationRequested()) return true;
            if (writeFailed) return true;
            if (idleTimeoutSecs > 0
                && std::chrono::steady_clock::now() - lastActivity
                       > std::chrono::seconds(idleTimeoutSecs))
            {
                return true;
            }
            return false;
        };

        bool ok = true;
        std::string error;
        if (contentLength > 0)
        {
            auto res = socket->readBytes((size_t) contentLength, nullptr, onChunk, cancel);
            ok = res.first;
            error = res.second;
        }

        if (ok && file.is_open())
        {
            file.close();
            ok = !writeFailed && (bool) file;
            if (!ok) error = "Cannot write the body file " + filePath;
        }
        else if (!ok && file.is_open())
        {
            file.close();
        }

        if (!ok)
        {
            if (!filePath.empty()) std::remove(filePath.c_str());
            if (writeFailed) error = "Cannot write the body file " + filePath;
            return std::make_pair(false, std::string("Error reading request body: ") + error);
        }

        request->bodyFilePath = filePath;
        return std::make_pair(true, "");
    }

    bool Http::sendResponse(HttpResponsePtr response, std::unique_ptr<Socket>& socket)
    {
        // Write the response to the socket
        std::stringstream ss;
        ss << "HTTP/1.1 ";
        ss << response->statusCode;
        ss << " ";
        ss << response->description;
        ss << "\r\n";

        if (!socket->writeBytes(ss.str(), nullptr))
        {
            return false;
        }

        // Write headers
        ss.str("");
        ss << "Content-Length: " << response->body.size() << "\r\n";
        for (auto&& it : response->headers)
        {
            ss << it.first << ": " << it.second << "\r\n";
        }
        ss << "\r\n";

        if (!socket->writeBytes(ss.str(), nullptr))
        {
            return false;
        }

        return response->body.empty() ? true : socket->writeBytes(response->body, nullptr);
    }
} // namespace ix
