// Payload — shared payload 实现。

#include "mini/net/buffer/Payload.h"

namespace mini::net::buffer {

Payload::Payload(std::string data)
    : data_(std::move(data)) {
}

std::string_view Payload::view() const noexcept {
    return data_;
}

bool Payload::empty() const noexcept {
    return data_.empty();
}

std::size_t Payload::size() const noexcept {
    return data_.size();
}

void Payload::reset(std::string_view data) {
    data_.assign(data);
}

void Payload::reset(std::string&& data) {
    data_ = std::move(data);
}

void Payload::clear() {
    data_.clear();
}

}  // namespace mini::net::buffer
