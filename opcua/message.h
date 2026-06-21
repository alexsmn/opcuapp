#pragma once

#include "opcua/server_session_manager.h"
#include "opcua/service_message.h"

#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace opcua {

struct ServiceFault {
  scada::Status status{scada::StatusCode::Bad};
};

using SubscriptionId = scada::UInt32;
using MonitoredItemId = scada::UInt32;

enum class MonitoringMode : scada::UInt32 {
  Disabled = 0,
  Sampling = 1,
  Reporting = 2,
};

enum class TimestampsToReturn : scada::UInt32 {
  Source = 0,
  Server = 1,
  Both = 2,
  Neither = 3,
};

enum class ApplicationType : scada::UInt32 {
  Server = 0,
  Client = 1,
  ClientAndServer = 2,
  DiscoveryServer = 3,
};

enum class UserTokenType : scada::UInt32 {
  Anonymous = 0,
  UserName = 1,
  Certificate = 2,
  IssuedToken = 3,
};

enum class MessageSecurityMode : scada::UInt32 {
  Invalid = 0,
  None = 1,
  Sign = 2,
  SignAndEncrypt = 3,
};

struct ApplicationDescription {
  std::string application_uri;
  std::string product_uri;
  scada::LocalizedText application_name;
  ApplicationType application_type = ApplicationType::Server;
  std::string gateway_server_uri;
  std::string discovery_profile_uri;
  std::vector<std::string> discovery_urls;
};

struct UserTokenPolicy {
  std::string policy_id;
  UserTokenType token_type = UserTokenType::Anonymous;
  std::string issued_token_type;
  std::string issuer_endpoint_url;
  std::string security_policy_uri;
};

struct EndpointDescription {
  std::string endpoint_url;
  ApplicationDescription server;
  scada::ByteString server_certificate;
  MessageSecurityMode security_mode = MessageSecurityMode::None;
  std::string security_policy_uri;
  std::vector<UserTokenPolicy> user_identity_tokens;
  std::string transport_profile_uri;
  scada::UInt8 security_level = 0;
};

struct FindServersRequest {
  std::string endpoint_url;
  std::vector<std::string> locale_ids;
  std::vector<std::string> server_uris;
};

struct FindServersResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<ApplicationDescription> servers;
};

struct GetEndpointsRequest {
  std::string endpoint_url;
  std::vector<std::string> locale_ids;
  std::vector<std::string> profile_uris;
};

struct GetEndpointsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<EndpointDescription> endpoints;
};

enum class DeadbandType : scada::UInt32 {
  None = 0,
  Absolute = 1,
  Percent = 2,
};

enum class DataChangeTrigger : scada::UInt32 {
  Status = 0,
  StatusValue = 1,
  StatusValueTimestamp = 2,
};

struct DataChangeFilter {
  bool operator==(const DataChangeFilter&) const = default;

  DataChangeTrigger trigger = DataChangeTrigger::StatusValue;
  DeadbandType deadband_type = DeadbandType::None;
  double deadband_value = 0;
};

using MonitoringFilter = std::variant<DataChangeFilter, boost::json::value>;

struct MonitoringParameters {
  bool operator==(const MonitoringParameters&) const = default;

  scada::UInt32 client_handle = 0;
  double sampling_interval_ms = 0;
  std::optional<MonitoringFilter> filter;
  scada::UInt32 queue_size = 1;
  bool discard_oldest = true;
};

struct MonitoredItemCreateRequest {
  bool operator==(const MonitoredItemCreateRequest&) const = default;

  scada::ReadValueId item_to_monitor;
  std::optional<std::string> index_range;
  MonitoringMode monitoring_mode = MonitoringMode::Reporting;
  MonitoringParameters requested_parameters;
};

struct MonitoredItemCreateResult {
  bool operator==(const MonitoredItemCreateResult&) const = default;

  scada::Status status{scada::StatusCode::Good};
  MonitoredItemId monitored_item_id = 0;
  double revised_sampling_interval_ms = 0;
  scada::UInt32 revised_queue_size = 0;
  std::optional<boost::json::value> filter_result;
};

struct MonitoredItemModifyRequest {
  bool operator==(const MonitoredItemModifyRequest&) const = default;

  MonitoredItemId monitored_item_id = 0;
  MonitoringParameters requested_parameters;
};

struct MonitoredItemModifyResult {
  bool operator==(const MonitoredItemModifyResult&) const = default;

  scada::Status status{scada::StatusCode::Good};
  double revised_sampling_interval_ms = 0;
  scada::UInt32 revised_queue_size = 0;
  std::optional<boost::json::value> filter_result;
};

struct SubscriptionParameters {
  bool operator==(const SubscriptionParameters&) const = default;

  double publishing_interval_ms = 0;
  scada::UInt32 lifetime_count = 0;
  scada::UInt32 max_keep_alive_count = 0;
  scada::UInt32 max_notifications_per_publish = 0;
  bool publishing_enabled = true;
  scada::UInt8 priority = 0;
};

struct CreateSubscriptionRequest {
  SubscriptionParameters parameters;
};

struct CreateSubscriptionResponse {
  scada::Status status{scada::StatusCode::Good};
  SubscriptionId subscription_id = 0;
  double revised_publishing_interval_ms = 0;
  scada::UInt32 revised_lifetime_count = 0;
  scada::UInt32 revised_max_keep_alive_count = 0;
};

struct ModifySubscriptionRequest {
  SubscriptionId subscription_id = 0;
  SubscriptionParameters parameters;
};

struct ModifySubscriptionResponse {
  scada::Status status{scada::StatusCode::Good};
  double revised_publishing_interval_ms = 0;
  scada::UInt32 revised_lifetime_count = 0;
  scada::UInt32 revised_max_keep_alive_count = 0;
};

struct SetPublishingModeRequest {
  bool publishing_enabled = true;
  std::vector<SubscriptionId> subscription_ids;
};

struct SetPublishingModeResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct DeleteSubscriptionsRequest {
  std::vector<SubscriptionId> subscription_ids;
};

struct DeleteSubscriptionsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct CreateMonitoredItemsRequest {
  SubscriptionId subscription_id = 0;
  TimestampsToReturn timestamps_to_return = TimestampsToReturn::Both;
  std::vector<MonitoredItemCreateRequest> items_to_create;
};

struct CreateMonitoredItemsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<MonitoredItemCreateResult> results;
};

struct ModifyMonitoredItemsRequest {
  SubscriptionId subscription_id = 0;
  TimestampsToReturn timestamps_to_return = TimestampsToReturn::Both;
  std::vector<MonitoredItemModifyRequest> items_to_modify;
};

struct ModifyMonitoredItemsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<MonitoredItemModifyResult> results;
};

struct DeleteMonitoredItemsRequest {
  SubscriptionId subscription_id = 0;
  std::vector<MonitoredItemId> monitored_item_ids;
};

struct DeleteMonitoredItemsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct SetMonitoringModeRequest {
  SubscriptionId subscription_id = 0;
  MonitoringMode monitoring_mode = MonitoringMode::Reporting;
  std::vector<MonitoredItemId> monitored_item_ids;
};

struct SetMonitoringModeResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct SubscriptionAcknowledgement {
  bool operator==(const SubscriptionAcknowledgement&) const = default;

  SubscriptionId subscription_id = 0;
  scada::UInt32 sequence_number = 0;
};

struct MonitoredItemNotification {
  bool operator==(const MonitoredItemNotification&) const = default;

  scada::UInt32 client_handle = 0;
  scada::DataValue value;
};

struct EventFieldList {
  bool operator==(const EventFieldList&) const = default;

  scada::UInt32 client_handle = 0;
  std::vector<scada::Variant> event_fields;
};

struct DataChangeNotification {
  bool operator==(const DataChangeNotification&) const = default;

  std::vector<MonitoredItemNotification> monitored_items;
};

struct EventNotificationList {
  bool operator==(const EventNotificationList&) const = default;

  std::vector<EventFieldList> events;
};

struct StatusChangeNotification {
  bool operator==(const StatusChangeNotification&) const = default;

  scada::StatusCode status = scada::StatusCode::Good;
};

using NotificationData = std::variant<DataChangeNotification,
                                      EventNotificationList,
                                      StatusChangeNotification>;

struct NotificationMessage {
  bool operator==(const NotificationMessage&) const = default;

  scada::UInt32 sequence_number = 0;
  base::Time publish_time;
  std::vector<NotificationData> notification_data;
};

struct PublishRequest {
  std::vector<SubscriptionAcknowledgement> subscription_acknowledgements;
};

struct PublishResponse {
  scada::Status status{scada::StatusCode::Good};
  SubscriptionId subscription_id = 0;
  std::vector<scada::StatusCode> results;
  bool more_notifications = false;
  NotificationMessage notification_message;
  std::vector<SubscriptionId> available_sequence_numbers;
};

struct RepublishRequest {
  SubscriptionId subscription_id = 0;
  scada::UInt32 retransmit_sequence_number = 0;
};

struct RepublishResponse {
  scada::Status status{scada::StatusCode::Good};
  NotificationMessage notification_message;
};

struct TransferSubscriptionsRequest {
  std::vector<SubscriptionId> subscription_ids;
  bool send_initial_values = false;
};

struct TransferSubscriptionsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

// OPC UA Part 4 §5.3.2 RegisterNodes / §5.3.3 UnregisterNodes. Registration is
// an optional optimization hint; this server keeps no registered-node handles,
// so RegisterNodes echoes the requested NodeIds and UnregisterNodes is a no-op.
struct RegisterNodesRequest {
  std::vector<scada::NodeId> nodes_to_register;
};

struct RegisterNodesResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::NodeId> registered_node_ids;
};

struct UnregisterNodesRequest {
  std::vector<scada::NodeId> nodes_to_unregister;
};

struct UnregisterNodesResponse {
  scada::Status status{scada::StatusCode::Good};
};

using RequestBody = std::variant<FindServersRequest,
                                 GetEndpointsRequest,
                                 CreateSessionRequest,
                                 ActivateSessionRequest,
                                 CloseSessionRequest,
                                 CreateSubscriptionRequest,
                                 ModifySubscriptionRequest,
                                 SetPublishingModeRequest,
                                 DeleteSubscriptionsRequest,
                                 PublishRequest,
                                 RepublishRequest,
                                 TransferSubscriptionsRequest,
                                 CreateMonitoredItemsRequest,
                                 ModifyMonitoredItemsRequest,
                                 DeleteMonitoredItemsRequest,
                                 SetMonitoringModeRequest,
                                 ReadRequest,
                                 WriteRequest,
                                 BrowseRequest,
                                 BrowseNextRequest,
                                 TranslateBrowsePathsRequest,
                                 CallRequest,
                                 HistoryReadRawRequest,
                                 HistoryReadEventsRequest,
                                 AddNodesRequest,
                                 DeleteNodesRequest,
                                 AddReferencesRequest,
                                 DeleteReferencesRequest,
                                 RegisterNodesRequest,
                                 UnregisterNodesRequest>;

using ResponseBody = std::variant<FindServersResponse,
                                  GetEndpointsResponse,
                                  CreateSessionResponse,
                                  ActivateSessionResponse,
                                  CloseSessionResponse,
                                  CreateSubscriptionResponse,
                                  ModifySubscriptionResponse,
                                  SetPublishingModeResponse,
                                  DeleteSubscriptionsResponse,
                                  PublishResponse,
                                  RepublishResponse,
                                  TransferSubscriptionsResponse,
                                  CreateMonitoredItemsResponse,
                                  ModifyMonitoredItemsResponse,
                                  DeleteMonitoredItemsResponse,
                                  SetMonitoringModeResponse,
                                  ServiceFault,
                                  ReadResponse,
                                  WriteResponse,
                                  BrowseResponse,
                                  BrowseNextResponse,
                                  TranslateBrowsePathsResponse,
                                  CallResponse,
                                  HistoryReadRawResponse,
                                  HistoryReadEventsResponse,
                                  AddNodesResponse,
                                  DeleteNodesResponse,
                                  AddReferencesResponse,
                                  DeleteReferencesResponse,
                                  RegisterNodesResponse,
                                  UnregisterNodesResponse>;

struct RequestMessage {
  scada::UInt32 request_handle = 0;
  RequestBody body;
};

struct ResponseMessage {
  scada::UInt32 request_handle = 0;
  ResponseBody body;
};

}  // namespace opcua
