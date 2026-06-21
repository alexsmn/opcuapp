#pragma once

#include "opcua/scada/basic_types.h"
#include "opcua/scada/string.h"

#include <ostream>
#include <string>

namespace opcua {

// Built-in OPC UA QualifiedName: a name qualified by a NamespaceIndex, used for
// BrowseNames and similar identifiers. OPC UA Part 3 §8.3 QualifiedName,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.3
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


inline const std::string& ToString(const opcua::QualifiedName& name) {
  return name.name();
}

std::u16string ToString16(const opcua::QualifiedName& name);
}  // namespace opcua (vendored)
