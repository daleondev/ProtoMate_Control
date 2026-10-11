# Two-bus topic and service examples

Build and run from the repository root (GCC 16 with reflection):

```sh
cmake --preset debug-linux
cmake --build --preset debug-linux --target application_p2p_example application_p2p_service_example application_p2p_transport_tests application_p2p_service_tests
./build/debug-linux/src/p2p/examples/application_p2p_example
./build/debug-linux/src/p2p/examples/application_p2p_service_example
ctest --test-dir build/debug-linux -R '^application\.p2p\.' --output-on-failure
```

The examples build independently of the unfinished Robot/Controller integration
and requires no hardware. The repository build uses its Linux/ThreadX runtime.

Start with [Topics.hpp](Topics.hpp), then read [topics.cpp](topics.cpp).
The catalog is where we specify **what crosses the bridge and who publishes it**:

| Topic | Publisher's bus | Subscriber's bus |
| --- | --- | --- |
| `demo/io/state` | IO | UI |
| `demo/ui/status` | UI | IO |

```mermaid
flowchart LR
    IO[IO bus] <--> IB[IO bridge]
    IB <-- serialized frames --> T[InProcessTransport pair]
    T <-- serialized frames --> UB[UI bridge]
    UB <--> UI[UI bus]
```

Each bridge registers the same catalog once, identifying its **local publisher**:

```cpp
io_bridge.registerTopics<Topics>(TopicPublisher::IO);
ui_bridge.registerTopics<Topics>(TopicPublisher::UI);
```

Topics annotated with the local publisher are forwarded to the peer; those
annotated with the peer's identity are received locally. The publisher enum must
declare exactly two distinct values; annotations must use those values.
Unannotated topics stay local. The enum belongs to the application, not p2p.

[Services.hpp](Services.hpp) defines the service catalog. Its annotations identify
the **provider**, not the request direction:

```cpp
io_bridge.registerServices<Services>(ServiceProvider::IO);
ui_bridge.registerServices<Services>(ServiceProvider::UI);
```

For a service annotated `ServiceProvider::IO`, the IO bridge accepts incoming
requests for its local handler; the UI bridge forwards local requests to IO.
Replies return automatically. [services.cpp](services.cpp) shows the complete
request/response example: IO serves `demo/io/echo`, UI calls it with `hello`, and
receives `IO says: hello`. Both use ordinary local bus handles:

```cpp
auto provider = Services::echo.on(io_bus).serve([](const std::string& request) {
    return "IO says: " + request;
});
auto call = Services::echo.on(ui_bus).request("hello", 2s);
provider.poll(2s);     // Dispatch the real handler on its application worker.
auto response = call.get(); // Wait on the caller's worker, never the p2p thread.
```

Here main dispatches the provider and waits for the response while the dedicated
p2p thread forwards both ways. A real application dispatches providers on its IO
worker or through pneumo's coroutine API. For deferred work, `serveDeferred()`
hands the handler a `Reply` token to retain and complete later; the bridge forwards
the eventual response or error without executing the handler itself. Keep the
call handle alive until completion; destroying it cancels the local call.

Both bridges use one context on a dedicated p2p thread, following the application's
`context.run()` thread-entry pattern. Each bridge has separate send and receive
coroutines. Waiting for incoming bytes does not prevent outgoing work. After the
dual-core migration, each core will have its own context and thread.

Main publishes `idle` and `ready`, then waits up to two seconds per local
subscription and dispatches its callback. Those callbacks run on main, while
serialization and forwarding run on the p2p thread. Expected output:

```text
UI received demo/io/state: idle
IO received demo/ui/status: ready
Both directions verified; bridges stopped cleanly.
```

Stop both bridges while the context still runs, then stop and join its thread.
Declaration order also enforces this cleanup if the example throws. No transport
disconnect is needed for normal shutdown.

Outgoing topics retain the latest pending value; services retain separate calls
with correlation IDs and bounded capacity. Up to eight calls are forwarded per
direction; additional calls report `Busy`. Errors and cancellation propagate,
and deadline timers wake idle bridges. Cancellation is best effort: it ends the
local wait and invalidates the remote reply when received, but cannot undo work
the handler has already done. Actions and peer-restart sessions remain later steps.
