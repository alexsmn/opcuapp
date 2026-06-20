#pragma once

#include "opcua/base/any_executor.h"

#include <cassert>
#include <memory>
#include <optional>

namespace opcua::scada::service_resolver {

template <typename Service>
Service& RequireSharedService(const std::shared_ptr<Service>& service) {
  assert(service);
  return *service;
}

template <typename CoroutineService, typename CallbackService, typename Adapter>
CoroutineService* ResolveCoroutineService(
    const AnyExecutor& executor,
    const std::shared_ptr<CoroutineService>& coroutine_service,
    const std::shared_ptr<CallbackService>& callback_service,
    std::unique_ptr<Adapter>& adapter) {
  if (coroutine_service)
    return coroutine_service.get();

  if (!callback_service)
    return nullptr;

  if (auto* service = dynamic_cast<CoroutineService*>(callback_service.get()))
    return service;

  adapter = std::make_unique<Adapter>(executor, *callback_service);
  return adapter.get();
}

template <typename CoroutineService, typename CallbackService, typename Adapter>
CoroutineService* ResolveCoroutineService(
    const std::optional<AnyExecutor>& executor,
    const std::shared_ptr<CoroutineService>& coroutine_service,
    const std::shared_ptr<CallbackService>& callback_service,
    std::unique_ptr<Adapter>& adapter) {
  if (coroutine_service)
    return coroutine_service.get();

  if (!callback_service)
    return nullptr;

  if (auto* service = dynamic_cast<CoroutineService*>(callback_service.get()))
    return service;

  if (!executor)
    return nullptr;

  adapter = std::make_unique<Adapter>(executor, *callback_service);
  return adapter.get();
}

template <typename CoroutineService,
          typename CallbackService,
          typename CallbackToCoroutineAdapter,
          typename CoroutineToCallbackAdapter>
CoroutineService* ResolveCoroutineService(
    const std::optional<AnyExecutor>& executor,
    const std::shared_ptr<CoroutineService>& coroutine_service,
    const std::shared_ptr<CallbackService>& callback_service,
    std::unique_ptr<CallbackToCoroutineAdapter>& callback_to_coroutine_adapter,
    std::unique_ptr<CoroutineToCallbackAdapter>& coroutine_to_callback_adapter) {
  auto* service = ResolveCoroutineService(
      executor, coroutine_service, callback_service,
      callback_to_coroutine_adapter);
  if (service && executor) {
    coroutine_to_callback_adapter =
        std::make_unique<CoroutineToCallbackAdapter>(executor, *service);
  }
  return service;
}

template <typename CoroutineService, typename CallbackService, typename Adapter>
std::shared_ptr<CoroutineService> ResolveCoroutineServiceShared(
    const AnyExecutor& executor,
    const std::shared_ptr<CoroutineService>& coroutine_service,
    const std::shared_ptr<CallbackService>& callback_service) {
  if (coroutine_service)
    return coroutine_service;

  if (!callback_service)
    return nullptr;

  if (auto* service = dynamic_cast<CoroutineService*>(callback_service.get())) {
    return std::shared_ptr<CoroutineService>{callback_service, service};
  }

  return std::make_shared<Adapter>(executor, *callback_service);
}

template <typename CoroutineService, typename CallbackService, typename Adapter>
CoroutineService& RequireCoroutineService(
    const AnyExecutor& executor,
    const std::shared_ptr<CoroutineService>& coroutine_service,
    const std::shared_ptr<CallbackService>& callback_service,
    std::unique_ptr<Adapter>& adapter) {
  auto* service = ResolveCoroutineService(executor, coroutine_service,
                                          callback_service, adapter);
  assert(service);
  return *service;
}

}  // namespace opcua::scada::service_resolver
