#pragma once

namespace opcua {

OPCUA_DEFINE_METHODS(QualifiedName);

inline void Copy(const OpcUa_QualifiedName& source, OpcUa_QualifiedName& target) {
  target.NamespaceIndex = source.NamespaceIndex;
  Copy(source.Name, target.Name);
}

class QualifiedName {
 public:
  QualifiedName() {
    Initialize(value_);
  }

  QualifiedName(OpcUa_QualifiedName&& source) : value_{source} {
    Initialize(source);
  }

  QualifiedName(QualifiedName&& source) : value_{source.value_} {
    Initialize(source.value_);
  }

  QualifiedName(const QualifiedName& source) = delete;

  ~QualifiedName() {
    Clear(value_);
  }

  QualifiedName& operator=(const QualifiedName& source) {
    if (&source != this)
      Copy(source.value_, value_);
    return *this;
  }

  QualifiedName& operator=(QualifiedName&& source) {
    if (&source != this) {
      Clear(value_);
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  bool empty() const { return ::OpcUa_String_IsEmpty(&value_.Name); }
  const OpcUa_String& name() const { return value_.Name; }
  NamespaceIndex namespace_index() const { return value_.NamespaceIndex; }

  OpcUa_QualifiedName& get() { return value_; }
  const OpcUa_QualifiedName& get() const { return value_; }

 private:
  OpcUa_QualifiedName value_;
};

} // namespace opcua
