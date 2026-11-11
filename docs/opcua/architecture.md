# Ownership and memory

```mermaid
flowchart TB
  subgraph M7["M7 / UI application"]
    GUI[TouchGFX widgets] --> GB[Thin GUI backend]
    GB --> S["ui::Services<br/>bounded operations, snapshots and messages"]
    UA["OPC UA owner<br/>reflected catalog and sessions"] --> S
    CLI[CLI] --> C["Single motion::Client + Robot<br/>jobs, source ownership, supervision"]
    S --> C
    C --> P[Existing planning worker]
    C <--> B[ICC bridge]
    NET["Wi-Fi/SDIO → NetX<br/>network worker and packet pools"] <--> UA
  end
  B <--> IO["M4 ICC + motion::Controller<br/>authoritative admission, STEP/DMA, stop branches"]
  IO --> H[Physical IO]
```

`src/ui/opcua` owns the server, reflection catalog, transport adapters, allocation and compatibility adaptations. `platform/network` owns radio/SDIO/NetX and byte-safe external-memory access. Hardware networking has no motion/catalog dependency. The standalone sample links the same reusable components without application services.

The UA owner is the only thread touching UA server objects. Notifications and cached snapshots bridge application workers; network callbacks never execute robot methods. Shared services provide 16 ordinary and four urgent operation slots. Notifications are bounded weak subscriptions; outcomes are correlated instead of sharing one replaceable GUI result. Motion jobs retain the existing single 32-entry execution history.

```mermaid
sequenceDiagram
  participant R as Remote client
  participant U as OPC UA worker
  participant S as UI services
  participant C as Motion client / planner
  participant I as M4 controller
  R->>U: BeginControl (local permission already ON)
  U->>C: Open supervised source generation
  C-->>R: Generation
  loop Every 100 ms
    R->>U: Heartbeat + held-job IDs
    U->>C: Renew source/input deadlines
  end
  R->>U: Method(generation, increasing request ID, bounded record)
  U->>U: Types/bounds/permission/dedup validation
  U->>S: Nonblocking submit
  S->>C: Existing application operation
  S-->>R: Submission receipt + operation/job ID
  C->>I: Reserve/admit through existing ICC
  I-->>C: Accepted or rejected
  C->>I: Prepared motion, if valid
  I-->>C: Telemetry and terminal result
  C-->>S: Correlated outcome / shared history
  S-->>U: Cached state + notification
  U-->>R: Reads/subscriptions/job pages
  Note over C,I: Expiry/disconnect selects existing stop/abort path.<br/>No fresh remote planning is needed to stop.
```

## SDRAM reservations

The active framebuffer bytes stay at their original addresses. Reserved tails, not active images, provide network/UA storage. These are **external AS4C4M16SA SDRAM**, not M4 internal SRAM or ICC SRAM4.

```mermaid
block-beta
  columns 1
  A["0xC0000000–0xC012BFFF · RGB565 framebuffer 0 · 1,228,800 B"]
  B["0xC0180000–0xC01FFFFF · OPC UA arena · 512 KiB"]
  C["0xC0200000–0xC032BFFF · RGB565 framebuffer 1 · 1,228,800 B"]
  D["0xC0380000–0xC03BFFFF · networking · 256 KiB"]
  E["0xC03C0000–0xC03DFFFF · snapshots / USB byte rings · 128 KiB"]
  F["0xC03E0000–0xC03FFFFF · unallocated · 128 KiB"]
  G["0xC0400000–0xC05FFFFF · existing M4 RAM-debug reservation · 2 MiB"]
  H["0xC0600000–0xC07FFFFF · existing 3D renderer buffers · 2 MiB"]
```

The small gaps before each reclaimed tail are not assigned. Linker assertions enforce placement and overlap limits. Active framebuffers remain non-cacheable. New arenas are normal, cacheable, execute-never memory; boot access tests and scoped byte-safe memory routines cover odd-address and cache-line-crossing accesses. `-mno-unaligned-access` is applied to affected code. This is a workaround for the FMC access restriction, not a claim that caching removes it.

| Internal M7 storage retained | Reason |
|---|---|
| ICC/client/planner/GUI stacks and ISR state | Existing latency and lifecycle requirements |
| SDIO DMA bounce storage | DMA accessibility and explicit cache ownership |
| OPC UA owner stack, 14 KiB | Measured complete-catalog call depth; increased from the initial 12 KiB proposal |
| Network management stack, 8 KiB | Bounded application-owned network worker |
| USB callback state, packet staging, worker | Interrupt-facing lifetime and DMA constraints |

Only USB's 20 KiB byte rings moved out of DTCM. The M4 heap/data, SRAM4 ICC rings, profiling reservations and renderer buffers are unchanged. Internal DTCM is nearly full; new large state must not be added there casually. The UA arena is 512 KiB on GIGA and 1 MiB on 64-bit Linux. No UA/vendor allocation falls back to the general UI heap.

Initial priorities: ICC/client/GUI keep their existing precedence; WHD 9, IP 10, DHCP 11, OPC UA 12, WCM 13, network management 14. The server bounds work per pass. The motion client checks 500 ms remote expiry independently of those workers, preserving GUI's 200 ms held-input supervision and M4's existing leases.

## Port maintenance

- `external/open62541` and `external/netxduo` remain upstream sources. Project-owned build-tree adaptations provide the native NetX event loop, selected default-config changes and a 1.5.10 string method-argument NodeId leak fix.
- NetX `nx_tcp_server_socket_accept(..., NX_NO_WAIT)` arms a passive open. The adapter calls it once and watches completion under the IP mutex; it must not poll it after peer state has been cleared by an abandoned handshake. Host fake-NetX regressions and real interrupted-TCP tests cover this.
- `NX_IP_PERIODIC_RATE` equals `TX_TIMER_TICKS_PER_SECOND` (1 kHz here), rather than the Cortex-M port's 100 Hz default. This changes network timer scaling only.
- TX and RX packet pools are separate and both are reported. RX starvation must not be inferred from a healthy TX pool.
- The allocator uses bounded best-fit blocks with adjacent coalescing; server shutdown returns its arena to zero. No global heap fallback hides exhaustion.
- Server/network failure revokes remote permission and cleans up owned work without shutting down unrelated GUI/CLI/local motion. ICC failure keeps the server available with stale quality and Unknown outcomes.
