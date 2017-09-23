#pragma once

namespace opcua {

struct Attach {};

template<typename T>
class Vector {
 public:
  Vector() {}

  explicit Vector(size_t size) {
    auto* data = reinterpret_cast<T*>(::OpcUa_Memory_Alloc(sizeof(T) * size));
    if (!data)
      throw OpcUa_BadOutOfMemory;
    std::for_each(data, data + size, [](auto& v) { Initialize(v); });
    data_ = data;
    size_ = size;
  }

  Vector(Attach, T* data, size_t size)
      : data_{data},
        size_{size} {
  }

  Vector(Vector&& source)
      : data_{source.data_},
        size_ {source.size_} {
    source.data_ = OpcUa_Null;
    source.size_ = 0;
  }

  ~Vector() {
    std::for_each(data_, data_ + size_, [](auto& v) { Clear(v); });
    ::OpcUa_Memory_Free(data_);
  }

  Vector(const Vector&) = delete;
  Vector& operator=(const Vector&) = delete;

  Vector& operator=(Vector&& source) {
    if (this != &source) {
      data_ = source.data_;
      size_ = source.size_;
      source.data_ = OpcUa_Null;
      source.size_ = 0;
    }
    return *this;
  }

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }

  T* data() { return data_; }
  const T* data() const { return data_; }

  T& at(size_t index) { assert(index < size_); return data_[index]; }
  const T& at(size_t index) const { assert(index < size_); return data_[index]; }

  T& operator[](size_t index) { return at(index); }
  const T& operator[](size_t index) const { return at(index); }

  T* release() {
    auto* data = data_;
    data_ = nullptr;
    size_ = 0;
    return data;
  }

  T* begin() { return data_; }
  T* end() { return data_ + size_; }
  const T* begin() const { return data_; }
  const T* end() const { return data_ + size_; }

 private:
  T* data_ = OpcUa_Null;
  size_t size_ = 0;
};

} // namespace opcuapp
