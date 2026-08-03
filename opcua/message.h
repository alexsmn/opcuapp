#pragma once

// OPC UA service request/response messages and their parameter types, per
// OPC UA Part 4 (Services). Each type below is annotated with its spec section
// and a link to the online reference (https://reference.opcfoundation.org,
// release v1.05). Request/response structures are cited against the Service
// that defines them (Part 4 §5); reusable parameter structures against the
// Common Parameter Type Definitions (Part 4 §7).

#include "opcua/services/service_message.h"
#include "opcua/session/server_session_manager.h"
#include "opcua/types/endpoint_description.h"
#include "opcua/ua/ua_types.h"

#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace opcua {

// Error response returned in place of the normal response when a request fails
// at the Service level. OPC UA Part 4 §7.34 ServiceFault,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.34
struct ServiceFault {
  Status status{StatusCode::Bad};
};

// Server-assigned identifier of a Subscription (an IntegerId). OPC UA Part 4
// §7.19 IntegerId,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.19
using SubscriptionId = UInt32;
// Server-assigned identifier of a MonitoredItem (an IntegerId). OPC UA Part 4
// §7.19 IntegerId,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.19
using MonitoredItemId = UInt32;

// Whether sampling and/or reporting are enabled for a MonitoredItem. OPC UA
// Part 4 §7.23 MonitoringMode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.23
enum class MonitoringMode : UInt32 {
  Disabled = 0,
  Sampling = 1,
  Reporting = 2,
};

// Which timestamps (source and/or server) the Server returns with values. OPC
// UA Part 4 §7.39 TimestampsToReturn,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.39
enum class TimestampsToReturn : UInt32 {
  Source = 0,
  Server = 1,
  Both = 2,
  Neither = 3,
};

// FindServers returns the Servers known to a Server or Discovery Server. OPC UA
// Part 4 §5.5.2 FindServers,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.5.2
struct FindServersRequest {
  std::string endpoint_url;
  std::vector<std::string> locale_ids;
  std::vector<std::string> server_uris;
};

struct FindServersResponse {
  Status status{StatusCode::Good};
  std::vector<ApplicationDescription> servers;
};

// GetEndpoints returns the Endpoints supported by a Server. OPC UA Part 4
// §5.5.4 GetEndpoints,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.5.4
struct GetEndpointsRequest {
  std::string endpoint_url;
  std::vector<std::string> locale_ids;
  std::vector<std::string> profile_uris;
};

struct GetEndpointsResponse {
  Status status{StatusCode::Good};
  std::vector<EndpointDescription> endpoints;
};

// RegisterServer lets a Server register itself with a Discovery Server (here
// the aggregating proxy) so it can be aggregated dynamically, without static
// config. OPC UA Part 4 §5.4.5 RegisterServer,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.4.5
struct RegisteredServer {
  std::string server_uri;
  std::string product_uri;
  std::vector<LocalizedText> server_names;
  ApplicationType server_type = ApplicationType::Server;
  std::string gateway_server_uri;
  std::vector<std::string> discovery_urls;
  std::string semaphore_file_path;
  bool is_online = true;
};

struct RegisterServerRequest {
  RegisteredServer server;
};

struct RegisterServerResponse {
  Status status{StatusCode::Good};
};

// The mDNS flavor of RegisterServer2's DiscoveryConfiguration — the only
// concrete subtype the spec defines. Carried here for its serverCapabilities
// (ServerCapabilityIdentifiers, e.g. "HD" = Historical Data — OPC UA Part 12
// Annex D), which let a discovery target treat a registrant by role (a
// historian is linked, not aggregated). OPC UA Part 4 §7.8,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.8
struct MdnsDiscoveryConfiguration {
  std::string mdns_server_name;
  std::vector<std::string> server_capabilities;
};

// RegisterServer2 extends RegisterServer with per-registration discovery
// configuration (the capabilities above). OPC UA Part 4 §5.4.6
// RegisterServer2,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.4.6
struct RegisterServer2Request {
  RegisteredServer server;
  // One entry per discoveryConfiguration element, in request order. nullopt
  // marks an ExtensionObject type this stack does not implement (decode side;
  // the handler reports Bad_NotSupported for it in configurationResults).
  // Encoding skips nullopt entries.
  std::vector<std::optional<MdnsDiscoveryConfiguration>>
      discovery_configuration;
};

struct RegisterServer2Response {
  Status status{StatusCode::Good};
  // Per-discoveryConfiguration-entry results (Part 4 §5.4.6.2
  // configurationResults).
  std::vector<StatusCode> configuration_results;
};

// How a DataChangeFilter deadband value is interpreted (none, absolute, or
// percent of range). OPC UA Part 4 §7.22.2 DataChangeFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.2
enum class DeadbandType : UInt32 {
  None = 0,
  Absolute = 1,
  Percent = 2,
};

// Which kind of change triggers a data-change notification. OPC UA Part 4
// §7.10 DataChangeTrigger,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.10
enum class DataChangeTrigger : UInt32 {
  Status = 0,
  StatusValue = 1,
  StatusValueTimestamp = 2,
};

// Filters data-change notifications by trigger condition and deadband. OPC UA
// Part 4 §7.22.2 DataChangeFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.2
struct DataChangeFilter {
  bool operator==(const DataChangeFilter&) const = default;

  DataChangeTrigger trigger = DataChangeTrigger::StatusValue;
  DeadbandType deadband_type = DeadbandType::None;
  double deadband_value = 0;
};

// The (extensible) filter applied to a MonitoredItem. A DataChangeFilter is
// modelled directly; event/aggregate filters are carried as encoded JSON. OPC
// UA Part 4 §7.22 MonitoringFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22
using MonitoringFilter = std::variant<DataChangeFilter, boost::json::value>;

// The monitoring characteristics of a MonitoredItem: client handle, sampling
// interval, filter, queue size and discard policy. OPC UA Part 4 §7.21
// MonitoringParameters,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.21
struct MonitoringParameters {
  bool operator==(const MonitoringParameters&) const = default;

  UInt32 client_handle = 0;
  double sampling_interval_ms = 0;
  std::optional<MonitoringFilter> filter;
  UInt32 queue_size = 1;
  bool discard_oldest = true;
};

// One MonitoredItem to create, as passed to CreateMonitoredItems. OPC UA Part 4
// §5.13.2 CreateMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
struct MonitoredItemCreateRequest {
  bool operator==(const MonitoredItemCreateRequest&) const = default;

  ReadValueId item_to_monitor;
  std::optional<std::string> index_range;
  MonitoringMode monitoring_mode = MonitoringMode::Reporting;
  MonitoringParameters requested_parameters;
};

// Result for one created MonitoredItem (assigned id and revised parameters).
// OPC UA Part 4 §5.13.2 CreateMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
struct MonitoredItemCreateResult {
  bool operator==(const MonitoredItemCreateResult&) const = default;

  Status status{StatusCode::Good};
  MonitoredItemId monitored_item_id = 0;
  double revised_sampling_interval_ms = 0;
  UInt32 revised_queue_size = 0;
  std::optional<boost::json::value> filter_result;
};

// One MonitoredItem to modify, as passed to ModifyMonitoredItems. OPC UA Part 4
// §5.13.3 ModifyMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.3
struct MonitoredItemModifyRequest {
  bool operator==(const MonitoredItemModifyRequest&) const = default;

  MonitoredItemId monitored_item_id = 0;
  MonitoringParameters requested_parameters;
};

// Result for one modified MonitoredItem. OPC UA Part 4 §5.13.3
// ModifyMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.3
struct MonitoredItemModifyResult {
  bool operator==(const MonitoredItemModifyResult&) const = default;

  Status status{StatusCode::Good};
  double revised_sampling_interval_ms = 0;
  UInt32 revised_queue_size = 0;
  std::optional<boost::json::value> filter_result;
};

// Publishing and lifetime parameters of a Subscription. OPC UA Part 4 §5.14.2
// CreateSubscription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.2
struct SubscriptionParameters {
  bool operator==(const SubscriptionParameters&) const = default;

  double publishing_interval_ms = 0;
  UInt32 lifetime_count = 0;
  UInt32 max_keep_alive_count = 0;
  UInt32 max_notifications_per_publish = 0;
  bool publishing_enabled = true;
  UInt8 priority = 0;
};

// CreateSubscription creates a Subscription. OPC UA Part 4 §5.14.2
// CreateSubscription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.2
struct CreateSubscriptionRequest {
  SubscriptionParameters parameters;
};

struct CreateSubscriptionResponse {
  Status status{StatusCode::Good};
  SubscriptionId subscription_id = 0;
  double revised_publishing_interval_ms = 0;
  UInt32 revised_lifetime_count = 0;
  UInt32 revised_max_keep_alive_count = 0;
};

// ModifySubscription changes the parameters of an existing Subscription. OPC UA
// Part 4 §5.14.3 ModifySubscription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.3
struct ModifySubscriptionRequest {
  SubscriptionId subscription_id = 0;
  SubscriptionParameters parameters;
};

struct ModifySubscriptionResponse {
  Status status{StatusCode::Good};
  double revised_publishing_interval_ms = 0;
  UInt32 revised_lifetime_count = 0;
  UInt32 revised_max_keep_alive_count = 0;
};

// SetPublishingMode enables or disables publishing for Subscriptions. OPC UA
// Part 4 §5.14.4 SetPublishingMode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.4. Migrated to
// the schema-generated ua:: types; the variants below hold them directly.

// DeleteSubscriptions deletes one or more Subscriptions. OPC UA Part 4 §5.14.8
// DeleteSubscriptions,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.8. The
// request/response types are the schema-generated ua:: structs (the first
// service migrated off the hand-written definitions); the variants below hold
// them directly.

// CreateMonitoredItems creates and adds MonitoredItems to a Subscription. OPC
// UA Part 4 §5.13.2 CreateMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
struct CreateMonitoredItemsRequest {
  SubscriptionId subscription_id = 0;
  TimestampsToReturn timestamps_to_return = TimestampsToReturn::Both;
  std::vector<MonitoredItemCreateRequest> items_to_create;
};

struct CreateMonitoredItemsResponse {
  Status status{StatusCode::Good};
  std::vector<MonitoredItemCreateResult> results;
};

// ModifyMonitoredItems changes the parameters of existing MonitoredItems. OPC
// UA Part 4 §5.13.3 ModifyMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.3
struct ModifyMonitoredItemsRequest {
  SubscriptionId subscription_id = 0;
  TimestampsToReturn timestamps_to_return = TimestampsToReturn::Both;
  std::vector<MonitoredItemModifyRequest> items_to_modify;
};

struct ModifyMonitoredItemsResponse {
  Status status{StatusCode::Good};
  std::vector<MonitoredItemModifyResult> results;
};

// DeleteMonitoredItems removes MonitoredItems from a Subscription. OPC UA
// Part 4 §5.13.6 DeleteMonitoredItems,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.6. Migrated to
// the schema-generated ua:: types; the variants below hold them directly.

// SetMonitoringMode sets the monitoring mode for MonitoredItems. OPC UA Part 4
// §5.13.4 SetMonitoringMode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.4. Migrated to
// the schema-generated ua:: types; the variants below hold them directly.

// Acknowledges receipt of a NotificationMessage so the Server may discard it.
// OPC UA Part 4 §5.14.5 Publish,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5
struct SubscriptionAcknowledgement {
  bool operator==(const SubscriptionAcknowledgement&) const = default;

  SubscriptionId subscription_id = 0;
  UInt32 sequence_number = 0;
};

// A data-change notification for a single MonitoredItem (its client handle and
// the new Value). OPC UA Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
struct MonitoredItemNotification {
  bool operator==(const MonitoredItemNotification&) const = default;

  UInt32 client_handle = 0;
  DataValue value;
};

// The selected event fields for a single reported event (per the EventFilter
// select clauses). OPC UA Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
struct EventFieldList {
  bool operator==(const EventFieldList&) const = default;

  UInt32 client_handle = 0;
  std::vector<Variant> event_fields;
};

// NotificationData carrying data-change notifications. OPC UA Part 4 §7.25
// NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
struct DataChangeNotification {
  bool operator==(const DataChangeNotification&) const = default;

  std::vector<MonitoredItemNotification> monitored_items;
};

// NotificationData carrying event notifications. OPC UA Part 4 §7.25
// NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
struct EventNotificationList {
  bool operator==(const EventNotificationList&) const = default;

  std::vector<EventFieldList> events;
};

// NotificationData reporting a change in the status of the Subscription. OPC UA
// Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
struct StatusChangeNotification {
  bool operator==(const StatusChangeNotification&) const = default;

  StatusCode status = StatusCode::Good;
};

// The notification payload union carried inside a NotificationMessage. OPC UA
// Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
using NotificationData = std::variant<DataChangeNotification,
                                      EventNotificationList,
                                      StatusChangeNotification>;

// A message published to a Subscription's client, carrying a sequence number,
// publish time and notification data. OPC UA Part 4 §7.26 NotificationMessage,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.26
struct NotificationMessage {
  bool operator==(const NotificationMessage&) const = default;

  UInt32 sequence_number = 0;
  DateTime publish_time;
  std::vector<NotificationData> notification_data;
};

// Publish acknowledges NotificationMessages and requests the next one (or a
// keep-alive). OPC UA Part 4 §5.14.5 Publish,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5
struct PublishRequest {
  std::vector<SubscriptionAcknowledgement> subscription_acknowledgements;
};

struct PublishResponse {
  Status status{StatusCode::Good};
  SubscriptionId subscription_id = 0;
  std::vector<StatusCode> results;
  bool more_notifications = false;
  NotificationMessage notification_message;
  std::vector<SubscriptionId> available_sequence_numbers;
};

// Republish requests retransmission of a previously sent NotificationMessage.
// OPC UA Part 4 §5.14.6 Republish,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.6
struct RepublishRequest {
  SubscriptionId subscription_id = 0;
  UInt32 retransmit_sequence_number = 0;
};

struct RepublishResponse {
  Status status{StatusCode::Good};
  NotificationMessage notification_message;
};

// TransferSubscriptions transfers Subscriptions and their MonitoredItems to a
// different Session. OPC UA Part 4 §5.14.7 TransferSubscriptions,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.7. Migrated to
// the schema-generated ua:: types; the variants below hold them directly. The
// generated response carries ua::TransferResult (StatusCode +
// availableSequenceNumbers) per subscription, unlike the previous bare
// StatusCode list — the earlier wire form was non-conformant.

// RegisterNodes / UnregisterNodes use the generated, spec-conformant ua:: wire
// types directly (OPC UA Part 4 §5.9.5 / §5.9.6). Registration is an optional
// optimization; this server keeps no registered-node handles, so the runtime
// echoes the requested NodeIds (RegisterNodes) or treats it as a no-op
// (UnregisterNodes) — see session/server_runtime.cpp.

// Discriminated union of every Service request body this stack dispatches. The
// envelope follows the OPC UA Message structure. OPC UA Part 6 §6.2 Message
// structure, https://reference.opcfoundation.org/Core/Part6/v105/docs/6.2
using RequestBody = std::variant<FindServersRequest,
                                 GetEndpointsRequest,
                                 RegisterServerRequest,
                                 RegisterServer2Request,
                                 CreateSessionRequest,
                                 ActivateSessionRequest,
                                 CloseSessionRequest,
                                 CreateSubscriptionRequest,
                                 ModifySubscriptionRequest,
                                 ua::SetPublishingModeRequest,
                                 ua::DeleteSubscriptionsRequest,
                                 PublishRequest,
                                 RepublishRequest,
                                 ua::TransferSubscriptionsRequest,
                                 CreateMonitoredItemsRequest,
                                 ModifyMonitoredItemsRequest,
                                 ua::DeleteMonitoredItemsRequest,
                                 ua::SetMonitoringModeRequest,
                                 ua::ReadRequest,
                                 ua::WriteRequest,
                                 ua::BrowseRequest,
                                 ua::BrowseNextRequest,
                                 ua::TranslateBrowsePathsToNodeIdsRequest,
                                 ua::CallRequest,
                                 ua::HistoryReadRequest,
                                 ua::HistoryUpdateRequest,
                                 ua::AddNodesRequest,
                                 ua::DeleteNodesRequest,
                                 ua::AddReferencesRequest,
                                 ua::DeleteReferencesRequest,
                                 ua::RegisterNodesRequest,
                                 ua::UnregisterNodesRequest>;

// Discriminated union of every Service response body this stack produces
// (including ServiceFault). OPC UA Part 6 §6.2 Message structure,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.2
using ResponseBody = std::variant<FindServersResponse,
                                  GetEndpointsResponse,
                                  RegisterServerResponse,
                                  RegisterServer2Response,
                                  CreateSessionResponse,
                                  ActivateSessionResponse,
                                  CloseSessionResponse,
                                  CreateSubscriptionResponse,
                                  ModifySubscriptionResponse,
                                  ua::SetPublishingModeResponse,
                                  ua::DeleteSubscriptionsResponse,
                                  PublishResponse,
                                  RepublishResponse,
                                  ua::TransferSubscriptionsResponse,
                                  CreateMonitoredItemsResponse,
                                  ModifyMonitoredItemsResponse,
                                  ua::DeleteMonitoredItemsResponse,
                                  ua::SetMonitoringModeResponse,
                                  ServiceFault,
                                  ua::ReadResponse,
                                  ua::WriteResponse,
                                  ua::BrowseResponse,
                                  ua::BrowseNextResponse,
                                  ua::TranslateBrowsePathsToNodeIdsResponse,
                                  ua::CallResponse,
                                  ua::HistoryReadResponse,
                                  ua::HistoryUpdateResponse,
                                  ua::AddNodesResponse,
                                  ua::DeleteNodesResponse,
                                  ua::AddReferencesResponse,
                                  ua::DeleteReferencesResponse,
                                  ua::RegisterNodesResponse,
                                  ua::UnregisterNodesResponse>;

// A dispatched request: the client request handle plus the request body. OPC UA
// Part 6 §6.2 Message structure,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.2
struct RequestMessage {
  UInt32 request_handle = 0;
  RequestBody body;
  // Optional W3C traceparent carried in RequestHeader.additionalHeader; empty
  // = absent. See ServiceRequestHeader::trace_parent.
  std::string trace_parent;
};

// A dispatched response: the originating request handle plus the response body.
// OPC UA Part 6 §6.2 Message structure,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.2
struct ResponseMessage {
  UInt32 request_handle = 0;
  ResponseBody body;
};

}  // namespace opcua
