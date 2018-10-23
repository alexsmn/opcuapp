#pragma once

#include <opcuapp/basic_types.h>
#include <opcuapp/object_wrapper.h>
#include <opcuapp/string.h>

namespace opcua {

inline void Initialize(OpcUa_StringTable& value) {
  ::OpcUa_StringTable_Initialize(&value);
}

inline void Clear(OpcUa_StringTable& value) {
  ::OpcUa_StringTable_Clear(&value);
}

class StringTable : public detail::ObjectWrapper<OpcUa_StringTable> {
 public:
  void Append(const String& string) { AddStrings(&string.get(), 1); }

  void AddStrings(const OpcUa_String* strings, UInt32 count) {
    ::OpcUa_StringTable_AddStrings(&get(), const_cast<OpcUa_String*>(strings),
                                   count);
  }

  void AddStringList(const OpcUa_StringA* strings) {
    ::OpcUa_StringTable_AddStringList(&get(),
                                      const_cast<OpcUa_StringA*>(strings));
  }

  String FindString(Int32 index) const {
    OpcUa_String string;
    Check(::OpcUa_StringTable_FindString(&const_cast<OpcUa_StringTable&>(get()),
                                         index, &string));
    return string;
  }

  Int32 FindIndex(const OpcUa_String& string) const {
    Int32 index;
    if (OpcUa_IsGood(::OpcUa_StringTable_FindIndex(
            &const_cast<OpcUa_StringTable&>(get()),
            &const_cast<OpcUa_String&>(string), &index)))
      return index;
    else
      return -1;
  }

  UInt32 GetCount() const { return get().Count; }

  const OpcUa_String& operator[](size_t index) const {
    assert(index >= 0 && index < get().Count);
    return get().Values[index];
  }

  const OpcUa_String* begin() const { return get().Values; }
  const OpcUa_String* end() const { return get().Values + get().Count; }
};

}  // namespace opcua
