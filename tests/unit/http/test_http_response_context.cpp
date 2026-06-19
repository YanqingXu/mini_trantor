// Unit tests for HttpResponseContext incremental parser.
// Uses Buffer directly — no EventLoop or networking needed.

#include "mini/http/HttpResponseContext.h"
#include "mini/net/Buffer.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

using namespace mini::http;
using namespace std::string_view_literals;

int main() {
    // 1. Parse simple 200 response with body
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: 5\r\n"
                   "\r\n"
                   "hello"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        const auto& resp = ctx.response();
        assert(resp.statusCode() == HttpResponse::k200Ok);
        assert(resp.statusMessage() == "OK");
        assert(resp.body() == "hello");
        assert(buf.readableBytes() == 0);
        std::printf("  PASS: simple 200 with body\n");
    }

    // 2. Parse 404 without body
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 404 Not Found\r\n"
                   "Content-Length: 0\r\n"
                   "\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        const auto& resp = ctx.response();
        assert(resp.statusCode() == HttpResponse::k404NotFound);
        assert(resp.statusMessage() == "Not Found");
        assert(resp.body().empty());
        std::printf("  PASS: 404 without body\n");
    }

    // 3. 500 Internal Server Error
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.0 500 Internal Server Error\r\n"
                   "Content-Length: 12\r\n"
                   "\r\n"
                   "server error"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        const auto& resp = ctx.response();
        assert(resp.statusCode() == HttpResponse::k500InternalServerError);
        assert(resp.statusMessage() == "Internal Server Error");
        assert(resp.body() == "server error");
        std::printf("  PASS: 500 with body\n");
    }

    // 4. Incremental parsing (split across multiple buffer chunks)
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;

        buf.append("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: 5\r\n"
                   "\r\n"sv);
        assert(!ctx.gotAll());
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());  // Waiting for body

        buf.append("he"sv);
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());  // Only 2/5 bytes

        buf.append("llo"sv);
        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        assert(ctx.response().body() == "hello");
        std::printf("  PASS: incremental parsing\n");
    }

    // 5. Headers split across chunks
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;

        buf.append("HTTP/1.1 200 OK\r\n"
                   "Content-"sv);
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());

        buf.append("Length: 3\r\n"sv);
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());

        buf.append("\r\nab"sv);
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());  // Only 2/3 body bytes

        buf.append("c"sv);
        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().body() == "abc");
        std::printf("  PASS: header split across chunks\n");
    }

    // 6. Connection: close detection
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\n"
                   "Connection: close\r\n"
                   "Content-Length: 0\r\n"
                   "\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().closeConnection());
        std::printf("  PASS: Connection: close detection\n");
    }

    // 7. Multiple headers
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Cache-Control: no-cache\r\n"
                   "X-Custom: hello\r\n"
                   "Content-Length: 0\r\n"
                   "\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        const auto& hs = ctx.response().headers();
        assert(hs.find("Content-Type") != hs.end());
        assert(hs.at("Content-Type") == "application/json");
        assert(hs.at("Cache-Control") == "no-cache");
        assert(hs.at("X-Custom") == "hello");
        std::printf("  PASS: multiple headers\n");
    }

    // 8. Reset for keep-alive reuse (simulating second response on same connection)
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;

        // First response
        buf.append("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 1\r\n"
                   "\r\n"
                   "a"sv);
        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());

        // Reset for second response
        ctx.reset();
        buf.append("HTTP/1.1 403 Forbidden\r\n"
                   "Content-Length: 0\r\n"
                   "\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().statusCode() == HttpResponse::k403Forbidden);
        assert(ctx.response().statusMessage() == "Forbidden");
        std::printf("  PASS: reset for keep-alive reuse\n");
    }

    // 9. Empty response (no headers, no body)
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 204 No Content\r\n"
                   "\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().statusCode() == HttpResponse::k204NoContent);
        assert(ctx.response().body().empty());
        std::printf("  PASS: 204 No Content\n");
    }

    // 10. Malformed status line
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("NOT_HTTP 200 OK\r\n\r\n", 19);

        assert(!ctx.parseResponse(&buf));
        std::printf("  PASS: malformed status line rejected\n");
    }

    // 11. Incremental body: partial body does not trigger gotAll
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123"sv);

        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());  // Only 4/10 body bytes

        buf.append("4567"sv);
        assert(ctx.parseResponse(&buf));
        assert(!ctx.gotAll());  // Only 8/10

        buf.append("89"sv);
        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().body() == "0123456789");
        std::printf("  PASS: incremental body parsing\n");
    }

    // 12. Malformed Content-Length (non-numeric)
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nbody"sv);

        assert(!ctx.parseResponse(&buf));
        std::printf("  PASS: malformed Content-Length rejected\n");
    }

    // 13. Empty buffer input
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;

        assert(ctx.parseResponse(&buf));  // No data to parse, returns true
        assert(!ctx.gotAll());             // But not complete
        std::printf("  PASS: empty buffer handled gracefully\n");
    }

    // 14. Four-digit status code rejected
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 2000 OK\r\n\r\n"sv);

        assert(!ctx.parseResponse(&buf));
        std::printf("  PASS: four-digit status code rejected\n");
    }

    // 15. Connection: CLOSE (uppercase) detected
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\nConnection: CLOSE\r\n\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(ctx.response().closeConnection());
        std::printf("  PASS: Connection: CLOSE (uppercase) detected\n");
    }

    // 16. Connection: keep-alive keeps connection open
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(!ctx.response().closeConnection());
        std::printf("  PASS: Connection: keep-alive keeps open\n");
    }

    // 17. Default is keep-alive when no Connection header in HTTP/1.1
    {
        HttpResponseContext ctx;
        mini::net::Buffer buf;
        buf.append("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"sv);

        assert(ctx.parseResponse(&buf));
        assert(ctx.gotAll());
        assert(!ctx.response().closeConnection());
        std::printf("  PASS: HTTP/1.1 default keeps open\n");
    }

    std::printf("\nAll HttpResponseContext tests passed.\n");
    return 0;
}
