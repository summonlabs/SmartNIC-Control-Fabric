# Architecture

## Boundary

SmartNIC Control Fabric owns control-plane state for programmable NIC functions.
It consumes evidence produced by adjacent runtimes and never re-implements them:

| Input | Expected producer | Accepted sources |
| --- | --- | --- |
| SmartNIC / device / package registration | topology runtime or operator | any principal with a command identity |
| Capability report | capability evidence provider | `capability_evidence_provider`, `operator`, `synthetic_fixture` |
| Device observation | device observation runtime | `device_observation_runtime`, `operator`, `synthetic_fixture` |
| Effect report | execution runtime | `execution_runtime`, `synthetic_fixture` |
| Policy generation | policy runtime | supplied per request as a generation value |

Anything outside that boundary — firmware, packet processing, vendor SDKs, route
computation, host networking, telemetry collection — is refused with
`refused_out_of_boundary` rather than absorbed.

## Module layout

| Header | Responsibility |
| --- | --- |
| `sncf/strong.hpp` | Strongly typed identities and checked arithmetic |
| `sncf/status.hpp` | Stable reason codes, `Status`, `Result` |
| `sncf/ids.hpp` | Identity, generation, epoch, incarnation and capability types |
| `sncf/digest.hpp` | SHA-256 and CRC-32C |
| `sncf/time.hpp` | Injected clock; no sleeping and no timeouts anywhere |
| `sncf/canonical.hpp` | Canonical JSON value model, writer and strict parser |
| `sncf/evidence.hpp` | Evidence envelopes, provenance, freshness policy |
| `sncf/lifecycle.hpp` | Lifecycle, attempt, authority and freshness state machines |
| `sncf/authority.hpp` | Exclusive scopes, leases, durable fencing ledger |
| `sncf/model.hpp` | Durable records, counters, configuration and state container |
| `sncf/persistence.hpp` | Framed journal, snapshots, recovery classification |
| `sncf/explain.hpp` | Deterministic explanation surface |
| `sncf/fabric.hpp` | Public runtime API |
| `sncf/protocol.hpp` | Framed bounded wire protocol |
| `sncf/service.hpp` | Protocol-to-runtime adapter |
| `sncf/transport.hpp` | Bounded TCP server and client |

Implementation files mirror the headers; `src/fabric.cpp` holds durable open,
replay, commit, apply and export, while `src/fabric_ops.cpp` holds every
operation decision. `src/fabric_impl.hpp` is the shared internal interface.

## Time

The runtime has no timers. `Clock` is injected; `ManualClock` makes every
freshness, expiry and lease decision reproducible in tests, and `SystemClock` is
the production default. Nothing in the codebase sleeps, polls or applies a
timeout: shutdown is driven by explicit wakeups (a self-connect to the listener,
an explicit socket shutdown, a condition variable), and every test synchronises
on completed work.

## Decision pipeline

Every mutating call follows the same five steps:

1. **Dedupe.** `(command, principal)` is looked up in a bounded table rebuilt
   from the journal. A hit returns the recorded outcome and changes nothing.
2. **Decide.** A pure function of the current state and the request produces a
   `Decision`: accept or refuse, a primary reason code, ordered explanation
   factors, and the entity patches that would be applied.
3. **Journal.** The decision is encoded as one record, appended and flushed to
   stable storage. Nothing mutable has happened yet, so a write failure is a
   clean refusal.
4. **Apply.** `Impl::apply` installs the record's patches. This is the only
   mutation path, shared with replay, so live state and replayed state cannot
   diverge.
5. **Report.** The typed result returns to the caller and the decision is
   recorded in the bounded event ring and the reason counters.

Refusals stop after step 2: they never journal, never mutate and never become a
partial effect.

## Record shapes

A journal record carries `kind`, `command`, `principal`, `epoch`, `at`,
`outcome`, `subject` and a `body` that is a set of entity patches:

`smartnic`, `device`, `package`, `evidence`, `observation`, `lease`,
`instance`, `instances` (array), `fencing` (array), `epoch`+`boot`, and
`alloc` (allocator high-water marks).

`kind` labels the decision for explanations and history; the patches are what
replay reproduces. A patch that cannot be decoded refuses the record, so a body
key can never be silently ignored.

## Locking

The coordinator uses one mutex. Every public `Fabric` method takes it, calls a
pure `decide_*` function, commits and applies, then returns. No user callback is
invoked under the lock, no lock is acquired twice, and the transport never calls
into the runtime while holding a transport lock.

Two injected collaborators are called while the lock is held: the `Clock` and
the `Journal`. Both are library-owned or embedder-supplied objects that must not
re-enter the runtime; `sncf/time.hpp` states that requirement on `Clock`.
Neither calls back into the `Fabric`, and neither takes a lock of its own.

The transport has three mutexes used in a strict, one-directional order:

1. `Impl::mutex` — connection queue and active-connection set
2. `Impl::stats_mutex` — every `ServerStats` field
3. `Impl::shutdown_mutex` — shutdown signalling

Statistics live in exactly one lock domain: every update, including the
connection bookkeeping, happens under `stats_mutex`, and `Server::stats()` reads
under the same mutex. `Impl::mutex` may be held when `stats_mutex` is taken;
never the other way round. `shutdown_mutex` is never taken while either of the
others is held.

## Shutdown order

`Server::stop()` is serialised by a teardown mutex, so a second or concurrent
caller blocks until teardown is complete rather than returning early, and the
service object outlives `stop()` so a concurrent `shutdown_requested()` never
dereferences a released pointer. Joins are skipped if the caller is the thread
being joined. The order is:

1. Set the stopping flag.
2. Wake the accept loop with a self-connect (no timeout is involved).
3. Join the accept thread.
4. Under the connection lock: mark the queue closed, close every queued socket,
   and `shutdown()` every active socket so blocked reads return end-of-stream.
5. Notify the worker condition variable and release any waiter in
   `wait_for_shutdown`.
6. Join all workers outside every lock.
7. Close the listening socket and release the service.

No thread is ever joined while a lock the joined thread needs is held.

## Failure semantics

* Submission is not completion: `activate` returns a grant, not an effect.
* Recommendation is not authorization: `plan_deployment` mutates nothing,
  appends nothing, allocates no identity and cannot change the durable digest.
* Authorization is not application: an acknowledgement moves the attempt to
  `acknowledged`, never to `applied`.
* Application is not verification: `applied` and `verified` are separate
  enforcement-side reports.
* An interruption between dispatch and report leaves the attempt `ambiguous`,
  and the ambiguity is resolved only by a matching enforcement-side report.
