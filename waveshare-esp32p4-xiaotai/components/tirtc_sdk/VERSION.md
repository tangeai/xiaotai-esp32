# TiRTC SDK Version

| Item | Value |
| --- | --- |
| Package channel | 2.3.0 P4 validation rebuild with transport and RTC-thread stack fixes |
| TiRTC version | 2.3.0 |
| Target | ESP32-P4 / FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| Toolchain | riscv32-esp-elf-gcc-14.2.0_20260121 |
| Nano source | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` with P4 TGWebRTC archive `2229474ecb8a03ab2a7144acb529decd7dc206f328dd4ecdbb1ef308a691a86d` |
| TGWebRTC source | `e39114731ad488c88573d16f0855a1326d97c989` plus validation patch set `e5b3109cc0dee3f0d8958c23a60f69b236d87acb909cac95c4d6bb24812dbbaf` |
| Official archive SHA-256 | `6daa39e04edf552283360f6a7defa6d12de8c8dd8d8094f8a6bbbdbb64a3f190` |
| Pre-strip library SHA-256 | `738c969244ab39c2b0eacc21068ecebc9bad736a4a5d713794836605d8e9f982` |
| Integrated library SHA-256 | `a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451` |
| Build date | 2026-08-31 |
| Debug-info sanitization date | 2026-09-02 |

The integrated P4 archive was processed with `riscv32-esp-elf-strip --strip-debug`.
All 99 members retain identical allocated section contents, live symbols,
relocations and archive symbol lookup. No source rebuild or runtime data edit
was performed. The original archive remains in the prior local source Tag;
source-line debugging inside the SDK requires that original archive.

RTC endpoints must remain HTTPS. This archive contains required certificate
verification, the ESP-IDF CA bundle, hostname verification and verification-result
checks. Application code must propagate connection errors without HTTP fallback.

## Build Contract

- `CONFIG_FREERTOS_HZ=1000`
- FreeRTOS trace facility disabled
- FreeRTOS stats formatting functions disabled
- FreeRTOS runtime stats disabled
- `sizeof(StaticSemaphore_t)=84`
- `CONFIG_LWIP_MAX_SOCKETS=16`
- `libwebrtc_nosctp.a` is bundled into `libTiRTC.a`
- ESP32-P4 and ESP32-S3 libraries are not interchangeable

The rebuild uses this application's generated ESP-IDF configuration. The
application uses ESP32-C6 Wi-Fi over ESP-Hosted SDIO, so it keeps
`CONFIG_ESP_HOST_WIFI_ENABLED=n`, `CONFIG_ESP_WIFI_REMOTE_ENABLED=y`, and
`CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y`.

## P4 Compatibility Patch

- Use the ESP-IDF 5.5.4 P4 register include directory
  `soc/esp32p4/register/hw_ver1`.
- Bound one ICE UDP receive callback to at most 8 datagrams or 4 ms, then ask
  the RTC poll loop to run again without waiting. This prevents a continuously
  readable media socket from monopolizing the RTC thread while retaining all
  packets in the socket queue for subsequent polls.
- Bound one TGTRP audio-jitter poll to at most 4 ordered work items. When a
  missing frame repairs a queued run, the remaining ready frames continue on
  the next 1 ms scheduler turn instead of being released synchronously in one
  socket callback. Ordering and loss accounting remain unchanged.
- Preserve a slow-path diagnostic when one receive callback still takes at
  least 100 ms: `[ICE_RX_DIAG]` reports packet count, bytes, receive time,
  `ice_agent_input()` time, media callback time, and whether the fairness
  budget yielded. Nested `[TGTRP_*_DIAG]` records separate segment parsing,
  frame completion, jitter polling, assembly, and application callback costs.
- Run only the P4 RTC receive/assembly task at priority 17. The audio capture
  task remains above it, while the H264 decode worker remains below it. This
  prevents every queued frame in a jitter-buffer recovery burst from waking
  the decoder and preempting the RTC task before the remaining frames can be
  delivered.
- After the P4 ICE receive callback consumes its packet/time budget, yield the
  RTC task for one 1 ms tick. No fixed delay is added to ordinary one-packet
  traffic. This gives CPU0 IDLE and downstream workers a bounded scheduling
  window without changing the normal receive cadence.
- Keep TGTRP NACK batch scratch memory on the connection heap instead of the
  RTC task stack, preventing stack-protection faults during severe loss bursts.
- Pass the real NACK scratch capacity after that storage became a pointer.
  Using `sizeof(pointer)` limited the encoder to 4 bytes on P4, so every real
  frame-NACK request failed with `TGTRP_ERR_BUFFER_TOO_SMALL (-7)` and recovery
  fell back to the much slower high-RTT retransmission timeout path.
- Keep normal TGTRP diagnostics at a 10-second maintenance cadence while
  retaining one-second warnings when receive progress is genuinely stalled.
- Keep recovery probes bounded by clean loss, delay, RTT, and sender-backlog
  gates. A static or quantizer-limited encoder no longer stalls recovery
  forever; the controller retries after 3 seconds, raises a clean-window target
  by at most 40 percent, and caps the minimum-bitrate recovery hold at 12
  seconds. Congested feedback cannot consume a pending recovery probe.
- Replace the TURN allocation binary-search keys with lightweight address keys.
  This reduces the P4 stack frames of the relay and address lookup functions
  from 3792 bytes to 32 and 48 bytes without changing lookup ordering.
- Keep the P4 signal RTC thread at 8 KiB and reserve 12 KiB for connection RTC
  threads. The larger connection stack is a validation margin in addition to
  the TURN lookup root-cause fix.

## Bitrate Adaptation Contract

- Register `TiRtcConnSetVideoBitrateParams()` after a TGTRP connection is
  accepted and before sending the video stream.
- Valid ranges satisfy `0 < min_bps <= start_bps <= max_bps`.
- `TIRTCCALLBACKS.on_update_bitrate()` supplies an absolute target bitrate in
  bps. The callback runs on an SDK thread, so the application posts the target
  to its media-control task and returns immediately.
- The callback is a recommendation. The application remains responsible for
  applying the target to its encoder, frame rate, or resolution policy.
- The API is TGTRP-specific in this SDK. Unsupported transports or invalid
  parameters return `TIRTC_E_INVALID_PARAMETER`.
- SDK bitrate feedback is enabled by default through
  `CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE`; the separate legacy local automatic
  downgrade policy remains disabled so only one controller adjusts the encoder.
