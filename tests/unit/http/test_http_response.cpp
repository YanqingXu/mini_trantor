// Unit tests for HttpResponse serialization.

#include "mini/http/HttpResponse.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace mini::http;

int main() {
    // 1. Default response serializes correctly
    {
        HttpResponse resp;
        resp.setStatusCode(HttpResponse::k200Ok);
        resp.setStatusMessage("OK");
        resp.setBody("hello");

        std::string s = resp.serialize();
        assert(s.find("HTTP/1.1 200 OK\r\n") == 0);
        assert(s.find("Content-Length: 5\r\n") != std::string::npos);
        assert(s.find("Connection: keep-alive\r\n") != std::string::npos);
        assert(s.find("\r\n\r\nhello") != std::string::npos);
        std::printf("  PASS: default response serialization\n");
    }

    // 2. Close connection response
    {
        HttpResponse resp(true);
        resp.setStatusCode(HttpResponse::k404NotFound);
        resp.setStatusMessage("Not Found");

        std::string s = resp.serialize();
        assert(s.find("HTTP/1.1 404 Not Found\r\n") == 0);
        assert(s.find("Connection: close\r\n") != std::string::npos);
        assert(s.find("Content-Length: 0\r\n") != std::string::npos);
        std::printf("  PASS: close connection response\n");
    }

    // 3. Custom headers
    {
        HttpResponse resp;
        resp.setStatusCode(HttpResponse::k200Ok);
        resp.setStatusMessage("OK");
        resp.setContentType("application/json");
        resp.addHeader("X-Custom", "test-value");
        resp.setBody(R"({"key":"value"})");

        std::string s = resp.serialize();
        assert(s.find("Content-Type: application/json\r\n") != std::string::npos);
        assert(s.find("X-Custom: test-value\r\n") != std::string::npos);
        assert(s.find("Content-Length: 15\r\n") != std::string::npos);
        std::printf("  PASS: custom headers\n");
    }

    // 4. 400 Bad Request
    {
        HttpResponse resp(true);
        resp.setStatusCode(HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");

        std::string s = resp.serialize();
        assert(s.find("HTTP/1.1 400 Bad Request\r\n") == 0);
        assert(s.find("Connection: close\r\n") != std::string::npos);
        std::printf("  PASS: 400 Bad Request response\n");
    }

    // 5. Empty body has Content-Length: 0
    {
        HttpResponse resp;
        resp.setStatusCode(HttpResponse::k204NoContent);
        resp.setStatusMessage("No Content");

        std::string s = resp.serialize();
        assert(s.find("Content-Length: 0\r\n") != std::string::npos);
        std::printf("  PASS: empty body Content-Length 0\n");
    }

    // 6. setCloseConnection toggling
    {
        HttpResponse resp;
        assert(!resp.closeConnection());
        resp.setCloseConnection(true);
        assert(resp.closeConnection());
        std::string s = resp.serialize();
        assert(s.find("Connection: close\r\n") != std::string::npos);
        std::printf("  PASS: setCloseConnection toggle\n");
    }

    std::printf("All HttpResponse unit tests passed.\n");
    return 0;
}
