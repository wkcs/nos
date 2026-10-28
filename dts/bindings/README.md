# NOS device-tree bindings

Bindings are JSON objects with a `compatible` string and a `properties` map.
The generator selects the first matching compatible in node order. Each rule
supports `type` (`string`, `uint32-array`, `boolean`, or `phandle-array`), optional `required`,
`min-items`, `max-items`, `minimum` (for numeric arrays), and `enum`. Duplicate compatible bindings are an error.

Nodes without a matching binding are currently retained without validation;
add a binding before a driver relies on their properties. This format is a
small NOS-specific subset and is not a Zephyr YAML or Linux dt-schema binding.
