#!/usr/bin/env python3
"""Flatten opcua::scada:: into opcua::, keeping the monitored-item domain
cluster nested (it is reworked to wire forms in a later stage). Idempotent.

Run from the opcuapp repo root:  python3 tools/codemod_flatten.py
"""
import re
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "opcua"

# Monitored-item domain names that must stay in opcua::scada:: for now (the 6
# colliders + the abstraction/machinery they belong to). References to these
# keep their scada:: / opcua::scada:: qualification; everything else flattens.
KEEP = {
    "DataChangeFilter", "MonitoringFilter", "MonitoringParameters",
    "MonitoredItemId", "MonitoredItemCreateRequest", "MonitoredItemCreateResult",
    "MonitoredItemSubscriptionOptions", "DataChangeNotification",
    "EventNotification", "ItemStatusNotification", "OverflowNotification",
    "MonitoredItemNotification", "DataChangeHandler", "EventHandler",
    "MonitoredItemHandler", "MonitoredItem", "MonitoredItemSubscription",
    "MonitoredItemService", "MonitoredItemSubscriptionPump",
    "LegacyMonitoredItemAdapter", "MonitoredItemFactory", "MakeItemFactorySubscription",
    "MockMonitoredItem", "MockMonitoredItemService",
    "MockDataChangeHandler", "MockEventHandler",
}

# Files under opcua/scada/ that DEFINE the kept cluster — keep their nested
# `namespace scada` block intact.
CLUSTER_FILES = {
    "monitored_item.h", "monitoring_parameters.h", "data_change_filter.h",
    "monitored_item_service.h", "monitored_item_subscription_pump.h",
    "monitored_item_subscription_pump.cpp", "legacy_monitored_item_adapter.h",
    "legacy_monitored_item_adapter.cpp", "item_factory_subscription.cpp", "item_factory_subscription.h",
    "monitored_item_mock.h", "monitored_item_service_mock.h",
}

QUAL = re.compile(r"\bopcua::scada::(\w+)")     # opcua::scada::X
BARE = re.compile(r"(?<![:\w])scada::(\w+)")     # bare scada::X (not opcua::scada::)


def flatten_refs(text: str) -> str:
    text = QUAL.sub(lambda m: m.group(0) if m.group(1) in KEEP
                    else "opcua::" + m.group(1), text)
    text = BARE.sub(lambda m: m.group(0) if m.group(1) in KEEP
                    else m.group(1), text)
    return text


def collapse_namespace(text: str) -> str:
    # Drop the inner `namespace scada {` opener(s) and `}  // namespace scada`
    # closer(s); contents merge up into the surrounding `namespace opcua`.
    text = re.sub(r"^namespace scada \{\n", "", text, flags=re.MULTILINE)
    text = re.sub(r"^\}  // namespace scada\n", "", text, flags=re.MULTILINE)
    return text


def main() -> None:
    changed = 0
    for path in ROOT.rglob("*"):
        if path.suffix not in (".h", ".cpp"):
            continue
        text = path.read_text()
        new = flatten_refs(text)
        rel_scada = "scada" in path.relative_to(ROOT).parts[:1]
        if rel_scada and path.name not in CLUSTER_FILES:
            new = collapse_namespace(new)
        if new != text:
            path.write_text(new)
            changed += 1
    print(f"flatten codemod: {changed} files changed")


if __name__ == "__main__":
    main()
