#pragma once

#include <cstddef>

namespace std {

template<class T>
struct span
{
    explicit span(T* d = {}, size_t l = {}) : data(d), len(l) {}
    span(T* from, T* to) : data(from), len(to-from) { assert(from <= to); }
    T& operator[](size_t i) { return data[i]; }
    T* begin() { return data; }
    T* end() { return data + len; }
    auto size() const { return len; }
    explicit operator bool() const { return size() > 0; }

private:
    T* data;
    size_t len;
};

} // namespace std
