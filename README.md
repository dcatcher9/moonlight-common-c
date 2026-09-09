# Moonlight Streaming Core Library

Moonlight-common-c contains the core GameStream client code shared between [Moonlight](https://moonlight-stream.org) clients, including [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt), [Moonlight Android](https://github.com/moonlight-stream/moonlight-android), [Moonlight iOS](https://github.com/moonlight-stream/moonlight-ios), and [Moonlight Chrome](https://github.com/moonlight-stream/moonlight-chrome).

If you are implementing your own Moonlight game streaming client that can use a C library, you probably want the code here.

## Note to Developers

Moonlight-common-c requires the _specific_ version of ENet that is bundled as a submodule. This version has changes required for IPv6 compatibility and retransmission reliability, among other things. These are breaking API/ABI changes which make Moonlight-common-c incompatible with other versions of the ENet library. Attempting to runtime link to another libenet library will cause your client to crash when connecting to recent versions of GeForce Experience.

### Termination callbacks and reconnects

Termination callbacks run asynchronously and may arrive after `LiStopConnection()` returns or another connection starts. Each queued notification retains the callback and error from the connection that scheduled it. To distinguish reconnects that share a callback function, set `CONNECTION_LISTENER_CALLBACKS.connectionTerminatedWithSession` and assign a distinct `connectionSessionId` before each `LiStartConnection()`. The identifier is an opaque, client-local `uint64_t`; it does not change the host protocol or SBS behavior.

When the session-aware callback is set, it takes precedence over `connectionTerminated`. Its callback, error, and identifier are captured for delivery. The client must reject an old identifier before reporting an error or stopping the current stream, and serialize that check and any resulting stop with connection replacement. Calling `LiStopConnection()` inside a callback remains supported; stopping does not join detached termination callbacks.

Legacy callbacks remain supported when the optional callback is NULL, but they cannot identify a stale notification when connections share the same callback. Initialize callback structures with `LiInitializeConnectionCallbacks()` or zero initialization. These appended fields change the structure size, so rebuild the library and callers together; cross-version binary ABI compatibility is not provided.

The deterministic native regression uses the real connection start, stop, and termination dispatcher with platform/network/stream dependencies stubbed and thread delivery controlled by the test. Build and run it with:

```sh
cmake -S . -B build -DLC_BUILD_CONNECTION_TERMINATION_TESTS=ON
cmake --build build --target connection-termination-test
ctest --test-dir build --output-on-failure -R '^connection-termination-test$'
```
