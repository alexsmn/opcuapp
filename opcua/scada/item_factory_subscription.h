#pragma once

#include "opcua/scada/monitored_item.h"
#include "opcua/scada/status_or.h"

#include <functional>
#include <memory>

namespace opcua {
namespace scada {

// opcuapp factory that creates a single legacy `MonitoredItem` for the given
// value and monitoring parameters, or returns null if the item cannot be
// created; a building block for the MonitoredItem Service Set. OPC UA Part 4
// §5.13 MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
//
// Factory that creates a single legacy `MonitoredItem` for the given value and
// monitoring parameters, or returns null if the item cannot be created.
using MonitoredItemFactory =
    std::function<std::shared_ptr<MonitoredItem>(const ReadValueId& value_id,
                                                 const MonitoringParameters&
                                                     params)>;

// Builds a `MonitoredItemSubscription` on top of the per-item `factory`. Each
// `AddItems` request invokes the factory once and routes the created item's
// data-change or event callbacks into the subscription's notification stream.
// This is the generic adapter that lets a service expose the subscription API
// while keeping its existing single-item creation logic. OPC UA Part 4 §5.13
// MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
StatusOr<std::unique_ptr<MonitoredItemSubscription>> MakeItemFactorySubscription(
    MonitoredItemFactory factory,
    MonitoredItemSubscriptionOptions options);

}  // namespace scada
}  // namespace opcua (vendored)
