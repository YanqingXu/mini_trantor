#pragma once

// noncopyable 提供显式禁止拷贝的基础能力。
// 用于表达拥有线程、生命周期或资源语义的对象不能被意外复制。

namespace mini::base {

class noncopyable {
protected:
    noncopyable() = default;
    ~noncopyable() = default;

public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
};

}  // namespace mini::base
