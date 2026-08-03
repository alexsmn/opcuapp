#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/server/service_handler.h"
#include "opcua/services/service_context.h"
#include "opcua/types/co_result.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace opcua {
namespace {

// The Call service's result plumbing. HandleCall used to hard-code every
// CallMethodResult to `{.status_code = status}`, so a method could never return
// anything to its caller; these tests pin the shape now that it can.

ServiceHandler MakeHandler(ServiceCallbacks callbacks) {
  return ServiceHandler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{},
  }};
}

ua::CallRequest OneMethod() {
  return ua::CallRequest{
      .methods_to_call = {ua::CallMethodRequest{.object_id = NodeId{1, 2},
                                                .method_id = NodeId{3, 2}}}};
}

TEST(ServiceHandlerCallTest, OutputArgumentsReachTheResponse) {
  ServiceCallbacks callbacks;
  callbacks.call = [](NodeId, NodeId, std::vector<Variant>,
                      ServiceContext) -> CoStatusOr<CallResult> {
    co_return CallResult{{Variant{Int32{7}}, Variant{String{"ok"}}}};
  };

  TestExecutor executor;
  auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(OneMethod()));

  const auto* call_response = std::get_if<ua::CallResponse>(&response);
  ASSERT_NE(call_response, nullptr);
  ASSERT_EQ(call_response->results.size(), 1u);
  EXPECT_TRUE(call_response->results[0].status_code.good());
  ASSERT_EQ(call_response->results[0].output_arguments.size(), 2u);
  EXPECT_EQ(call_response->results[0].output_arguments[0].get<Int32>(), 7);
  EXPECT_EQ(call_response->results[0].output_arguments[1].get<String>(), "ok");
}

// A method that simply succeeds is the common case and must not be forced to
// invent an output.
TEST(ServiceHandlerCallTest, ASuccessfulCallMayReturnNoOutputs) {
  ServiceCallbacks callbacks;
  callbacks.call = [](NodeId, NodeId, std::vector<Variant>,
                      ServiceContext) -> CoStatusOr<CallResult> {
    co_return CallResult{};
  };

  TestExecutor executor;
  auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(OneMethod()));

  const auto* call_response = std::get_if<ua::CallResponse>(&response);
  ASSERT_NE(call_response, nullptr);
  ASSERT_EQ(call_response->results.size(), 1u);
  EXPECT_TRUE(call_response->results[0].status_code.good());
  EXPECT_TRUE(call_response->results[0].output_arguments.empty());
}

// The failure path carries the operation status and nothing else — there is no
// value to draw outputs from, which is also why CallResult has no status field.
TEST(ServiceHandlerCallTest, AFailedCallReportsItsStatusWithoutOutputs) {
  ServiceCallbacks callbacks;
  callbacks.call = [](NodeId, NodeId, std::vector<Variant>,
                      ServiceContext) -> CoStatusOr<CallResult> {
    co_return Status{StatusCode::Bad_MethodInvalid};
  };

  TestExecutor executor;
  auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(OneMethod()));

  const auto* call_response = std::get_if<ua::CallResponse>(&response);
  ASSERT_NE(call_response, nullptr);
  ASSERT_EQ(call_response->results.size(), 1u);
  EXPECT_EQ(call_response->results[0].status_code.code(),
            StatusCode::Bad_MethodInvalid);
  EXPECT_TRUE(call_response->results[0].output_arguments.empty());
}

// Results are positional: the response's Nth result belongs to the request's
// Nth method, so a mid-batch failure must not shift the ones after it.
TEST(ServiceHandlerCallTest, BatchedResultsStayAlignedAcrossAFailure) {
  ServiceCallbacks callbacks;
  callbacks.call = [](NodeId, NodeId method_id, std::vector<Variant>,
                      ServiceContext) -> CoStatusOr<CallResult> {
    if (method_id == NodeId{2, 2}) {
      co_return Status{StatusCode::Bad_MethodInvalid};
    }
    co_return CallResult{{Variant{method_id}}};
  };

  ua::CallRequest request;
  for (NumericId id : {1, 2, 3}) {
    request.methods_to_call.push_back(ua::CallMethodRequest{
        .object_id = NodeId{9, 2}, .method_id = NodeId{id, 2}});
  }

  TestExecutor executor;
  auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(std::move(request)));

  const auto* call_response = std::get_if<ua::CallResponse>(&response);
  ASSERT_NE(call_response, nullptr);
  ASSERT_EQ(call_response->results.size(), 3u);
  ASSERT_EQ(call_response->results[0].output_arguments.size(), 1u);
  EXPECT_EQ(call_response->results[0].output_arguments[0].get<NodeId>(),
            (NodeId{1, 2}));
  EXPECT_EQ(call_response->results[1].status_code.code(),
            StatusCode::Bad_MethodInvalid);
  EXPECT_TRUE(call_response->results[1].output_arguments.empty());
  ASSERT_EQ(call_response->results[2].output_arguments.size(), 1u);
  EXPECT_EQ(call_response->results[2].output_arguments[0].get<NodeId>(),
            (NodeId{3, 2}));
}

}  // namespace
}  // namespace opcua
