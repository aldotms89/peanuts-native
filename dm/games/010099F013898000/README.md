# World War Z

Title ID `010099F013898000`, update `v458752`, SDK 10.4.1. Experimental.

## Status

On the earlier development build, the game entered offline solo gameplay on an AYN Thor with Adreno 740 and Turnip T30. Light areas reached around 30 fps. Town gameplay ran at 17–22 fps before the second round of replay optimizations. One run had an unexplained in-level crash. On main build `05ca2d8bfae05719e8eee7da75d90338`, the fault was a pointer-table read at `main+0x1c6e22c`. The writer of the invalid index remains open.

## Address space

The SDK reserves 8 GiB for StandardAllocator, then a game initializer requests another 2 GiB and writes to its result without checking it. With the smaller alias region the second reservation failed and the write faulted on a null pointer. The profile reserves a 64 GiB virtual alias region. Its pages are committed on demand.

## Offline match startup

The local server resolves a numeric wildcard address and binds one port from its requested range. The profile enables `net_loopback` so it can bind to IPv4 loopback and exchange packets with the in-process client while NIFM still reports offline.

## Shaders

The game indexes shader records in Saber Swarm SDC caches. Separate-image size queries previously prevented translation of 40 compute keys. The Vulkan replay also needs to resolve either word order in split uniform-buffer texture handles. `dual_want.tsv` lists 456 fragment keys with two float colour outputs. The replay uses the dual-source variant when the pipeline uses a secondary-source blend factor.

## Found on this game

- [A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)
