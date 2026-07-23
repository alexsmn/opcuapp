#include "opcua/base/boost_log.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/server/service_handler.h"
#include "opcua/services/service_context.h"

#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace opcua {
namespace {

// Captures the (message, TraceParent, UserId, Peer) tags of every record the
// handler's logger emits, so the tests assert on the observable log output
// rather than on formatting internals.
class TraceTagCapturingBackend final
    : public boost::log::sinks::basic_sink_backend<
          boost::log::sinks::concurrent_feeding> {
 public:
  struct Record {
    std::string message;
    std::string trace_parent;
    std::string user_id;
    std::string peer;
  };

  void consume(const boost::log::record_view& record) {
    Record captured;
    captured.message = boost::log::extract_or_default<std::string>(
        boost::log::attribute_name{"Message"}, record, std::string{});
    captured.trace_parent = boost::log::extract_or_default<std::string>(
        boost::log::attribute_name{kTraceParentLogAttribute}, record,
        std::string{});
    captured.user_id = boost::log::extract_or_default<std::string>(
        boost::log::attribute_name{"UserId"}, record, std::string{});
    captured.peer = boost::log::extract_or_default<std::string>(
        boost::log::attribute_name{"Peer"}, record, std::string{});
    std::lock_guard lock{mutex_};
    records_.push_back(std::move(captured));
  }

  std::vector<Record> records() {
    std::lock_guard lock{mutex_};
    return records_;
  }

 private:
  std::mutex mutex_;
  std::vector<Record> records_;
};

class ServiceHandlerTraceTest : public testing::Test {
 protected:
  using CapturingSink =
      boost::log::sinks::synchronous_sink<TraceTagCapturingBackend>;

  void SetUp() override {
    sink_ = boost::make_shared<CapturingSink>();
    boost::log::core::get()->add_sink(sink_);
  }

  void TearDown() override { boost::log::core::get()->remove_sink(sink_); }

  std::vector<TraceTagCapturingBackend::Record> Records() {
    sink_->flush();
    return sink_->locked_backend()->records();
  }

  boost::shared_ptr<CapturingSink> sink_;
};

constexpr char kTraceParent[] =
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

TEST_F(ServiceHandlerTraceTest, ReadCompletionLogCarriesTraceParent) {
  ServiceCallbacks callbacks;
  callbacks.read = [](ServiceContext,
                      std::shared_ptr<const std::vector<ReadValueId>> inputs)
      -> Awaitable<StatusOr<std::vector<DataValue>>> {
    co_return std::vector<DataValue>(inputs->size());
  };
  ServiceHandler handler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{}
                             .with_trace_id(kTraceParent)
                             .with_user_id(NodeId{42, 4})
                             .with_peer("203.0.113.7:49152"),
  }};

  TestExecutor executor;
  ReadRequest request{.inputs = {ReadValueId{}}};
  (void)WaitAwaitable(executor, handler.Handle(ServiceRequest{request}));

  const auto records = Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].message, "OPC UA Read completed");
  EXPECT_EQ(records[0].trace_parent, kTraceParent);
  EXPECT_EQ(records[0].user_id, NodeId(42, 4).ToString());
  EXPECT_EQ(records[0].peer, "203.0.113.7:49152");
}

TEST_F(ServiceHandlerTraceTest, WriteCompletionLogCarriesTraceParent) {
  ServiceCallbacks callbacks;
  callbacks.write = [](ServiceContext,
                       std::shared_ptr<const std::vector<WriteValue>> inputs)
      -> Awaitable<StatusOr<std::vector<StatusCode>>> {
    co_return std::vector<StatusCode>(inputs->size(), StatusCode::Good);
  };
  ServiceHandler handler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{}.with_trace_id(kTraceParent),
  }};

  TestExecutor executor;
  ua::WriteRequest request{.nodes_to_write = {ua::WriteValue{}}};
  (void)WaitAwaitable(executor, handler.Handle(ServiceRequest{request}));

  const auto records = Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].message, "OPC UA Write completed");
  EXPECT_EQ(records[0].trace_parent, kTraceParent);
  // Anonymous session: the UserId tag stays empty so sinks drop it.
  EXPECT_EQ(records[0].user_id, "");
}

TEST_F(ServiceHandlerTraceTest, CallCompletionLogCarriesTraceParent) {
  ServiceCallbacks callbacks;
  callbacks.call = [](NodeId, NodeId, std::vector<Variant>,
                      ServiceContext) -> Awaitable<Status> {
    co_return Status{StatusCode::Good};
  };
  ServiceHandler handler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{}.with_trace_id(kTraceParent),
  }};

  TestExecutor executor;
  CallRequest request{.methods = {MethodCallRequest{}}};
  (void)WaitAwaitable(executor, handler.Handle(ServiceRequest{request}));

  const auto records = Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].message, "OPC UA Call completed");
  EXPECT_EQ(records[0].trace_parent, kTraceParent);
}

TEST_F(ServiceHandlerTraceTest, BrowseCompletionLogCarriesTraceParent) {
  ServiceCallbacks callbacks;
  callbacks.browse = [](ServiceContext, std::vector<BrowseDescription> inputs)
      -> Awaitable<StatusOr<std::vector<BrowseResult>>> {
    co_return std::vector<BrowseResult>(inputs.size());
  };
  ServiceHandler handler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{}.with_trace_id(kTraceParent),
  }};

  TestExecutor executor;
  BrowseRequest request{.inputs = {BrowseDescription{}}};
  (void)WaitAwaitable(executor, handler.Handle(ServiceRequest{request}));

  const auto records = Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].message, "OPC UA Browse completed");
  EXPECT_EQ(records[0].trace_parent, kTraceParent);
}

}  // namespace
}  // namespace opcua
