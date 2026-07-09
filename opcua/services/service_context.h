#pragma once

#include "opcua/metrics/trace_id.h"
#include "opcua/types/node_id.h"

#include <memory>
#include <ostream>
#include <string>

namespace opcua {

class [[nodiscard]] ServiceContext {
 public:
  ServiceContext() = default;

  ServiceContext(const ServiceContext&) = default;
  ServiceContext& operator=(const ServiceContext&) = default;

  const opcua::NodeId& user_id() const;
  // The caller's access-rights bitmask captured at session activation (zero for
  // an anonymous session). Carried alongside user_id so the server can enforce
  // authorization without a per-request lookup.
  unsigned user_rights() const;
  uint64_t request_id() const;
  const TraceId& trace_id() const;
  // Remote network peer of the caller's connection ("address:port"), captured
  // at session activation and refreshed when the session resumes on another
  // connection. Empty when the transport has no network peer. The OTel
  // `client.address` equivalent for request logs and trace spans.
  const std::string& peer() const;

  ServiceContext with_user_id(const opcua::NodeId& user_id) const;
  ServiceContext with_user_rights(unsigned user_rights) const;
  ServiceContext with_request_id(uint64_t request_id) const;
  ServiceContext with_trace_id(const TraceId& trace_id) const;
  ServiceContext with_peer(std::string peer) const;

  friend std::ostream& operator<<(std::ostream& stream,
                                  const ServiceContext& context);

 private:
  struct Rep;

  explicit ServiceContext(const std::shared_ptr<const Rep>& rep);

  std::shared_ptr<const Rep> rep_ = kDefaultRep;

  static const std::shared_ptr<const Rep> kDefaultRep;
};

}  // namespace opcua
