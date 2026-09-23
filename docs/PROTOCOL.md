# Control-fabric protocol

## Framing

```
u32 magic ("SNCF" = 0x534E4346)   u16 protocol version
u16 frame kind                   u16 flags
u16 reserved                     u32 payload length (little-endian)
u32 payload CRC-32C              u64 request identity
payload bytes
```

The header is 28 bytes. All integers are little-endian. The payload is a
canonical JSON document: keys sorted byte-wise, unique, integers only, no
insignificant whitespace, valid UTF-8.

A frame is refused, and its connection closed, when the magic, version, kind,
length bound or checksum does not match. A damaged frame cannot be
resynchronised, so the connection is not reused after a framing failure. A
partial frame is not an error: the reader waits for more bytes, and a peer that
closes mid-frame simply produces no response at all — never a success.

## Requests

```json
{"args":{...},"op":"activate"}
```

The request document must carry exactly `op` and `args`; an unknown field is
refused with `refused_unknown_field` rather than ignored.

## Responses

Success:

```json
{"ok":true, ...operation specific fields...}
```

Refusal (frame kind `error`):

```json
{"detail":"human readable detail","ok":false,"reason":"refused_scope_conflict"}
```

The `reason` field is the canonical spelling of a stable reason code, so a client
never has to parse prose to recover semantics.

## Operations

| Operation | Purpose |
| --- | --- |
| `ping` | version, semantics identifier, epoch and boot identity |
| `register_smartnic` | register a logical SmartNIC |
| `register_device` | register a physical device and its incarnation |
| `register_package` | register a function package and its requirements |
| `submit_capability_evidence` | accept a capability report |
| `submit_observation` | accept a device presence observation |
| `reverify_evidence` | revalidate persisted evidence after a restart |
| `acquire_authority` | grant a lease and a fencing token for a scope |
| `release_authority` | revoke a lease |
| `plan_deployment` | evaluate a deployment without mutating anything |
| `activate` | dispatch a bounded activation attempt |
| `acknowledge` | record that the execution side received the order |
| `report_effect` | record what the execution side did |
| `request_quiesce` | record drain intent and withdraw authority |
| `request_withdraw` | record withdrawal intent and withdraw authority |
| `request_rollback` | dispatch a return to the previous package |
| `inspect_instance` | read one instance record |
| `explain_instance` | explain an instance's current state |
| `explain_last_decision` | explain the most recent decision |
| `export` | canonical machine-readable export |
| `counters`, `recovery`, `events`, `history` | operational surfaces |
| `shutdown` | request a graceful stop |

An unrecognised operation is refused with `refused_out_of_boundary`.

## Bounded concurrency

The server is started with a connection bound and a worker count. Connections
beyond the bound receive an `error` frame carrying `refused_overloaded` and are
closed; the refusal is counted in `ServerStats.connections_refused`. Each
connection is served by one worker at a time, and the per-connection read buffer
is bounded independently of the frame bound.

## Shutdown

A `shutdown` request is answered before the connection is closed. The serving
worker signals the server's shutdown condition, waking any caller blocked in
`wait_for_shutdown`. The server then stops accepting, unblocks every in-flight
connection with an explicit socket shutdown, drains, and joins its workers. No
timeout is involved at any point.
