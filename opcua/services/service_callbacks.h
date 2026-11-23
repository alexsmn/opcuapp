#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/services/service_message.h"

#include "opcua/monitored/monitored_item.h"
#include "opcua/services/method_types.h"
#include "opcua/types/co_result.h"
#include "opcua/types/status_or.h"

#include <functional>
#include <memory>

namespace opcua {

// Operation callbacks that connect the OPC UA protocol runtime to an
// application address-space implementation. opcuapp owns the wire/session
// semantics; the embedding application owns these operations.
struct ServiceCallbacks {
  using ReadCallback = std::function<CoStatusOr<std::vector<DataValue>>(
      ServiceContext,
      std::shared_ptr<const std::vector<ReadValueId>>)>;
  using WriteCallback = std::function<CoStatusOr<std::vector<StatusCode>>(
      ServiceContext,
      std::shared_ptr<const std::vector<WriteValue>>)>;
  using BrowseCallback = std::function<CoStatusOr<std::vector<BrowseResult>>(
      ServiceContext,
      std::vector<BrowseDescription>)>;
  // Takes the ServiceContext so the application can refuse to resolve a path
  // through nodes the session may not browse (OPC UA Part 3 §8.55
  // PermissionType bit 0 Browse,
  // https://reference.opcfoundation.org/Core/Part3/v105/docs/8.55).
  using TranslateBrowsePathsCallback = std::function<CoStatusOr<
      std::vector<BrowsePathResult>>(ServiceContext, std::vector<BrowsePath>)>;
  using CallCallback = std::function<CoStatusOr<
      CallResult>(NodeId, NodeId, std::vector<Variant>, ServiceContext)>;
  // Takes the ServiceContext so the application can require the ReadHistory
  // permission (OPC UA Part 3 §8.55 PermissionType bit 7 ReadHistory,
  // https://reference.opcfoundation.org/Core/Part3/v105/docs/8.55).
  using HistoryReadRawCallback =
      std::function<CoStatusOr<HistoryReadRawResult>(ServiceContext,
                                                     HistoryReadRawDetails)>;
  // Takes a ServiceContext for ReadHistory too, and because a historical event
  // carries a Message, which is a localizable LocalizedText (OPC UA Part 5
  // §6.4.2), so answering one needs to know which session asked. Part 11
  // states no exception for historical access, so the session's language
  // governs a stored message exactly as it governs a live one.
  using HistoryReadEventsCallback =
      std::function<CoStatusOr<HistoryReadEventsResult>(ServiceContext,
                                                        NodeId,
                                                        DateTime,
                                                        DateTime,
                                                        EventFilter)>;
  using HistoryUpdateCallback = std::function<Awaitable<
      StatusOr<std::vector<StatusCode>>>(ServiceContext, UpdateDataDetails)>;
  using HistoryUpdateEventsCallback = std::function<Awaitable<
      StatusOr<std::vector<StatusCode>>>(ServiceContext, UpdateEventDetails)>;
  using AddNodesCallback = std::function<CoStatusOr<
      std::vector<AddNodesResult>>(ServiceContext, std::vector<AddNodesItem>)>;
  using DeleteNodesCallback = std::function<CoStatusOr<std::vector<StatusCode>>(
      ServiceContext,
      std::vector<DeleteNodesItem>)>;
  using AddReferencesCallback = std::function<CoStatusOr<
      std::vector<StatusCode>>(ServiceContext, std::vector<AddReferencesItem>)>;
  using DeleteReferencesCallback =
      std::function<CoStatusOr<std::vector<StatusCode>>(
          ServiceContext,
          std::vector<DeleteReferencesItem>)>;
  using CreateSubscriptionCallback =
      std::function<StatusOr<std::unique_ptr<MonitoredItemSubscription>>(
          ServiceContext,
          MonitoredItemSubscriptionOptions)>;

  ReadCallback read;
  WriteCallback write;
  BrowseCallback browse;
  TranslateBrowsePathsCallback translate_browse_paths;
  CallCallback call;
  HistoryReadRawCallback history_read_raw;
  HistoryReadEventsCallback history_read_events;
  HistoryUpdateCallback history_update;
  HistoryUpdateEventsCallback history_update_event;
  AddNodesCallback add_nodes;
  DeleteNodesCallback delete_nodes;
  AddReferencesCallback add_references;
  DeleteReferencesCallback delete_references;
  CreateSubscriptionCallback create_subscription;
};

}  // namespace opcua
