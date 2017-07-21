#pragma once

namespace opcua {

template<typename T>
class Span {
 public:
  Span() : size_{0} {}
  Span(const Span<std::remove_const_t<T>>& source) : data_{source.data_}, size_{source.size_} {}
  Span(T* data, size_t size) : data_{data}, size_{size} {}

  size_t size() const { return size_; }
  T* data() const { return data_; }
  bool empty() const { return size_ == 0; }

  T& operator[](size_t index) {
    assert(index < size_);
    return data_[index];
  }

  const T& operator[](size_t index) const {
    assert(index < size_);
    return data_[index];
  }

  T* begin() const { return data_; }
  T* end() const { return data_ + size_; }

  T& front() { assert(size_ > 0); return *data_; }
  T& back() { assert(size_ > 0); return *(data_ + size_ - 1); }
  const T& front() const { assert(size_ > 0); return *data_; }
  const T& back() const { assert(size_ > 0); return *(data_ + size_ - 1); }

 private:
  T* data_;
  size_t size_;
};

template<typename T>
inline Span<T> MakeSpan(T* data, size_t size) {
  return Span<T>{data, size};
}

} // namespace opcua