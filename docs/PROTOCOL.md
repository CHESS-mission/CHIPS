# CHIPS Protocol Specification

This document defines the CHIPS wire protocol: how frames are built, how they are delimited on a byte stream, and how a master and a slave exchange them. It specifies observable behaviour only. It does not prescribe an API, a scheduling model, a buffering strategy or how an application is notified; any implementation that behaves as described interoperates with any other. Sections marked *informative* give guidance and contain no requirements.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are to be interpreted as described in RFC 2119.

## 1. Overview

CHIPS is a request/response protocol between exactly one **master** and one **slave** over a reliable-ordering but possibly lossy and corrupting byte stream (UART, USB CDC, socket, radio link, ...).

- The master sends **requests**; the slave answers each with exactly one **response**.
- At most one request is outstanding at any time.
- Lost frames are recovered by the master **resending** the request; the slave recognises resent **duplicates** and answers them from a **response cache** without executing the request again.
- Frames are delimited by a SYNC byte, byte-stuffed, and protected by a CRC-16/KERMIT.

There is no addressing, no protocol version field, no flow control and no negative acknowledgement frame.

## 2. Frame format

### 2.1 Frame body

Before byte stuffing, a frame body is:

```
+--------+--------+---------------------+---------+---------+
|  SEQ   |  CTL   |  PAYLOAD            | CRC lo  | CRC hi  |
| 1 byte | 1 byte | 0..1024 bytes       | 1 byte  | 1 byte  |
+--------+--------+---------------------+---------+---------+
```

| Field | Size | Description |
|---|---|---|
| `SEQ` | 1 | Sequence number, 0..255 (see §4.1). |
| `CTL` | 1 | Bit 7: `RESP` flag (0 = request, 1 = response). Bits 6..0: command identifier `CMD`, 0..127. |
| `PAYLOAD` | 0..1024 | Opaque application data. |
| `CRC` | 2 | CRC-16/KERMIT over `SEQ`, `CTL` and `PAYLOAD`, transmitted **least significant byte first**. |

There is no length field. The payload length is the unstuffed body length minus 4.

| Constant | Value |
|---|---|
| Header size (`SEQ` + `CTL`) | 2 bytes |
| CRC size | 2 bytes |
| Maximum payload | 1024 bytes |
| Minimum body | 4 bytes (empty payload) |
| Maximum body | 1028 bytes |

All 128 command values are available to the application; the protocol reserves none and assigns them no meaning. A response carries the same `CMD` as the request it answers.

### 2.2 CRC-16/KERMIT

| Parameter | Value |
|---|---|
| Width | 16 |
| Polynomial | 0x1021 (0x8408 reflected) |
| Initial value | 0x0000 |
| Input reflected | yes |
| Output reflected | yes |
| Final XOR | 0x0000 |
| Check value (`"123456789"`) | 0x2189 |

The CRC is computed over the **unstuffed** bytes `SEQ`, `CTL`, `PAYLOAD`, in that order. Bit-wise reference (reflected form):

```
crc = 0x0000
for each byte b:
    crc ^= b
    repeat 8 times:
        crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
```

### 2.3 Byte stuffing

Two byte values are reserved on the wire:

| Name | Value | Role |
|---|---|---|
| `SYNC` | 0x7E | Frame delimiter. |
| `ESC` | 0x7D | Escape prefix. |

Every byte of the frame body (header, payload **and** CRC) equal to `SYNC` or `ESC` is replaced by a two-byte sequence:

| Body byte | Transmitted as |
|---|---|
| 0x7E | 0x7D 0x5E |
| 0x7D | 0x7D 0x5D |

No other byte is escaped. A transmitter MUST NOT emit `ESC` followed by anything other than 0x5E or 0x5D.

### 2.4 Delimiting

A transmitter MUST send a frame as:

```
SYNC  stuffed(body)  SYNC
```

Every frame carries both an opening and a closing `SYNC`, so two frames sent back to back appear as `... SYNC SYNC ...`. The worst-case wire size of a frame with an `N`-byte payload (every body byte escaped) is `2 + 2 × (4 + N)` bytes, i.e. 2058 bytes for a full payload.

### 2.5 Receiving

A receiver processes the stream as follows:

1. **Hunt.** Bytes are discarded until a `SYNC` is seen. That `SYNC` opens a frame.
2. **Collect.** Bytes are unstuffed into the body until the next `SYNC`.
3. **Close.** The `SYNC` that ends a frame also opens the next one. A receiver MUST therefore accept frames separated by a single shared `SYNC` as well as by `SYNC SYNC`.

A closed frame is handled as follows:

| Condition | Result |
|---|---|
| Empty body (`SYNC SYNC`) | Ignored silently. Any number of consecutive `SYNC` bytes is harmless. |
| `ESC` followed by a byte other than 0x5E / 0x5D | Invalid (escape error). |
| `ESC` immediately followed by `SYNC` | Invalid (escape error); the `SYNC` still opens a new frame. |
| Body longer than 1028 bytes | Invalid (too long). |
| Body of 1..3 bytes | Invalid (too short). |
| CRC mismatch | Invalid (CRC error). |
| Otherwise | Valid frame. |

On any invalid frame the receiver discards it together with every byte up to the next `SYNC`, which opens a new frame. Too-long and escape errors are detected as soon as the offending byte arrives; the rest of the frame is skipped. Invalid frames have no protocol effect; they are equivalent to a lost frame.

A receiver MUST tolerate arbitrary fragmentation of the stream: the outcome does not depend on how bytes are split across reads.

### 2.6 Example frames (informative)

| Description | Wire bytes |
|---|---|
| Request, seq 1, cmd 0x10, payload `01 7E 02` | `7E 01 10 01 7D 5E 02 FF DB 7E` |
| Response, seq 1, cmd 0x10, empty payload | `7E 01 90 51 8D 7E` |
| Request, seq 1, cmd 0x01, empty payload | `7E 01 01 51 08 7E` |
| Response, seq 1, cmd 0x01, payload `"PONG"` | `7E 01 81 50 4F 4E 47 50 F6 7E` |

In the first example, the payload byte 0x7E is escaped to `7D 5E`; the CRC is 0xDBFF, sent as `FF DB`.

## 3. Timing parameters

Timing is configured per link rather than fixed by the protocol. A master and its slave MUST be configured with the same values.

| Parameter | Meaning |
|---|---|
| `RESEND_PERIOD` | Length of a resend slot: the minimum time between the starts of two consecutive slots. MUST be greater than zero. |
| `MAX_RESENDS` | Number of copies the master may send after the first one, 0..255. |

Two durations follow from them:

| Derived value | Definition | Meaning |
|---|---|---|
| Nominal timeout | `(MAX_RESENDS + 1) × RESEND_PERIOD` | When the master gives up if it observes every slot boundary without delay (§4.2). Late observation only makes it later. |
| `CACHE_WINDOW` | `(MAX_RESENDS + 2) × RESEND_PERIOD` | Time after a response was produced during which the slave answers duplicates from its cache. |

`CACHE_WINDOW` covers every resend. Every copy of a request starts before the master gives up, about `(MAX_RESENDS + 1) × RESEND_PERIOD` after `T0` (§4.2). The slave can only produce the response after receiving the first copy, i.e. after `T0`. The cache therefore stays valid until at least `T0 + CACHE_WINDOW`, which covers every resend as long as the last copy reaches the slave within one `RESEND_PERIOD` of being started, minus any delay in the master's observation of slot boundaries.

`CACHE_WINDOW` MUST NOT exceed 2³¹ − 1 s (about 68 years), so that implementations can measure it with a clock whose seconds count wraps at 2³² (a 32-bit `tv_sec`).

All durations are measured on each side's own monotonic clock, in seconds and nanoseconds; the two clocks need not be synchronised. A clock that advances in whole ticks can make a measured duration up to one tick shorter than the real one. Correctness does not depend on the exact period (§3.1), and `CACHE_WINDOW` has one full period of margin, so this is harmless.

### 3.1 Choosing the resend period (informative)

`RESEND_PERIOD` SHOULD be at least one worst-case round trip, so that a resend is never sent while the answer to the previous copy may still be on its way. On a UART with one start bit, `D` data bits, `P` parity bits (0 or 1) and `S` stop bits per byte, at `B` bit/s:

```
RESEND_PERIOD >= 2 × 2058 × (1 + D + P + S) / B  +  APP_LATENCY
```

The first term is a request and a response of maximum size with every body byte escaped (§2.4). `APP_LATENCY` is everything that is not time on the wire: the slave's worst-case handling time, how long each side may take to notice received data, and transport buffering delays.

| Link | Wire time of one round trip |
|---|---|
| 115200 baud, 8N1 | 357.3 ms |
| 9600 baud, 8E2 | 5.145 s |

A shorter period costs nothing in correctness: duplicates are recognised (§5.2). It only wastes line time on copies that cross the response. `MAX_RESENDS` sets how many lost frames in a row a transaction survives.

The examples in this document use `RESEND_PERIOD` = 1 s and `MAX_RESENDS` = 2 (nominal timeout 3 s, `CACHE_WINDOW` 4 s).

## 4. Master

### 4.1 Sequence numbers

- The master keeps an 8-bit sequence counter, initialised to 0.
- Each new request uses the counter incremented by one, modulo 256. The first request after initialisation therefore has `SEQ` = 1; after 255 comes 0, which is a valid sequence number.
- All copies of one request carry the same `SEQ` and are byte-identical.

### 4.2 Transaction

A transaction starts at time `T0`, when the master starts transmitting the first copy of the request.

**Resend slots.** The master divides the transaction into slots and counts them; it does not compare elapsed time with a timeout.

- The first slot starts at `T0`.
- A new slot starts the first time the master observes that at least `RESEND_PERIOD` has elapsed since the start of the previous slot.
- The master counts the slots after the first. While that count does not exceed `MAX_RESENDS`, each new slot is a **resend slot**. The slot after the last resend slot is the **deadline**.

**Transmission.**

- The first copy starts at `T0`.
- Each resend slot starts a new copy, unless a copy is still being written. A copy being written is never restarted, and the slot is used up without a new copy. A slow transport therefore costs resends but does not delay the deadline.
- If the transport reports a write fault, the copy is abandoned; the next copy starts from the first byte at the next resend slot. Bytes already written stay on the line.
- No copy is started at or after the deadline. A transaction thus sends at most `MAX_RESENDS + 1` copies (3 in the examples: at `T0`, `T0 + 1 s`, `T0 + 2 s`).

**Reception.** The master decodes incoming bytes (§2.5) from the start of the transaction, starting in the hunt state. For each valid frame:

| Frame | Action |
|---|---|
| `RESP` = 0 (a request, e.g. a line echo of its own transmission) | Ignored. |
| `RESP` = 1, no copy of the request completely written yet | Ignored (stale). |
| `RESP` = 1, `SEQ` ≠ current sequence | Ignored (stale). |
| `RESP` = 1, `SEQ` matches, `CMD` ≠ request command | Ignored (command mismatch). |
| `RESP` = 1, `SEQ` and `CMD` match | **Accepted**; the transaction ends successfully. |

Ignored frames do not end the transaction; the master keeps resending and listening until the deadline.

**Deadline.** If no response has been accepted when the deadline slot starts, the transaction ends without a response. If the master observes every slot boundary without delay this is at `T0 + (MAX_RESENDS + 1) × RESEND_PERIOD`; a master that observes them late reaches it later, because each slot is only counted once it has been observed. A copy still being written is abandoned. A matching response that has been received by the time the master observes the deadline MUST be accepted, even if the master only examines it at that moment.

**After the transaction.** Once a transaction has ended (accepted, timed out or cancelled), the master discards all received bytes until it starts the next transaction, including bytes that immediately followed the accepted response. A late response to an earlier transaction that arrives during a later one has a different `SEQ` and is ignored as stale.

### 4.3 Outcomes

| Outcome | Condition | What it says about the slave |
|---|---|---|
| OK | A matching response was accepted. | The slave executed the request and this is its answer. |
| TIMEOUT | Deadline reached; at least one copy was completely written. | Nothing. The slave may or may not have executed the request. |
| SEND_FAILED | Deadline reached; no copy was ever completely written. | Nothing guaranteed: an incomplete copy can still be completed on the line (§6.3) and executed. |
| CANCELLED | The application stopped the transaction locally. No frame is sent. | Nothing. The slave may have executed, or may still execute, the request. |

Only OK carries information about the slave. The protocol provides no way to undo a request.

## 5. Slave

### 5.1 State

The slave is in one of two states:

- **LISTENING**: waiting for a request.
- **PENDING**: a request has been handed to the application, which has not yet answered or abandoned it. The application may take as long as it needs.

It also holds a **response cache** with at most one entry: the fingerprint of the last answered request, the response frame, and the time the response was produced.

A request's **fingerprint** is the triple (`SEQ`, `CMD`, `CRC`). Since the CRC covers `SEQ`, `CTL` and `PAYLOAD`, two requests have the same fingerprint exactly when they are (barring CRC collisions) byte-identical. A request with the same `SEQ` and `CMD` but a different payload is a different request.

### 5.2 Handling a valid frame

Before handling received data, the slave expires the cache: the entry becomes invalid once `CACHE_WINDOW` or more has elapsed since the response was produced. A duplicate arriving exactly at the window boundary is therefore treated as a new request.

For each valid frame, in order:

| Condition | Action |
|---|---|
| `RESP` = 1 | Ignored. |
| State is PENDING | Ignored, whatever the request (duplicate of the pending one or not). It is not queued; the master's resend recovers it. |
| Fingerprint matches the valid cache entry, and a copy of the cached response is currently being written | Ignored. |
| Fingerprint matches the valid cache entry | The cached response is transmitted again, from its first byte. The application is not involved. |
| Otherwise | **New request**: handed to the application; state becomes PENDING. |

The slave accepts any `SEQ` in any order. It does not check that sequence numbers increase.

Because the state becomes PENDING on delivery, at most one new request is delivered at a time; later frames are ignored until the application answers or abandons it.

### 5.3 Answering

When the application **responds** to the pending request:

- The response frame has `SEQ` and `CMD` of the request, `RESP` = 1, and the application's payload (0..1024 bytes).
- It replaces the cache entry. The fingerprint is the request's, and the timestamp is the time the response was produced (not the time the request arrived).
- Any copy of the previous cached response still being written is cut short (§6.3).
- The response is transmitted once. Further copies are only sent in reply to duplicates (§5.2).
- The state returns to LISTENING.

When the application **abandons** the pending request:

- Nothing is transmitted and the cache is left unchanged.
- The state returns to LISTENING.
- A resend of the same request is therefore not a duplicate: it is delivered to the application again as a new request (§6.4).

The application MAY also **invalidate the cache** at any time, e.g. after a mode change or reset that makes the cached answer wrong. A copy of the cached response being written is cut short (§6.3), and the next duplicate is delivered as a new request. A pending request is not affected.

### 5.4 Transmission

- The slave transmits only responses, and only in reply to a request (new or duplicate). It never sends unsolicited frames.
- If the transport reports a write fault, the copy is abandoned and not retried by the slave. The master's next resend is a duplicate, which triggers a retransmission from the cache.
- Cache expiry does not interrupt a copy being written.

## 6. Delivery semantics and known limitations

### 6.1 Execution guarantees

- If the slave answers (rather than abandons) and the cache is not invalidated, the application executes each request **at most once**, provided every copy reaches the slave within the cache window.
- An OK outcome means the request was executed and the response belongs to it.
- Any other outcome leaves the slave's state unknown to the master. Applications that need certainty MUST make their commands idempotent or query the slave's state afterwards.

### 6.2 Master reinitialisation within the cache window

The master's sequence counter restarts at 0 on every initialisation (first request: `SEQ` = 1). If the master is reinitialised (e.g. reboots) and its first request is byte-identical to the request the slave last answered, and it arrives within `CACHE_WINDOW` of that response, the slave treats it as a duplicate. It replays the cached response **without executing the request**. A typical trigger is a master in a reboot loop that sends the same status query at boot.

This is a known limitation. Applications for which it matters can avoid it by, for example:

- having the master wait at least `CACHE_WINDOW` after initialisation before its first request, or
- having the slave invalidate its cache when it detects a master restart by application-level means.

The 8-bit wrap of the sequence number is otherwise harmless: only the single latest response is cached, and consecutive requests from a running master always differ in `SEQ`.

### 6.3 Frames cut short

When a sender stops writing a frame midway (master cancellation or deadline, transport write fault, slave replacing or invalidating its cached response), the bytes already written stay on the line. The next frame's opening `SYNC` usually closes them into an invalid frame, which the receiver discards. If **only the closing `SYNC` was missing**, however, that opening `SYNC` completes a valid frame:

| Cut-short frame | Consequence if completed |
|---|---|
| Master request copy, after cancellation | The slave receives and may execute the cancelled request. |
| Master request copy, after a write fault | Harmless: later copies are duplicates. |
| Slave response replaced by a newer response | The master sees a response for an earlier `SEQ` and ignores it as stale. |
| Slave response cut by cache invalidation | A master still waiting for that request accepts it as its answer. |

### 6.4 No negative acknowledgement

The protocol has no NAK or error frame. If the slave abandons a request (for example an unknown command), the master receives nothing. It resends at every resend slot, and each resend is delivered to the slave application as a new request: up to `MAX_RESENDS + 1` deliveries in total (3 in the examples). The transaction then ends with TIMEOUT.

Slaves SHOULD NOT abandon requests to signal errors. They SHOULD answer unknown or failed commands with a normal response whose payload carries an application-defined error code, so that the master gets an OK outcome with a definite answer and the handler runs once. Abandoning is intended for requests the application chooses not to answer at all.

### 6.5 Half-duplex and echoing links

A master ignores request frames and a slave ignores response frames. CHIPS therefore works on links that echo each side's own transmissions back to it.

## 7. Example exchanges (informative)

All examples use `RESEND_PERIOD` = 1 s and `MAX_RESENDS` = 2.

**Normal exchange.**

```
master                                   slave
  | T0   REQ seq=5 cmd=0x02 "hello" ------>|  new request -> application
  |                                        |  application responds
  |<------ RSP seq=5 cmd=0x02 "hello"      |  cached (seq 5, 0x02, crc)
  | OK
```

**Lost response.**

```
master                                   slave
  | T0      REQ seq=5 ------------------->|  new request -> application, responds
  |           X<------------- RSP seq=5   |  response lost on the line
  | T0+1s   REQ seq=5 (resend) --------->|  duplicate -> replay from cache
  |<-------------------------- RSP seq=5  |  application not involved
  | OK
```

**Slow application.**

```
master                                   slave
  | T0      REQ seq=6 ------------------->|  new request -> PENDING
  | T0+1s   REQ seq=6 (resend) --------->|  ignored (PENDING)
  |                                        |  application responds at T0+1.5s
  |<-------------------------- RSP seq=6  |
  | OK
```

**Abandoned request (discouraged, see §6.4).**

```
master                                   slave
  | T0      REQ seq=7 cmd=0x55 ---------->|  new request -> abandoned
  | T0+1s   REQ seq=7 cmd=0x55 ---------->|  new request again -> abandoned
  | T0+2s   REQ seq=7 cmd=0x55 ---------->|  new request again -> abandoned
  | T0+3s   TIMEOUT
```
