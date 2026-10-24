# Session Controller

One control task owns platform requests, the process-wide RTC listener and the
AI/device/WeChat/H5/GROUP session generation. No SDK call runs on a UI, MQTT or
audio callback. `init()` requires the queues and audio callbacks, not an online
or bound device. Start waits for real platform ONLINE and a persistent identity.

UI/AT intents are accepted into a bounded queue, not returned as completed calls.
Platform payloads are copied before callback return: 8191 bytes per JSON,
12 KiB total and four queued items. Queue rejection is observable. HTTP terminal
delivery failures are drained from the platform's independent mailbox. They
remain failures and, where creation/release may have happened, trigger an
authoritative room query instead of discarding possible server-side effects.

| Owner | Start Barrier | TX / RX |
| --- | --- | --- |
| AI | WHIP and matched start_session with both alaw/8000/1 formats verified | 1 / 1 |
| Device callee | room token, P2P, local Subscribe and positive 0x2000 send | 10 / 10 |
| Device caller | accepted P2P and matching-room 0x2000, then local Subscribe | 10 / 10 |
| WeChat | correlated HTTP/MQTT room, WHIP and 0x2000 | remote subscription / checked PCMA |
| H5 audio | admitted inbound and local Subscribe | 10 / 11, explicitly reported in profile |
| GROUP | helper validates join and bidirectional g711a/8000/1 response | 1 / 1, local PTT gate |

AI/GROUP/device/WeChat TX receives a separate generation/stream business permit
only after the listed handshake; those protocols do not require an SDK remote
subscription callback. H5 TX requires the actual remote subscription. Local
Subscribe grants receive permission only. AI captions follow caption_type,
utterance_id and mode, and stop at UTF-8 boundaries with a truncation indicator.
Unsupported AI device_action requests receive an explicit JSON-RPC error.
AI/GROUP receive handlers accept their documented legacy bare words and the
existing S3 packed-word compatibility form. DEVICE/WeChat accept and hangup use
exact bare `0x2000`/`0x2001`; outgoing business words remain unchanged.

ENDING first revokes TX, requests media stop and waits for the actual media owner
to finish. It then cancels/disconnects and waits for CLOSED or a contract-verified
RETIRED event, including pending connect cancellation. DEVICE release HTTP waits
for the SDK pending gate and actual media owner to clear. A joined WeChat room
sends bare `0x2001` with reason 0 after the real media stop and before disconnect,
including outgoing calls not yet answered by the phone. Before room connection,
WeChat reject HTTP waits for real RTC retirement; failed rejection retains the
room, generation and first error until a real recovery edge permits confirmation.
Repeated intents and duplicate ONLINE notifications do not resubmit it.
Local prompt handoff
may report BUSY; actual media errors
end the session. Unexpected media termination during ACTIVE is a visible error.
An unconfirmed room cleanup retains owner/room and becomes ERROR. Dismissing the
UI cannot silently release those resources or approve/reject a contact request.
Ordinary hangup does not Stop/Start the process-wide SDK listener.

The profile advertises pure PCMA audio in all three scenes and no video. WeChat
uses the formal mini-program (wx_version_type=0). MQTT remains online during all
media paths. Metadata refresh and TLS/RTC concurrency require live RAM testing.
The existing SDK default HTTP discovery mapping is retained and documented;
this adapter does not claim all RTC traffic is authenticated TLS.

Maintenance admission is control-task-only, idle-only and closes inbound
admission before checking connection state again. New UI intents receive an
error during explicit Wi-Fi reconfiguration. Background metadata HTTP is deferred
without discarding pending refreshes; already submitted requests retain their
real completion semantics. Releasing maintenance restores listening and deferred
refreshes. It never pauses MQTT or changes identity.

The host test compiles this real source with mock platform, RTC and media
boundaries. It covers strict AI format negotiation, copied JSON, real media-stop
barriers, cancelled HTTP room creation, DEVICE room identity, WeChat answer
barrier, failed cleanup ownership and negative 16 kHz negotiation. It is not
hardware, server, physical audio, weak-network or long-run evidence.
The joined-room phone cancellation and rejection ordering changes still require
their own hardware/server regression.
