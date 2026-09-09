# Shared client feature migration

This revision consolidates the VoidLink microphone transport and authored DualSense
haptics APIs into the shared core. It retains the session-aware termination callback
and detached-thread lifetime fix. Capture, permissions, audio routing, controller
rendering, and application lifecycle policy remain in each client.

This publication changes the shared core only. It makes no iOS workspace edits or
submodule switch; that client can consume the final published revision during its
separate adoption and qualification.

These are source APIs: rebuild clients against the same revision and initialize
configuration/callback structures with the library helpers. This document does not
promise binary ABI compatibility between independently compiled revisions.

## Host compatibility

| Host | Microphone | Authored DualSense PCM | Existing streaming |
| --- | --- | --- | --- |
| Sunshine/Apollo without these extensions | Optional microphone setup falls back to unavailable | No PCM capability is advertised by the client | Existing audio, video, and controller paths remain available |
| Sunshine 3D with its established SBS capabilities | Unavailable until a real receiver and microphone sink are added | Unavailable until a real authored PCM producer is added | Established SBS capability assignments are preserved |
| Compatible Foundation/VoidLink host | Uses the legacy negotiated Opus transport when enabled | Uses the legacy haptics capability profile when a callback is registered | Ordinary controller feedback remains separate |
| Host implementing the shared haptics capability profile | Same microphone transport | Uses the new nonconflicting capability assignments | SBS and authored PCM can coexist |

The inspected compatible host is
[Foundation Sunshine at `3e142f7d192ea26a4283ca8c9ab7c7d30db62023`](https://github.com/AlkaidLab/foundation-sunshine/tree/3e142f7d192ea26a4283ca8c9ab7c7d30db62023).
Its [Windows microphone sink](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/src/platform/windows/mic_write.cpp)
requires a usable virtual audio route for applications to receive microphone PCM.
Transport setup alone does not prove that the Windows input device is working.

Its authored PCM producer is a separate virtual DualSense backend. The
[sidecar contract](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/tools/sunshine-ds5-sidecar/README.md)
requires an elevated helper and the pinned HIDMaestro runtime; the composite profile
exposes HID and four-channel audio interfaces. The
[audio extraction implementation](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/tools/sunshine-ds5-sidecar/DualSenseHapticsAudio.cs)
preserves the two authored haptics channels separately from speaker audio. A ViGEm
Xbox 360/DS4 device alone is not this producer.

Sunshine 3D currently has neither backend. Publishing these shared client APIs does
not add either host feature, install a driver, or qualify a physical device. Its
host must keep these capabilities unavailable until the actual data paths exist.

## Microphone contract

`STREAM_CONFIGURATION.redirectMic` requests microphone forwarding.
`sendMicrophoneOpusData()` accepts an already encoded Opus packet;
`isMicrophoneEncryptionEnabled()` reports the initialized transport's encryption
state. The core owns setup, packetization, encryption, pings, and teardown. The
client owns recording, format conversion, and Opus encoder configuration. A Swift
convenience wrapper such as `opus_encoder_ctl_wrapper()` may stay in the iOS bridge.

The reference is
[VoidLink `MicrophoneStream.c` at `5ec6288caacf07f679123fbd4b2f8ea46ba6724a`](https://github.com/TrueZhuangJia/voidlink-c/blob/5ec6288caacf07f679123fbd4b2f8ea46ba6724a/src/MicrophoneStream.c).
Each UDP packet begins with this 12-byte header, followed by the Opus payload:

| Offset | Field | Encoding |
| --- | --- | --- |
| 0 | Flags | One byte, zero |
| 1 | Packet type | One byte, `0x61` |
| 2 | Sequence number | Little-endian `uint16_t` |
| 4 | Timestamp | Little-endian `uint32_t`, monotonic milliseconds modulo 2^32 |
| 8 | Stream marker | Little-endian `uint32_t`, `0x12345678` |

The entire datagram is at most 1400 bytes. For encrypted transport the header stays
clear and the payload uses AES-128-CBC with the negotiated remote input key. The
legacy [crypto helper](https://github.com/TrueZhuangJia/voidlink-c/blob/5ec6288caacf07f679123fbd4b2f8ea46ba6724a/src/PlatformCrypto.c)
first pads an unaligned Opus input to the next 16-byte boundary through
`CIPHER_FLAG_PAD_TO_BLOCK_SIZE`; an already aligned input receives no manual
padding. `CIPHER_FLAG_FINISH` then adds the cipher's final PKCS7 block. Ciphertext
length is therefore `roundUp(opusLength, 16) + 16`, not just `roundUp(opusLength,
16)`. The encrypted Opus limit is 1360 bytes; the plaintext limit is 1388 bytes.

The shared sender preserves those exact ciphertext bytes. It copies the borrowed
Opus input into a private buffer because the manual padding mutates the crypto
helper's input, and it reserves capacity for both padding operations. The IV
contains big-endian `riKeyId + sequenceNumber` in its first four bytes and zeros in
the remaining twelve. The 16-bit sequence wrap is also preserved; changing this
legacy wire format requires a separately negotiated protocol revision.

The inspected host's
[CBC decryptor](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/src/crypto.cpp#L366)
removes the cipher's final PKCS7 padding. The microphone receive path supplies the
result to Opus without a separate manual unpadding step. Consequently, a wire
compatibility test must expect the rounded input length after decryption, including
the retained manual padding for unaligned packets. Matching legacy ciphertext is
not itself a microphone audio-quality or physical input-device qualification.

The [reference RTSP exchange](https://github.com/TrueZhuangJia/voidlink-c/blob/5ec6288caacf07f679123fbd4b2f8ea46ba6724a/src/RtspConnection.c)
uses `SETUP streamid=mic/0/0` for server generation 5 or newer and `streamid=mic`
for older generations. A valid `server_port` chooses the UDP destination. Optional
`X-SS-Ping-Payload` carries the 16-byte session ping identifier. The shared core
preserves the returned identifier and sends the standard session ping structure,
or the legacy `PING` fallback when absent, from the microphone socket.

The microphone encryption flag is `SS_ENC_MICROPHONE = 0x08` in the **encryption**
namespace, unrelated to the haptics/SBS feature flags below. When the host supports
it, microphone encryption is selected for an explicit microphone encryption
request, a host request, or enabled audio encryption. Hosts without microphone
encryption retain the legacy plaintext option. Unrequested microphone forwarding
does not create a microphone transport.

Unsupported SETUP responses leave microphone unavailable while the normal stream
continues. Cancellation must still cancel the connection. Invalid ports and
malformed ping identifiers must never create a partially initialized transport.
Microphone stage notifications, failed initialization, send/stop races, and repeated
start/stop must keep their resources balanced. Clients must stop their capture
producer before starting another session so an old capture callback cannot submit
audio into the next session.

The [Foundation RTSP handler](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/src/rtsp.cpp)
uses the microphone port at the host base port plus 12 (48001 with the default
base). It advertises microphone SDP when enabled and returns the negotiated port
and ping identifier. That inspected version can return SETUP 200 while microphone
streaming is disabled, without registering a receiver; therefore SETUP success
alone is not an end-to-end microphone test. The client must honor the returned
port rather than hardcode the default.

## Haptics capability profiles

Host `x-ss-general.featureFlags` and client `x-ml-general.featureFlags` are different
namespaces. The shared assignments are:

| Meaning | Host flag | Shared client flag |
| --- | --- | --- |
| Authored PCM available | `LI_FF_DS5_HAPTICS_PCM = 0x80` | `ML_FF_DS5_HAPTICS_PCM = 0x20` |
| Authored IR v2 available | `LI_FF_DS5_HAPTICS_IR_V2 = 0x04000000` | `ML_FF_DS5_HAPTICS_IR_V2 = 0x40` |
| Shared haptics capability profile | `LI_FF_DS5_HAPTICS_CAPABILITIES_V2 = 0x08000000` | No separate client bit |
| Source frame identity v1 | `0x10000000` | `0x10` |
| Atomic presentation v2 | `0x20000000` | `0x08` |
| Host SBS telemetry v2 | `0x40000000` | `0x04` |

The haptics profile marker changes capability assignments, not the PCM payload
version. A host must advertise the marker together with the actual haptics features
it can supply. The marker alone does not imply a PCM producer or an IR analyzer.

The legacy VoidLink/Foundation client assignments are PCM `0x04` and IR `0x08`.
Use that legacy profile only when host PCM `0x80` is present and **none** of the
three shared SBS host flags or the shared haptics marker is present. In that
profile, omit all three shared SBS client bits before optionally adding the
legacy haptics bits. This also applies when no haptics callback is registered:
otherwise an Android client advertising telemetry could accidentally subscribe
to PCM or IR on an old host.

For a host advertising shared SBS capabilities without the new haptics marker,
retain its SBS flags and leave authored haptics unnegotiated. Never reinterpret an
established SBS client bit as haptics on that connection.

A registered PCM callback and host PCM support are required to advertise PCM.
`LI_CCAP_DS5_HAPTICS_PCM = 0x200` remains the per-controller capability for a device
that can render the authored waveform. A client that uses this capability must
also register `ds5HapticsPcm`; a global callback alone does not prove that every
controller can render PCM.

IR v2 remains an explicit opt-in. The shared profile requires its separate host
flag and callback. Legacy profile compatibility does not automatically register
an IR callback. The existing iOS integration's commented-out registration remains
inactive; retaining its types/parser does not qualify active IR rendering.

## Authored PCM payload and ownership

PCM uses control message type `0x550A`. The
[reference host serializer](https://github.com/AlkaidLab/foundation-sunshine/blob/3e142f7d192ea26a4283ca8c9ab7c7d30db62023/src/stream.cpp#L1542)
and [client parser](https://github.com/TrueZhuangJia/voidlink-c/blob/5ec6288caacf07f679123fbd4b2f8ea46ba6724a/src/Ds5HapticsStream.c)
agree on the following little-endian payload header:

| Offset | Field | Size |
| --- | --- | --- |
| 0 | Version, `1` | 1 byte |
| 1 | Stream flags | 1 byte |
| 2 | Header size, at least 28 | 2 bytes |
| 4 | Controller number | 2 bytes |
| 6 | Frame count, at most 480 | 2 bytes |
| 8 | Sequence number | 4 bytes |
| 12 | Presentation time in microseconds | 8 bytes |
| 20 | Sample rate, 48000 | 4 bytes |
| 24 | Channel count, 2 | 1 byte |
| 25 | Bits per sample, 16 | 1 byte |
| 26 | Reserved, zero | 2 bytes |

The reference host currently sends at most 240 frames per packet; the preserved
client parser permits up to 480. PCM begins at the declared header size and
contains exactly `frameCount * 4` bytes of interleaved stereo S16LE. Known flags
are stream start `0x01`, stream end
`0x02`, and discontinuity `0x04`; reject unknown flags, invalid formats, malformed
lengths, and truncated payloads. Preserve the sequence, timestamp, flags, and
channel ordering in `LI_DS5_HAPTICS_PCM_FRAME`.

The callback frame and its `pcmData` are borrowed and valid only until
`ds5HapticsPcm` returns. Copy both metadata and payload before queueing them for
later rendering; the iOS bridge's `NSData` copy is required. The client owns
playback scheduling and handling sequence gaps/end/discontinuity. Standard rumble,
trigger rumble, adaptive triggers, motion, and LED callbacks keep their existing
independent contracts.

## Matching host upgrade and qualification

A Sunshine 3D host upgrade needs both actual device integration and transport:

1. Add a session-bound microphone UDP receiver, negotiated decryption, bounded
   Opus decoding/playout, and a real Windows microphone input route. Tie socket,
   decoder, device, and queued PCM cleanup to the owning session.
2. Add a virtual DualSense backend that receives game-authored audio. Preserve
   its haptics channels and feed bounded, session-owned PCM packets into the
   existing encrypted control stream. Advertising support must depend on a
   working configured backend; do not derive authored PCM from ordinary rumble.
3. Advertise the shared haptics marker and available PCM/IR flags, interpret the
   new client bits, and preserve existing SBS assignments. Require controller
   capability and session subscription before sending the optional feedback.

Qualify the core with actual dispatcher/transport tests for unsupported hosts,
both capability profiles, callback absence, disabled IR, encrypted and plaintext
microphone packets, malformed SETUP fields, cancellation, and concurrent teardown.
Exercise PCM packet limits, flags, truncation, metadata, and payload ownership.
Retain termination-race, stopping-inside-callback, Android build, and existing
SBS/telemetry regressions.

Host integration additionally needs real receiver-to-microphone capture and
game-authored PCM-to-controller checks, plus disconnect/reconnect and unavailable
backend cases. Parser golden vectors and a successful Android build cannot
replace those device checks. Darwin thread ownership, iOS timing, audio-session
lifecycle, and signed-app qualification remain requirements for that client's
later adoption; this shared-core publication does not perform the iOS switch.

The [native compatibility workflow](../.github/workflows/feature-compatibility.yml)
builds the production library and runs eight regression executables on Linux and
macOS. The thread ownership fixture uses real platform threads, including normal
return, direct thread exit, detached delivery, and creation failures. Run the same
checks locally with an installed C compiler, CMake, and OpenSSL development files:

```sh
cmake -S . -B build-features -DCMAKE_BUILD_TYPE=Debug \
  -DLC_BUILD_CONNECTION_TERMINATION_TESTS=ON \
  -DLC_BUILD_PLATFORM_THREAD_TESTS=ON -DLC_BUILD_MICROPHONE_TESTS=ON \
  -DLC_BUILD_AUTHORED_HAPTICS_TESTS=ON -DLC_BUILD_PACKET_SIZE_TESTS=ON \
  -DLC_BUILD_INPUT_STREAM_TESTS=ON -DLC_BUILD_AUDIO_QUEUE_TESTS=ON
cmake --build build-features --parallel 2
ctest --test-dir build-features --output-on-failure --timeout 30
```
