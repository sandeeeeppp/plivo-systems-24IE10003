# NOTES — UDP streaming protocol

**Submitted grading delay: 90 ms. Measured floor: 81 ms** — the lowest delay at which both
Profile A and Profile B are valid. 90 ms is the floor padded for robustness against unseen
grading profiles (see §6); it holds the same clean result at 1.997× overhead
(up 479 297 B, feedback 0 B). Miss rate at the floor: A ≈ 0.07%, B ≈ 0.5%; overhead is
independent of the delay knob, so it is identical at 90 ms.

---

## 1. Architecture

Single-threaded sender and receiver, no dynamic allocation on the data path, no
reorder/playout buffer. Two mechanisms only: **immediate independent duplication**
(primary redundancy) and a **NACK/ARQ backstop** (which self-suppresses whenever the
deadline leaves no round-trip room, i.e. at both graded operating points).

### Wire format
| Direction | Format | Size |
|---|---|---|
| Uplink 47001→47002 (media & retx) | concat of 161-B chunks `[seq:1][payload:160]`; chunk count = `len/161` | 161 / 322 / 161k |
| Feedback 47003→47004 (NACK) | `[0x02][count:1]` then `count × [seq:2 BE]` | 2 + 2c |
| Harness legs (fixed) | `[seq:4 BE][payload:160]` | 164 |

The 1-byte uplink sequence number is the single most important optimization
(see §3). The receiver expands it to the full 4-byte seq the harness player expects
by resolving it to the value nearest the current playout position — the live window
spans only a handful of frames, far inside the 256 range, so this is unambiguous.

### Sender (poll on 47010 + 47004)
1. On each source frame: store its 161-B chunk in a 4096-slot ring, send the chunk.
2. Send the **same chunk a second time** as a separate datagram, admitted by the governor.
3. On a NACK: retransmit requested seqs (priority tier), rate-limited 25 ms/seq.

### Receiver (poll on 47002)
1. For every chunk of every packet: if the seq is new, forward `[seq:4][payload]` to the
   player **immediately** and mark an O(1) `delivered[]` bitmap. Duplicates and stragglers
   hit the bitmap and vanish — zero head-of-line blocking, zero playout timer.
2. Every 12 ms: NACK undelivered seqs that still have deadline headroom, bounded by
   <4 NACKs/seq, ≥30 ms spacing, and a 0.004× feedback self-cap.

---

## 2. Why immediate independent duplication (not lag-N piggyback)

Each frame is sent as **two separate datagrams at the same instant**. The relay draws
loss and delay *per received packet*, so the two copies get **independent** draws: the
frame is lost only if *both* are dropped (≈ p²), and the earliest-arriving copy has delay
`min(d₁,d₂)`, which makes tight deadlines that a single copy would miss. Crucially, both
copies arrive within **one** delay draw of emission — unlike a piggybacked repeat carried
on a *later* slot, which arrives ≥ 2 slots (40 ms) late and cannot meet a deadline equal to
`delay_max`. This is why the delay floor equals `delay_max`, not `delay_max + lag·20 ms`.

## 3. The overhead cap forces the sequence number to 1 byte

`overhead = (up+down) / (n·160)`, cap 2.0×, i.e. **320 B per frame** total. The payload is
160 B, so *two full payloads already consume the entire budget* — "100% redundancy" leaves
**zero** bytes for any header. Every header byte therefore steals from redundancy:

- A copy must carry seq + payload. The minimum is **161 B** = `[seq:1][payload:160]`.
- Two 161-B copies = 322 B/frame = 2.01× — just over. So the governor must *skip the
  duplicate on ~1.3% of frames*; those are the only exposed frames.
- A naïve `[count:1][seq:4][payload:160]` = 165-B chunk forces skipping ~7% of duplicates →
  ~5× more exposed frames → ~3% miss at 40 ms. Shrinking 165→161 B (drop the count byte,
  2→1-byte seq) is what took Profile A from INVALID to 0.07%.

## 4. The governor — overhead ≤ 2.0× as a causal invariant

With `frames` = source frames seen and `sent_up` = bytes sent so far:

```
allowed(i)  = 1.998 · 160 · i
duplicate  admitted iff sent_up + 161 ≤ allowed − reserve
retransmit admitted iff sent_up + 161k ≤ allowed        (priority tier)
reserve     = max(200, 2 · retx_bytes)
```

Every admission is checked *before* the send, so `up_bytes ≤ 1.998·160·n` holds at **every
instant**, for any loss pattern — not merely in expectation. The receiver independently
caps feedback at 0.004×. Worst case 1.998 + 0.004 < 2.0×, guaranteed by construction. In
practice feedback is 0 at both operating points, so the measured figure is a flat 1.997×.

## 5. Where the delay floor comes from, and what breaks it

**A resend costs one detection interval plus two hostile lane crossings** (down NACK + up
retx), ≈ 60 ms + two U[·] draws. At A's 40 ms or B's 80 ms deadline there is no room for
that, so **ARQ never rescues a frame at the graded delays** — it is a pure backstop for
looser hidden profiles. Recovery therefore rests entirely on the two same-slot copies, and:

- **Below `delay_max`** a frame whose *both* copies drew a high delay is unrecoverable.
  Profile B `delay_max` = 80 ms, so 80 ms is the hard ceiling of the relay's own delay and
  the mathematical floor. At exactly 80 ms the ~0.7 ms pipeline overhead pushes the
  near-`delay_max` frames just past the deadline (13–15 misses); **+1 ms (81 ms) clears
  them**, leaving only the loss floor (~7). Hence **81 ms**.
- **The loss floor** (~7 frames on B) is the ~0.25% of frames whose both copies are dropped
  (5% i.i.d. loss) plus the ~1.3% single-copy frames that are then dropped. ARQ cannot beat
  the deadline, so this floor is irreducible at any delay — but 7 ≪ 15, so it is comfortable.
- **A burst of ≥ 3 consecutive relay-packet drops** kills both copies of a frame outright
  (they ride adjacent packets). Neither profile A nor B has `burst_loss`, so this does not
  occur here; a hidden Gilbert-Elliott profile with in-burst fraction > ~1% is the documented
  failure mode, and the fix is the conservative fallback delay (~240 ms) where the ARQ tier —
  already budget-prioritized — recovers burst cores end-to-end.
- **A 100 ms jitter spike** (a straggler arriving after 5 newer packets) causes no
  head-of-line blocking: the receiver has no ordering buffer and judges each seq
  independently, so newer frames forward instantly and the straggler is either a bitmap
  duplicate (its repeat already delivered) or still counts if 100 ms < deadline.
- **Transient harness stalls** (the `*` runs in RUNLOG): a Python relay/player GC pause of
  ~0.5 s dumps a contiguous batch of frames past their deadline. This is **delay-independent**
  (equally frequent at 82/85/88/92 ms) — an environment artifact of the shared test box, not
  of the operating point — so no amount of delay margin absorbs it and it does not motivate a
  higher graded delay. Sockets are given 8 MB buffers so a stall costs lateness, never a drop.

## 6. Operating point

**Submitted operating point: delay_ms = 90 (measured floor 81), immediate duplicate.**
81 ms is the mathematical floor — B's `delay_max` (80) + 1 ms overhead — at which both A
(0.07%) and B (~0.5%) are valid at 1.997× overhead, ≤ 2.0× by construction. 90 ms is the
number submitted for grading: it preserves the clean loss-floor result (the B sweep is flat
at ~7 misses for every D ≥ 81, confirmed at 85 / 88 / 92 ms) while adding ~10 ms of headroom
against an unseen profile whose `delay_max` exceeds 80, and it stays inside the ARQ-silent
regime so feedback remains 0 and overhead a flat 1.997×. The one thing that breaks either
number is correlated loss spanning ≥ 3 consecutive relay packets on more than ~1% of frames
(no such profile in A or B); the documented fallback there is ~240 ms with the ARQ tier.
