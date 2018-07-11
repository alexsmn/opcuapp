#pragma once

#include <opcua_encodeableobject.h>

namespace opcua {

class EncodableTypeTable {
 public:
  EncodableTypeTable() { Check(::OpcUa_EncodeableTypeTable_Create(&table_)); }

  ~EncodableTypeTable() { ::OpcUa_EncodeableTypeTable_Delete(&table_); }

  EncodableTypeTable(const EncodableTypeTable&) = delete;
  EncodableTypeTable& operator=(const EncodableTypeTable&) = delete;

  OpcUa_EncodeableTypeTable& get() { return table_; }

  void AddTypes(OpcUa_EncodeableType** types) {
    Check(::OpcUa_EncodeableTypeTable_AddTypes(&table_, types));
  }

  void AddKnownTypes() { AddTypes(OpcUa_KnownEncodeableTypes); }

 private:
  OpcUa_EncodeableTypeTable table_;
};

}  // namespace opcua
