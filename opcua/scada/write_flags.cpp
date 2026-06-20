#include "opcua/scada/write_flags.h"

#include "opcua/base/debug_util.h"


namespace opcua::scada {

std::ostream& operator<<(std::ostream& stream, WriteFlags flags) {
  constexpr std::string_view kBitStrings[] = {
      "Select",
      "Parameter",
  };
  return stream << BitMaskToString(flags.raw(), kBitStrings);
}

}  // namespace opcua::scada
