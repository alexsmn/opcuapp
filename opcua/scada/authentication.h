#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/struct_writer.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <functional>

namespace opcua {
namespace scada {

struct AuthenticationResult {
  opcua::scada::NodeId user_id;
  unsigned user_rights = 0;
  bool multi_sessions = false;
};

using AuthenticationCallback =
    std::function<void(const AuthenticationResult& result)>;

// TODO: Merge into `SessionService`.
using Authenticator = std::function<
    Awaitable<opcua::scada::StatusOr<AuthenticationResult>>(
        opcua::scada::LocalizedText user_name,
        opcua::scada::LocalizedText password)>;

using AsyncAuthenticator = Authenticator;

class CoroutineAuthenticator {
 public:
  virtual ~CoroutineAuthenticator() = default;

  virtual Awaitable<opcua::scada::StatusOr<AuthenticationResult>> Authenticate(
      opcua::scada::LocalizedText user_name,
      opcua::scada::LocalizedText password) = 0;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const AuthenticationResult& result) {
  StructWriter{stream}
      .AddField("user_id", result.user_id)
      .AddField("user_rights", result.user_rights)
      .AddField("multi_sessions", result.multi_sessions);
  return stream;
}

}  // namespace scada
}  // namespace opcua (vendored)
