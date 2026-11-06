# PCMA RTC Adapter

One control task owns all SDK calls and polls events. Register the PCMA receiver
before Start. `SYS_STARTED` may occur before Start returns. SDK callbacks only
copy control payloads, update bounded state and call a nonblocking receiver.
The receiver must copy the 160-byte payload before returning.

Audio uses A-law, 8000 Hz, mono, 160 bytes / 20 ms. TX needs either a real remote
subscription or an explicit generation/stream business permit issued only after
the AI/GROUP/WeChat/device protocol handshake. H5 uses remote subscriptions only.
The two masks are separate; business permission never fabricates a subscription.
Local Subscribe only grants receive permission. No Opus codec or video path
is added. The caller chooses stream 1 for AI and stream 10 for device/H5 audio.

RX packet boundaries are independent of the 20 ms playback block. Nonempty
encoded PCMA chunks are reblocked in byte order, retaining at most 159 bytes for
the single locally subscribed receive stream. This accepts 80-byte/10 ms input,
arbitrary splits and larger aggregates without padding or discarding a normal
tail. A successful stream change, matching unsubscribe, cancellation, connection
error or retirement discards the old partial tail. Other streams never fill or
clear the selected stream's carry. Remote TX subscriptions remain separate.

The fixed official Web SDK uses `Date.now()` per recorder processing batch,
queues encoding asynchronously and splits G.711 into up to 640-byte chunks with
the same timestamp. Those wall-clock values do not establish sample continuity,
so the adapter does not reject gaps or regressions as codec errors. Output PTS
annotates the block's first source byte, advancing 20 ms for subsequent blocks
within the callback, with uint32 wrap. The current player ignores PTS. There is
no G.711 PTT-start/stop marker in that Web path: a partial tail can consequently
span a pause until the next bytes or an explicit subscription/lifecycle boundary.
No arbitrary wall-clock timeout is presented as a verified PTT boundary.

Each SDK callback does at most eight nonblocking receiver calls, matching the
product's eight-block playback queue. Receiver rejection, concurrent callbacks
or excess callback work discard their affected bytes visibly, never replay them
later. `rx_dropped` counts complete 160-byte blocks; `rx_bytes_dropped` counts
all discarded encoded bytes, `rx_partial_dropped` counts discarded tail bytes,
`rx_unselected_frames` counts packets outside the owned receive subscription,
and `rx_carry_bytes` reports its current partial length. Codec/rate/mono, nonempty
payload and stream range remain checked. The receiver is never called under the
adapter lock; generation and subscription epochs prevent an old callback from
clearing a new owner's carry. The unlock-to-receiver interval is not an atomic
control-epoch barrier: a same-generation unsubscribe/regrant can race one already
admitted complete block. The current receiver checks generation/stream, not this
private subscription epoch. Current business flows subscribe once per session
and retire the generation when stopping; general same-generation unsubscribe
does not promise to retract an in-flight receiver call.

The [official AI guide](https://docs.tange.ai/products/ai-chat/guides/device-integration.html)
requires streaming downlink playback by the negotiated format, and only gives a
100 ms recommendation for *uplink*. It does not mandate a 160B downlink packet or
a 100 ms downlink limit. The
[WeChat guide](https://docs.tange.ai/products/wxvoip/guides/device-integration.html)
also demonstrates 320B/40ms PCMA. Callback/queue backpressure above is a local
bounded-latency policy, not a claimed server packet-duration contract.

WHIP and ordinary P2P each own copied peer/token inputs until failure or CLOSED.
Pending cancel latches the generation and closes late success from the control
task. Inbound connections require an explicitly armed nonzero generation. A
closed handle is an identity in the event only and is never dereferenced later.

Application command APIs preserve the exact wire word and positive SDK sent-byte
results. Legacy AI/WeChat/device/GROUP words must not be globally re-encoded as
logical IDs. Command events retain the original word and separately expose SDK
ID/SN/response-bit extractions for protocols using packed words. In particular,
bare legacy `0x2001` is a hangup word, not an SDK response to `0x2000`.
Subscription calls normalize the SDK's nonnegative sent-byte result to zero,
while retaining negative errors.

Queues are static: 8 control events (with 511 inline payload bytes each)
and 5 audio packets. Larger commands use owned allocations: at most 12 KiB per
command and 16 KiB total. Poll consumers call `xmini_rtc_event_release()` after
use. Oversize, allocation failure and budget exhaustion are visible errors,
never truncation. The static queues consume about 5.5 KiB plus queue controls on
ESP32-C3, with no new task stack. The current diagnostic product uses a 10 KiB control
task. The SDK5/16 KiB probe observed maximum use of 6364B on A and 6596B on B
(HWM 10020B/9788B); the SDK5/10 KiB DEV probe then observed an actual 3644B minimum
remaining on its exercised path.
This is not full-path stack acceptance: WeChat/GROUP/H5 and exceptional paths
still need high-water and overflow checks with the smaller stack. SDK candidate 5 moves the
8192-byte WHIP offer to connection-owned, on-demand heap storage; the exact C3
compiler reports a 64-byte WHIP entry frame instead of the old 8256-byte internal
frame. The smaller diagnostic stack is based on both compiler and observed board
evidence, and remains subject to further board tests. Offer storage overlaps the
reserved stack during setup.
SDK network, TLS and retransmission allocations still require live measurement.

The consumed SDK diagnostic-2 retains candidate 6's private borrowed diagnostic hook. Only the
session thread reads its TX history, RX store/jitter, ingress queue and pacer;
the adapter bounded-copies the latest TX/RX/QUEUE records into 204B fixed storage.
`xmini_rtc_transport_diagnostics()` returns copies, never live SDK lists or payload
pointers. A zero capture sequence means unavailable (including the older SDK5).
Each record retains the application owner generation/connection at capture and
the opaque lower session identity, kind and stream; compare all of them before
comparing counts across snapshots. Cleanup retains the last original record.

Payload-byte snapshots count retained segment allocation requests: TX includes
the reserved 42-byte fragment prefix and RX does not. TX pending and QUEUE
auxiliary counters preserve their logical payload-byte unit without that prefix;
RX auxiliary counters are its jitter queue. Queue main counters cover the queued
head's kind/stream, auxiliary covers all ingress, and pacer counters are session-wide.
The hook performs no printf/allocation/SDK call and never retains its borrowed DTO.
Exact private header hash, ABI and archive inputs are in the SDK provenance lock.
Private version2 adds per-RX-stream `rx_allocation_failures` and
`first_failed_frame_id`, preserving the first receiver-visible allocation failure
even after later inputs or polls. It is not exhaustive accounting of nested SDK
allocations. RX frame ID tracks parsed arrival, including allocation rejection;
failure and 320 ms poll snapshots no longer depend on successful completion.
Poll captures the largest retained frame-count stream (or latest arrival if empty),
so empty DATA cannot hide a retained AUDIO store. Kind/stream/identity still scope
every record. No receive retention, jitter/NACK or failure behavior is corrected
by this diagnostic-only archive.

Queue overflow latches a visible fault, disables new media and requests one SDK
stop from the control task. Failure is not returned as success or automatically
restarted. Stop completion must be observed before Uninit; ordinary hangup uses
Disconnect without stopping the process-wide listener. Queued audio older than
100 ms is dropped with a counter, and unsubscribe invalidates queued packets so
a quick resubscribe cannot revive old speech.
Unsubscribe also revokes business permission, and reauthorization cannot revive
packets queued before revocation. First format/gate failures record metadata,
never audio payloads, without weakening codec/rate/packet validation.

`tests/test_rtc.c` compiles the actual adapter with the delivered headers and
mock queues/SDK. It covers synchronous lifecycle callbacks, copied async inputs,
pending cancel, inbound generations, both connect modes, subscriptions, bounded
queues, wrong codec, payload copies, stale handles and overflow. A 9 KiB command,
aggregate-budget exhaustion and release byte accounting are covered too. Tests do
not execute the RISC-V archive or prove FreeRTOS concurrency and live media.
The SDK mocks return real five-byte subscription and six-byte command successes;
command tests cover unchanged bare/packed words, SN and response-bit preservation.
RX tests reproduce the observed 320B and 80B failures before their respective
changes. The actual-source matrix checks byte-exact splits, wall-clock gaps,
uint32 PTS wrap, aggregates, pressure accounting, owned-stream and generation
boundaries, malformed codecs/flags, stop/restart and zero callback allocations.
The C3 compiler measures a 180-byte assembler plus 16 bytes of status fields;
the direct callback frame remains 672 bytes, matching the prior source.
Final linked padding, full call-chain stack use and the 80B browser trigger still
need board verification. These are adapter host checks, not playback proof.
Snapshot tests cover initialization gates, strong-hook bounded copies, independent
stages/streams, invalid versions, generation/identity and retained cleanup evidence.

Hardware SDK5/10 KiB passed two boards times 20 short AI reply rounds after the
aggregate split. Three ordinary DEV P2P ACTIVE rounds still exhausted heap beyond
64 transmitted frames; its cause is not established. Candidate 6's representation
savings and snapshots must not be presented as resolving that failure before the
original board trigger is repeated. The complete lower host suite has 18 passes
and nine inherited multi-segment audio ordering assertion failures in diagnostic-2,
also present in candidate 5 and 6, separately documented rather than hidden or
claimed passing. A separate frozen-SDK6 finite-heap matrix retains six real RX
retention/stall failures and 12 control passes; these host failures are not a
completed hardware root-cause fix. The original board trigger still needs fresh
arrival/poll/store evidence.
