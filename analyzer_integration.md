# Analyzer Integration Spec — Full-Function Bench Tool on One Wire

_Everything an external tool (the MCU logic analyzer) needs to fully replace
the debug terminal: decode the packet stream, classify every code, drive the
console, and parse every reply. Written for a MACHINE client — echo handling,
pacing, and state transitions a human at a terminal never notices are spelled
out. Reply grammars below are extracted verbatim from `app/uart_cmd.c`
(firmware source of truth); wire timing is normative in `link_protocol.md`._

---

## 1. Roles and modes — model this first

One physical wire (transmitter P1.0 ↔ analyzer), **9600 baud 8N1**, carrying
exactly one of two things at any moment (strict mutual exclusion in firmware):

| Line state | What's on the wire | Analyzer behavior |
|---|---|---|
| **PACKET MODE** (power-on default; the only mode of production firmware) | Binary packets, ~every 40 ms, nothing else | Decode passively (§3). Analyzer TX must stay idle/high-Z |
| **CONSOLE MODE** (debug firmware, after `CONSOLE UNLOCK`) | ASCII console traffic only — **the packet stream is suspended** | Send commands, parse replies (§4–§5) |

The analyzer must track which mode it believes the line is in, and expect the
transitions in §4. A tool that decodes packets while blindly assuming the
stream never stops, or that sends commands while packets are flowing, will
misbehave.

## 2. Electrical

- Transmitter TX: push-pull, ~5 V logic, idle high. **High-Z while the
  transmitter is in reset** — the line's idle-high pull during reset comes
  from an external pull-up (harness requirement), not the MCU.
- Analyzer TX onto the shared line: drive it **only while deliberately
  sending a command**; otherwise release/high-Z (never fight the line —
  downhole this wire belongs to the logger).
- Analyzer baud tolerance: transmitter is crystal-derived, +0.005 % error.

## 3. Decoding the packet stream

### 3.1 Frame format

```
|0x7F| >2ms gap |MSB|LSB|CHK|   ...>20ms idle...   repeat
CHK = (~(MSB + LSB)) & 0xFF        (additive checksum — NOT a CRC)
16-bit value = (MSB << 8) | LSB    (big-endian)
```

Timing envelope (what a healthy transmitter produces — flag violations):

| Measurement | Healthy range |
|---|---|
| Sync stop-end → data start (gap) | 2.9 – 4.9 ms |
| Sync start → checksum stop-end (packet) | ≤ 9.2 ms (spec < 10) |
| Data bytes | back-to-back, no inter-byte gaps |
| Checksum end → next sync (idle) | ≥ 21.8 ms guaranteed (~31 ms nominal; see verification_guide.md idle-floor note) |
| Sync-start → sync-start (period) | 40 ms nominal |
| Max legitimate gap between valid packets | ≈ 75 ms (fenced stall) — anything longer, see staleness |

### 3.2 Decode algorithm (reference C — port as-is)

**Rules that must survive the port:** `0x7F` is LEGAL in MSB, LSB, and CHK
positions (e.g. code 127 = `7F 00 7F 80`, code 383 = `7F 01 7F 7F`) — never
re-sync mid-frame on a payload `0x7F`; on checksum failure slide ONE byte and
re-search (never blindly stride 4-byte groups); a consecutive-valid-frame
streak is a UI confidence hint, never a parsing rule.

```c
#include <stdint.h>

typedef struct {
    uint8_t  win[4];
    uint8_t  len;
    uint16_t code;            /* valid when feed() returns LD_FRAME       */
    uint8_t  text;            /* valid when feed() returns LD_TEXT        */
    uint32_t frames_ok;
    uint32_t checksum_errors;
    uint16_t streak;          /* consecutive valid frames (confidence)    */
} link_dec_t;

enum { LD_NONE = 0, LD_FRAME = 1, LD_TEXT = 2 };

static void link_dec_init(link_dec_t *d) { d->len = 0; d->streak = 0;
    d->frames_ok = 0; d->checksum_errors = 0; }

/* Feed one received byte; returns at most one event per call.            */
static int link_dec_feed(link_dec_t *d, uint8_t b)
{
    d->win[d->len++] = b;
    if (d->len < 4) return LD_NONE;
    if (d->win[0] != 0x7F) {                      /* searching for sync   */
        d->text = d->win[0];
        d->win[0]=d->win[1]; d->win[1]=d->win[2]; d->win[2]=d->win[3];
        d->len = 3;
        return LD_TEXT;                           /* console text / noise */
    }
    if ((uint8_t)(~(d->win[1] + d->win[2])) == d->win[3]) {
        d->code = (uint16_t)((d->win[1] << 8) | d->win[2]);
        d->len = 0; d->frames_ok++; d->streak++;
        return LD_FRAME;
    }
    /* bad checksum: this 0x7F was payload/noise — slide one byte         */
    d->checksum_errors++; d->streak = 0;
    d->text = d->win[0];
    d->win[0]=d->win[1]; d->win[1]=d->win[2]; d->win[2]=d->win[3];
    d->len = 3;
    return LD_TEXT;
}
```

`LD_TEXT` bytes ARE the console output when the line is in console mode —
route them to the analyzer's terminal display. (Printable ASCII can never
contain `0x7F`, so console text can never fake a sync byte.)

Executable reference + full adversarial test vectors: `host_ui/link_decoder.py`
and `host_ui/test_link_decoder.py` (mid-join, dropped/inserted byte,
corruption per position, 0x7F cases, chunked feeds).

### 3.3 Code map — classification, display, severity

| Code (hex) | Meaning | Suggested display | Severity |
|---|---|---|---|
| `0x0000`–`0x2710` (0–10000) | Pressure, **0.1 bar/LSB** → `bar = code / 10.0` | `123.4 bar` | normal (green) |
| `0xFF01` | `NO_READING` — boot, no sample yet | `BOOT / NO READING` | info (amber) |
| `0xFF02` | `UNCAL` — no valid calibration | `UNCALIBRATED` | warning (amber) |
| `0xFF03` | `DISAGREE` — probes disagree beyond threshold | `FAULT: PROBE DISAGREE` | fault (red) |
| `0xFF04` | `ADC_STALL` — ADC conversions dead | `FAULT: ADC STALL` | fault (red) |
| `0xFF05` | `VDDEXT` — excitation rail unstable | `FAULT: EXCITATION` | fault (red) |
| `0xFF06` | `OVER_RANGE` — computed > 1010.0 bar | `OVER RANGE (>1010 bar)` | fault (red) |
| `0xFF07` | `UNDER_RANGE` — computed < −5.0 bar | `UNDER RANGE (<-5 bar)` | fault (red) |
| anything else (10001–0xFEFF, other 0xFFxx) | never transmitted | `INVALID 0x....` | error (red) — counts as a decode anomaly |

Priority (one code per packet; a higher fault hides lower ones on the wire —
`STATUS` in console mode lists ALL active causes):
`ADC_STALL > VDDEXT > DISAGREE > UNCAL > range codes > pressure`.

### 3.4 Staleness — mandatory display rule

**No checksum-valid frame for ≥ 500 ms → display `NO DATA / STALE`** (do not
keep showing the last value as if live). Legitimate gaps never exceed ~75 ms;
a silent line means a dead/resetting transmitter, a cut wire — or simply that
someone (you) opened the console and the stream is suspended. Distinguish the
last case by tracking your own mode (§1).

## 4. Console session state machine (machine-client view)

```
LOCKED ──"CONSOLE UNLOCK\r\n"──▶ UNLOCKING ──▶ UNLOCKED (console mode)
  ▲                                                  │
  └──── power cycle ── 5-min RX inactivity ── "CONSOLE LOCK\r\n" ◀┘
                    (RESUMING: ≥22 ms quiet → single packet → 40 ms cadence)
```

**LOCKED (boot state, always):**
- Zero ASCII from the transmitter — packets only. No banner at boot, ever.
- Every line you send is silently ignored (no echo, no error) EXCEPT the
  exact line `CONSOLE UNLOCK` (case-insensitive, `\r` or `\n` terminated).
- Production firmware never leaves this state → **build identification:**
  send `CONSOLE UNLOCK`; if the stream never pauses and no banner arrives
  within ~1 s, it's a production build (or wrong baud).

**UNLOCKING:** on receipt of the unlock line, the in-flight packet COMPLETES
(expect up to one more full packet), then the stream stops, then the banner
arrives:

```
\r\n== Pressure Transmitter v2 (digital link) ==\r\n
Console UNLOCKED — packet stream SUSPENDED (CONSOLE LOCK to resume)\r\n
Boot RST 0x<hex4> WFS 0x<hex4>[ [WDT1]][ [POR]][ [PIN]]\r\n
[WARN: NVM data flash inconsistent (saves disabled)\r\n]     ← only if unhealthy
```

`[WDT1]` in the banner = the last reset was a watchdog reset — surface this
prominently.

**UNLOCKED (console mode) — the three rules of parsing:**
1. **ECHO IS ON.** Every printable byte you send comes back before the reply.
   After sending `STATUS\r\n` you will receive `STATUS` (your echo), then
   `\r\n`, then the reply lines. Strategy: strip the first line if it equals
   the command you just sent, or discard N echoed bytes as you transmit.
2. **One command per firmware loop pass.** Send one line, wait for its
   complete reply before the next. Do not stream a batch blind.
3. Case-insensitive, trailing spaces trimmed, blank lines ignored, backspace
   (`0x08`/`0x7F`) edits (echoed as `\b \b`) — as a machine, just don't send
   them. **Do not send more than ~1 KB without draining replies** (TX ring
   drops on overflow).

**Re-lock paths:** your `CONSOLE LOCK` (reply below) · 5 minutes without any
RX byte reaching the firmware (auto-relock — the goodbye line arrives
unprompted) · power cycle (instant, silent). After the goodbye line drains:
≥ 22 ms of silence, then ONE packet, then normal 40 ms cadence — reset your
staleness timer on re-entry to packet mode.

**Transient lines to tolerate anywhere while unlocked** (slated for removal):
`nvm: set write... rc=<int>` / `nvm: cal write... rc=<int>` /
`nvm: cal erase... rc=<int>` (rc=0 means success), and the async
`Captured (<n> pts)\r\n` (see CAL).

## 5. Command reference — exact grammars

`<u>` = unsigned decimal, `<f>` = decimal with 3 fraction digits (e.g.
`123.400`), `<hex4>` = 4 uppercase hex digits. All replies end `\r\n`.
"Settings" replies use ` (saved)` on success or ` (NVM write failed)`.

### Session

| Send | Success reply | Errors / notes |
|---|---|---|
| `CONSOLE UNLOCK` | banner (§4) | while already unlocked: `Already unlocked` |
| `CONSOLE LOCK` | `Console LOCKED — packet stream resuming` | then stream resumes (§4). AUTO is force-disabled by re-lock |
| `CONSOLE` (bare) | — | `ERR: CONSOLE LOCK|UNLOCK` |

### Readouts

**`STATUS`** — exactly 5 lines:
```
ProbeA: <u>  ProbeB: <u>  Avg: <u>  Probe: A|B|AVG
Link: 0x<hex4>  LIVE|TEST(!)  mode=PKT|CONSOLE (stream suspended)  pkts=<u> aborts=<u> skips=<u>
Faults: none|[ADC_STALL ][VDDEXT ][DISAGREE]
Rate: <u>ms  Thresh: <u>  NVM: ok|INCONSISTENT
Cal: NONE|VALID  slope=<f> offset=<f>
```
- Probe counts are **12-bit-scaled (0–4092)**.
- `Link:` = the code that is (or will be) on the wire. `TEST(!)` = LINKTEST
  override active — flag it loudly.
- `Faults:` lists ALL active causes; the wire carries only the highest.
- `pkts` is a u32 counter of packets sent; it **freezes while the console is
  unlocked** (the stream is suspended), so it only approximates
  25×seconds-since-boot at the first unlock. `aborts`/`skips` should stay 0.

**`RAW`** — one fresh diagnostic burst, **native 10-bit units** (production
counts = 4× these):
```
RAW (native 10-bit, 1 LSB ~5mV; production counts = 4x):
  A: avg=<u> min=<u> max=<u> mV=<u> valid=<u>/16
  B: avg=<u> min=<u> max=<u> mV=<u> valid=<u>/16
```

**`SCAN`** — all analog inputs (`<-` marks firmware channels):
```
SCAN all analog inputs (<- = used by firmware):
  AN0(P2.0)    cnt=<u> mV=<u>
  AN1(P2.1)    cnt=<u> mV=<u>
  AN2(P2.2)    cnt=<u> mV=<u>
  AN3(P2.3) <-B cnt=<u> mV=<u>
  AN7(P2.7) <-A cnt=<u> mV=<u>
```

**`AUTO`** — toggles a telemetry line each refresh (default 1 Hz). Replies
`Auto ON` / `Auto OFF`. Line grammar:
```
A:<u> <u>mV  B:<u> <u>mV  Avg:<u>  P:<f>bar|uncal  Link:0x<hex4> TST|FLT|CAL|UNC
```
Tag: `TST` LINKTEST active · `FLT` fault · `CAL` calibrated · `UNC`
uncalibrated. Cleared automatically on re-lock.

**`POWER`** → `Power: 40 mW (continuous)` (placeholder value).

**`PSI <x>`** → `<f> psi = <f> bar` · **`BAR <x>`** → `<f> bar = <f> psi`
(errors: `ERR: PSI <value>` / `ERR: BAR <value>`).

**`HELP`** → 18-line command list (header `Commands:`).

### Settings (NVM-persisted)

| Send | Range | Success | Error |
|---|---|---|---|
| `RATE <ms>` | 100–5000 | `Rate=<u>ms (saved)` | `ERR: rate 100-5000` |
| `THRESH <cnt>` | 1–4092 | `Thresh=<u> (saved)` | `ERR: thresh 1-4092` |
| `PROBE A\|B\|AVG` | (`DUAL`=`AVG`) | `Probe=A|B|AVG (saved)` | `ERR: PROBE A|B|AVG` |

On flash failure the value is LIVE but not persisted: `Rate=<u>ms (NVM write
failed)` / `Thresh=<u> (NVM write failed)` / `Probe set (NVM write failed)`.
Arguments are strictly numeric — trailing garbage is rejected with the error
reply, nothing changes.

### Link test

| Send | Reply |
|---|---|
| `LINKTEST <n>` (0–65535 decimal) | `LinkTest=0x<hex4> — OVERRIDES live/fault codes; auto-expires in 5 min` + `(stream is suspended while unlocked: CONSOLE LOCK to transmit it)` |
| `LINKTEST OFF` | `LinkTest OFF — live values resume` |
| malformed / bare | `ERR: LINKTEST <0-65535>|OFF` |

**The gotcha:** the wire only carries the forced code AFTER `CONSOLE LOCK`
(stream suspended while unlocked). Sequence: `LINKTEST <n>` → `CONSOLE LOCK`
→ observe the wire → `CONSOLE UNLOCK` → `LINKTEST OFF`. Auto-expires after
5 minutes; also dies on power cycle.

### Calibration

| Send | Success | Distinct errors |
|---|---|---|
| `CAL ARM` | `Cal ARMED (<u> pts)` | `ERR: capture in progress, wait for 'Captured'` |
| `CAL <bar>` or `CAL <x> PSI` (0 < bar ≤ 1000, max 8 pts) | `Capturing at <f> bar[ (<f> psi)]...` then **ASYNC, ~8×RATE ms later:** `Captured (<u> pts)` | `ERR: CAL ARM first` · `ERR: max 8 pts (STORE or ABORT)` · `ERR: capture in progress, wait for 'Captured'` · `ERR: CAL <0..1000 bar>[ PSI]|ARM|STORE|CLEAR|STATUS|ABORT` |
| `CAL STORE` | `Cal stored: slope=<f> offset=<f>` | `ERR: need >=2 pts` · `ERR: degenerate fit (points at same counts)` · `ERR: NVM write failed` · capture-in-progress error |
| `CAL STATUS` | `Cal: VALID|NONE  pts=<u>[  slope=<f>  offset=<f>]` | — |
| `CAL CLEAR` | `Cal cleared` | partial: `Cal cleared (RAM only - NVM erase failed, may return after reset)` |
| `CAL ABORT` | `Cal aborted` | — |

**The analyzer must wait for the async `Captured (<n> pts)` line** before
sending the next capture — a capture takes `8 × RATE` ms (8 s at default;
send `RATE 100` first for ~0.8 s captures). Capture pauses while any fault is
active (the `Captured` line is simply delayed).

### Catch-alls

Unknown input → `ERR: unknown '<echoed input>' (try HELP)`. Bare keywords
return usage: `ERR: RATE <100-5000 ms>` · `ERR: THRESH <1-4092>` ·
`ERR: PROBE A|B|AVG` · `ERR: LINKTEST <0-65535>|OFF` ·
`ERR: CONSOLE LOCK|UNLOCK` ·
`ERR: CAL <bar>[ PSI]|ARM|STORE|CLEAR|STATUS|ABORT` · `ERR: PSI <value>` ·
`ERR: BAR <value>`.

## 6. Scripted workflows (expected traffic end-to-end)

**A. Passive monitor (default duty):** decode frames → display per §3.3 →
staleness per §3.4 → maintain counters (frames, checksum errors, rate ≈25/s).
Never transmit.

**B. Full calibration run:**
`CONSOLE UNLOCK` → banner → `RATE 100` → `(saved)` → `CAL ARM` →
`Cal ARMED (0 pts)` → apply ref P₁ → `CAL 14.7 PSI` → `Capturing at 1.013
bar (14.700 psi)...` → wait `Captured (1 pts)` → apply ref P₂ → `CAL <bar>` →
`Captured (2 pts)` → `CAL STORE` → `Cal stored: slope=… offset=…` →
`RATE 1000` → `CONSOLE LOCK` → goodbye → stream resumes → **verify decoded
deci-bar against the reference gauge.**

**C. Wire-vector verification (0x7F pass-through proof):**
unlock → `LINKTEST 10000` → lock → expect `7F … 27 10 C8` · repeat with
`127` → `7F 00 7F 80` and `383` → `7F 01 7F 7F` (decoder must read 12.7 /
38.3 bar, not lose sync) · unlock → `LINKTEST OFF` → lock.

**D. Fault observation:** unlock → `AUTO` → note |A−B| → `THRESH <below it>`
→ `STATUS` shows `Faults: DISAGREE` → lock → wire carries `0xFF03` → unlock
→ restore `THRESH 80` → lock → wire returns to live.

**E. Build identification:** send `CONSOLE UNLOCK`; stream pauses + banner =
debug build; nothing changes = production build (or wrong baud — packets
still decoding at 9600 confirms baud, isolating the answer).

## 7. Parsing gotchas (the complete list)

1. **Echo** precedes every reply while unlocked (§4 rule 1).
2. **`Captured (<n> pts)` is asynchronous** — it arrives seconds after its
   command, possibly after you've done other things. Treat it as an event.
3. **One command per loop pass** — wait for each reply.
4. `0x7F` is legal INSIDE frames — decode per §3.2, never scan-and-reset.
5. Text can never contain `0x7F` → a sync byte is always a real sync byte.
6. After ANY re-lock (yours, timeout, power cycle), expect ~22 ms silence
   (≥21.8 ms guaranteed) then a single packet — don't flag staleness during
   this handover.
7. `nvm: … rc=` lines can appear inside settings/CAL replies — skip them.
8. TX ring drops bytes when full — don't send >1 KB bursts.
9. First boot after a firmware flash: settings + calibration are wiped by
   design (NVM magic bumps) — expect defaults and `0xFF02` UNCAL.
10. `pkts` counter is u32 (wraps after ~5 years); `aborts`/`skips` are u16.

## 8. Cross-references

| Topic | File |
|---|---|
| Normative wire timing + logger questionnaire | `link_protocol.md` |
| Human-oriented command doc | `uart_command_reference.md` |
| Executable decoder reference + test vectors | `host_ui/link_decoder.py`, `host_ui/test_link_decoder.py` |
| Firmware reply strings (source of truth) | `app/uart_cmd.c` (`process_cmd`) |
| Code map / timing constants | `app/link_frame.h` |
| Hardware acceptance procedure | `verification_guide.md` |
