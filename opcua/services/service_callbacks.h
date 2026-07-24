#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/services/service_message.h"

#include "opcua/monitored/monitored_item.h"
#include "opcua/types/status_or.h"

#include <functional>
#include <memory>

namespace opcua {

// Operation callbacks that connect the OPC UA protocol runtime to an
// application address-space implementation. opcuapp owns the wire/session
// semantics; the embedding application owns these operations.
struct ServiceCallbacks {
  using ReadCallback =
      std::function<Awaitable<StatusOr<std::vector<DataValue>>>(
          ServiceContext,
          std::shared_ptr<const std::vector<ReadValueId>>)>;
  using WriteCallback =
      std::function<Awaitable<StatusOr<std::vector<StatusCode>>>(
          ServiceContext,
          std::shared_ptr<const std::vector<WriteValue>>)>;
  using BrowseCallback =
      std::function<Awaitable<StatusOr<std::vector<BrowseResult>>>(
          ServiceContext,
          std::vector<BrowseDescription>)>;
  using TranslateBrowsePathsCallback =
      std::function<Awaitable<StatusOr<std::vector<BrowsePathResult>>>(
          std::vector<BrowsePath>)>;
  using CallCallback = std::function<
      Awaitable<Status>(NodeId, NodeId, std::vector<Variant>, ServiceContext)>;
  using HistoryReadRawCallback =
      std::function<Awaitable<StatusOr<HistoryReadRawResult>>(
          HistoryReadRawDetails)>;
  using HistoryReadEventsCallback = std::function<Awaitable<StatusOr<
      HistoryReadEventsResult>>(NodeId, DateTime, DateTime, EventFilter)>;
  using HistoryUpdateCallback =
      std::function<Awaitable<HistoryUpdateResult>(ServiceContext,
                                                   UpdateDataDetails)>;
  using HistoryUpdateEventsCallback =
      std::function<Awaitable<HistoryUpdateResult>(ServiceContext,
                                                   UpdateEventDetails)>;
  using AddNodesCallback =
      std::function<Awaitable<StatusOr<std::vector<AddNodesResult>>>(
          ServiceContext,
          std::vector<AddNodesItem>)>;
  using DeleteNodesCallback =
      std::function<Awaitable<StatusOr<std::vector<StatusCode>>>(
          ServiceContext,
          std::vector<DeleteNodesItem>)>;
  using AddReferencesCallback =
      std::function<Awaitable<StatusOr<std::vector<StatusCode>>>(
          ServiceContext,
          std::vector<AddReferencesItem>)>;
  using DeleteReferencesCallback =
      std::function<Awaitable<StatusOr<std::vector<StatusCode>>>(
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
