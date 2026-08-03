#pragma once

namespace opcua {

struct ServiceLogParams {
  bool log_read = false;
  bool log_browse = false;
  bool log_history = false;
  bool log_event = false;
  bool log_model_change_event = false;
  bool log_node_semantics_change_event = false;
};

}  // namespace opcua
