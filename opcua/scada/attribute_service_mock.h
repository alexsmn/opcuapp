#pragma once

#include "opcua/scada/attribute_service.h"

#include <gmock/gmock.h>

namespace opcua {

class MockAttributeService : public AttributeService {
 public:
  MOCK_METHOD((Awaitable<StatusOr<std::vector<DataValue>>>),
              Read,
              (ServiceContext context,
               std::shared_ptr<const std::vector<ReadValueId>> inputs),
              (override));

  MOCK_METHOD((Awaitable<StatusOr<std::vector<StatusCode>>>),
              Write,
              (ServiceContext context,
               std::shared_ptr<const std::vector<WriteValue>> inputs),
              (override));
};

class SimpleMockAttributeService : public AttributeService {
 public:
  SimpleMockAttributeService() {
    using namespace testing;

    ON_CALL(*this, Read(_))
        .WillByDefault(Return(MakeReadError(StatusCode::Bad)));

    ON_CALL(*this, Write(_, _)).WillByDefault(Return(StatusCode::Good));
  }

  MOCK_METHOD(DataValue, Read, (const ReadValueId& value_id));

  MOCK_METHOD(StatusCode,
              Write,
              (const ServiceContext& context, const WriteValue& value));

  virtual Awaitable<StatusOr<std::vector<DataValue>>> Read(
      ServiceContext context,
      std::shared_ptr<const std::vector<ReadValueId>> inputs) override {
    std::vector<DataValue> results(inputs->size());
    for (size_t i = 0; i < inputs->size(); ++i)
      results[i] = Read((*inputs)[i]);
    co_return std::move(results);
  }

  virtual Awaitable<StatusOr<std::vector<StatusCode>>> Write(
      ServiceContext context,
      std::shared_ptr<const std::vector<WriteValue>> inputs) override {
    std::vector<StatusCode> results(inputs->size());
    for (size_t i = 0; i < inputs->size(); ++i)
      results[i] = Write(context, (*inputs)[i]);
    co_return std::move(results);
  }
};

}  // namespace opcua (vendored)
