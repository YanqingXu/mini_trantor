// Unit tests for HttpRequest parsing and HttpResponse serialization.
// Pure value-object tests — no EventLoop or networking needed.

#include "mini/http/HttpRequest.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace mini::http;

int main() {
    // 1. parseMethod — known methods
    {
        assert(HttpRequest::parseMethod("GET") == HttpMethod::kGet);
        assert(HttpRequest::parseMethod("POST") == HttpMethod::kPost);
        assert(HttpRequest::parseMethod("PUT") == HttpMethod::kPut);
        assert(HttpRequest::parseMethod("DELETE") == HttpMethod::kDelete);
        assert(HttpRequest::parseMethod("HEAD") == HttpMethod::kHead);
        assert(HttpRequest::parseMethod("OPTIONS") == HttpMethod::kOptions);
        assert(HttpRequest::parseMethod("PATCH") == HttpMethod::kPatch);
        std::printf("  PASS: parseMethod known methods\n");
    }

    // 2. parseMethod — invalid method
    {
        assert(HttpRequest::parseMethod("FOOBAR") == HttpMethod::kInvalid);
        assert(HttpRequest::parseMethod("") == HttpMethod::kInvalid);
        assert(HttpRequest::parseMethod("get") == HttpMethod::kInvalid);
        std::printf("  PASS: parseMethod invalid methods\n");
    }

    // 3. methodString round-trip
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        assert(std::string(req.methodString()) == "GET");
        req.setMethod(HttpMethod::kPost);
        assert(std::string(req.methodString()) == "POST");
        req.setMethod(HttpMethod::kInvalid);
        assert(std::string(req.methodString()) == "UNKNOWN");
        std::printf("  PASS: methodString round-trip\n");
    }

    // 4. Header storage and retrieval
    {
        HttpRequest req;
        req.addHeader("Host", "example.com");
        req.addHeader("Content-Type", "text/plain");
        assert(req.getHeader("Host") == "example.com");
        assert(req.getHeader("Content-Type") == "text/plain");
        assert(req.getHeader("Missing") == "");
        assert(req.headers().size() == 2);
        std::printf("  PASS: header storage and retrieval\n");
    }

    // 5. Path, query, body, version setters/getters
    {
        HttpRequest req;
        req.setPath("/api/test");
        req.setQuery("foo=bar&baz=1");
        req.setBody("hello body");
        req.setVersion(HttpVersion::kHttp11);
        assert(req.path() == "/api/test");
        assert(req.query() == "foo=bar&baz=1");
        assert(req.body() == "hello body");
        assert(req.version() == HttpVersion::kHttp11);
        std::printf("  PASS: path, query, body, version\n");
    }

    // 6. reset clears all fields
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kPost);
        req.setPath("/path");
        req.setQuery("q=1");
        req.setBody("data");
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("X-Custom", "value");

        req.reset();

        assert(req.method() == HttpMethod::kInvalid);
        assert(req.path().empty());
        assert(req.query().empty());
        assert(req.body().empty());
        assert(req.version() == HttpVersion::kUnknown);
        assert(req.headers().empty());
        std::printf("  PASS: reset clears all fields\n");
    }

    std::printf("All HttpRequest unit tests passed.\n");
    return 0;
}
