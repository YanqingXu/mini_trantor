#include "mini/http/HttpRequest.h"

#include <cstring>

namespace mini::http {

HttpMethod HttpRequest::parseMethod(std::string_view methodStr) {
    if (methodStr == "GET") return HttpMethod::kGet;
    if (methodStr == "POST") return HttpMethod::kPost;
    if (methodStr == "PUT") return HttpMethod::kPut;
    if (methodStr == "DELETE") return HttpMethod::kDelete;
    if (methodStr == "HEAD") return HttpMethod::kHead;
    if (methodStr == "OPTIONS") return HttpMethod::kOptions;
    if (methodStr == "PATCH") return HttpMethod::kPatch;
    return HttpMethod::kInvalid;
}

const char* HttpRequest::methodString() const {
    switch (method_) {
        case HttpMethod::kGet:     return "GET";
        case HttpMethod::kPost:    return "POST";
        case HttpMethod::kPut:     return "PUT";
        case HttpMethod::kDelete:  return "DELETE";
        case HttpMethod::kHead:    return "HEAD";
        case HttpMethod::kOptions: return "OPTIONS";
        case HttpMethod::kPatch:   return "PATCH";
        default:                   return "UNKNOWN";
    }
}

}  // namespace mini::http
