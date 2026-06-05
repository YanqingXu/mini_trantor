// Unit tests for HttpContext incremental parser.
// Uses Buffer directly — no EventLoop or networking needed.

#include "mini/http/HttpContext.h"
#include "mini/net/Buffer.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

using namespace mini::http;
using namespace std::string_view_literals;

int main() {
    // 1. Parse simple GET request in one shot
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET /index.html HTTP/1.1\r\n"
                   "Host: localhost\r\n"
                   "Connection: keep-alive\r\n"
                   "\r\n"sv);

        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());

        const auto& req = ctx.request();
        assert(req.method() == HttpMethod::kGet);
        assert(req.path() == "/index.html");
        assert(req.query().empty());
        assert(req.version() == HttpVersion::kHttp11);
        assert(req.getHeader("Host") == "localhost");
        assert(req.getHeader("Connection") == "keep-alive");
        assert(buf.readableBytes() == 0);
        std::printf("  PASS: simple GET request\n");
    }

    // 2. Parse GET with query string
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET /search?q=reactor&page=1 HTTP/1.1\r\n"
                   "Host: example.com\r\n"
                   "\r\n"sv);

        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());

        const auto& req = ctx.request();
        assert(req.path() == "/search");
        assert(req.query() == "q=reactor&page=1");
        std::printf("  PASS: GET with query string\n");
    }

    // 3. Parse POST request with body
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("POST /api/data HTTP/1.1\r\n"
                   "Host: localhost\r\n"
                   "Content-Length: 13\r\n"
                   "\r\n"
                   "hello, world!"sv);

        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());

        const auto& req = ctx.request();
        assert(req.method() == HttpMethod::kPost);
        assert(req.path() == "/api/data");
        assert(req.body() == "hello, world!");
        assert(buf.readableBytes() == 0);
        std::printf("  PASS: POST with body\n");
    }

    // 4. Incremental parsing — partial request line
    {
        HttpContext ctx;
        mini::net::Buffer buf;

        // First chunk: partial request line (no \r\n yet)
        buf.append("GET /path HTTP"sv);
        assert(ctx.parseRequest(&buf));
        assert(!ctx.gotAll());
        assert(ctx.state() == HttpContext::kExpectRequestLine);

        // Second chunk: completes request line + headers
        buf.append("/1.1\r\nHost: test\r\n\r\n"sv);
        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().path() == "/path");
        assert(ctx.request().getHeader("Host") == "test");
        std::printf("  PASS: incremental request line\n");
    }

    // 5. Incremental parsing — partial headers
    {
        HttpContext ctx;
        mini::net::Buffer buf;

        buf.append("GET / HTTP/1.1\r\nHost: loc"sv);
        assert(ctx.parseRequest(&buf));
        assert(!ctx.gotAll());
        assert(ctx.state() == HttpContext::kExpectHeaders);

        buf.append("alhost\r\nX-Test: value\r\n\r\n"sv);
        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().getHeader("Host") == "localhost");
        assert(ctx.request().getHeader("X-Test") == "value");
        std::printf("  PASS: incremental headers\n");
    }

    // 6. Incremental parsing — partial body
    {
        HttpContext ctx;
        mini::net::Buffer buf;

        buf.append("POST /upload HTTP/1.1\r\n"
                   "Content-Length: 10\r\n"
                   "\r\n"
                   "hello"sv);
        assert(ctx.parseRequest(&buf));
        assert(!ctx.gotAll());
        assert(ctx.state() == HttpContext::kExpectBody);

        buf.append("world"sv);
        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().body() == "helloworld");
        std::printf("  PASS: incremental body\n");
    }

    // 7. Malformed request line — invalid method
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("FOOBAR /index HTTP/1.1\r\nHost: x\r\n\r\n"sv);
        assert(!ctx.parseRequest(&buf));
        std::printf("  PASS: malformed request line (invalid method)\n");
    }

    // 8. Malformed request line — missing path
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET\r\nHost: x\r\n\r\n"sv);
        assert(!ctx.parseRequest(&buf));
        std::printf("  PASS: malformed request line (missing path)\n");
    }

    // 9. Malformed header — missing colon
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n"sv);
        assert(!ctx.parseRequest(&buf));
        std::printf("  PASS: malformed header (no colon)\n");
    }

    // 10. HTTP/1.0 version
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET / HTTP/1.0\r\nHost: test\r\n\r\n"sv);

        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().version() == HttpVersion::kHttp10);
        std::printf("  PASS: HTTP/1.0 version\n");
    }

    // 11. Invalid version string
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET / HTTP/2.0\r\nHost: test\r\n\r\n"sv);
        assert(!ctx.parseRequest(&buf));
        std::printf("  PASS: invalid HTTP version\n");
    }

    // 12. Reset for keep-alive — parse two sequential requests
    {
        HttpContext ctx;
        mini::net::Buffer buf;

        // First request
        buf.append("GET /first HTTP/1.1\r\nHost: a\r\n\r\n"sv);
        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().path() == "/first");

        ctx.reset();

        // Second request
        buf.append("POST /second HTTP/1.1\r\n"
                   "Content-Length: 4\r\n"
                   "\r\n"
                   "data"sv);
        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().path() == "/second");
        assert(ctx.request().method() == HttpMethod::kPost);
        assert(ctx.request().body() == "data");
        std::printf("  PASS: reset for keep-alive\n");
    }

    // 13. Header with leading/trailing spaces trimmed
    {
        HttpContext ctx;
        mini::net::Buffer buf;
        buf.append("GET / HTTP/1.1\r\nX-Value:   spaced   \r\n\r\n"sv);

        assert(ctx.parseRequest(&buf));
        assert(ctx.gotAll());
        assert(ctx.request().getHeader("X-Value") == "spaced");
        std::printf("  PASS: header value trimming\n");
    }

    std::printf("All HttpContext unit tests passed.\n");
    return 0;
}
