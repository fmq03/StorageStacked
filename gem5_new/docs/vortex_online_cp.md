# gem5 online command-processor memory

The existing CP hooks synchronously fetch data through a functional proxy and separately enqueue timing traffic. A downstream protocol link cannot supply authoritative response data through that arrangement.

Run each CP tick in a gem5 Coroutine on the same host thread. The synchronous read/write hooks issue DmaPort timing requests and yield to the caller. The normal gem5 completion event schedules resumption on the device clock; a read copies the returned bytes before its hook returns. This preserves the command processor's stack, including multi-part descriptors and DMA copies, without speculative reads or a second simulation kernel. Core requests retain their existing asynchronous completion interface.

Only hosted timing-memory mode uses this mechanism. A suspended hook owns exactly one pending CP request, and a new tick cannot start until that request and the previous tick finish. Backpressure and latency are therefore supplied by the memory path. Standalone/private-memory mode remains synchronous.

Acceptance: hosted kernel result checks, CP/core byte and completion accounting, zero functional target accesses during measured execution, and downstream latency perturbation. This changes the external gem5 coupling rather than the default standalone SimX/RTL timing model.
