#pragma once

namespace opcua {

// opcuapp/SCADA-specific user privilege (the right to configure or to control).
// It is a domain access-right concept and does not map to a standard OPC UA
// PermissionType bit, so no spec reference applies.
enum class Privilege { Configure = 0, Control = 1 };

}  // namespace opcua (vendored)
