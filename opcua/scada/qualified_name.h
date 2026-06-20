#pragma once

#include "opcua/scada/basic_types.h"
#include "opcua/scada/string.h"

#include <ostream>
#include <string>

namespace opcua::scada {

class QualifiedName {
 public:
  QualifiedName() {}

  QualifiedName(String name, NamespaceIndex namespace_index = 0)
      : name_{std::move(name)}, namespace_index_{namespace_index} {}

  QualifiedName(const char* name, NamespaceIndex namespace_index = 0)
      : name_{name}, namespace_index_{namespace_index} {}

  NamespaceIndex namespace_index() const { return namespace_index_; }
  const String& name() const { return name_; }
  bool empty() const { return namespace_index_ == 0 && name_.empty(); }

 private:
  String name_;
  NamespaceIndex namespace_index_ = 0;
};

inline bool operator==(const QualifiedName& a, const QualifiedName& b) {
  return a.namespace_index() == b.namespace_index() && a.name() == b.name();
}

inline bool operator!=(const QualifiedName& a, const QualifiedName& b) {
  return !operator==(a, b);
}

inline std::ostream& operator<<(std::ostream& stream,
                                const QualifiedName& value) {
  return stream << '"' << value.name() << '"';
}

}  // namespace opcua::scada

inline const std::string& ToString(const opcua::scada::QualifiedName& name) {
  return name.name();
}

std::u16string ToString16(const opcua::scada::QualifiedName& name);
