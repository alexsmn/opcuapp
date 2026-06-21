#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/struct_writer.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <functional>

namespace opcua {

struct AuthenticationResult {
  opcua::NodeId user_id;
  unsigned user_rights = 0;
  bool multi_sessions = false;
};

using AuthenticationCallback =
    std::function<void(const AuthenticationResult& result)>;

// TODO: Merge into `SessionService`.
using Authenticator = std::function<
    Awaitable<opcua::StatusOr<AuthenticationResult>>(
        opcua::LocalizedText user_name,
        opcua::LocalizedText password)>;

using AsyncAuthenticator = Authenticator;

class CoroutineAuthenticator {
 public:
  virtual ~CoroutineAuthenticator() = default;

  virtual Awaitable<opcua::StatusOr<AuthenticationResult>> Authenticate(
      opcua::LocalizedText user_name,
      opcua::LocalizedText password) = 0;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const AuthenticationResult& result) {
  StructWriter{stream}
      .AddField("user_id", result.user_id)
      .AddField("user_rights", result.user_rights)
      .AddField("multi_sessions", result.multi_sessions);
  return stream;
}

}  // namespace opcua (vendored)
