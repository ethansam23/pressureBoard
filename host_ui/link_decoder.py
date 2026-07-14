"""
Downhole-link packet decoder — host side (pure, no I/O).

Decodes the transmitter's one-way UART stream (see link_protocol.md):

    |0x7F| >2ms gap |MSB|LSB|CHK|  ...>20ms idle...   repeat
    CHK = ~(MSB+LSB) & 0xFF   (additive checksum, NOT a CRC)

Design rules (from the firmware design review):

- PER-FRAME state model: FRAME_SEARCH (find a 0x7F candidate) ->
  FRAME_CANDIDATE (consume exactly the next three bytes POSITIONALLY) ->
  validate -> back to FRAME_SEARCH. There is no "locked" mode that blindly
  strides the stream in 4-byte groups — one dropped or inserted byte must
  never permanently misalign the decoder.
- A candidate is NEVER rejected because a payload or checksum byte equals
  0x7F (payload is sacred; e.g. code 127 = 7F 00 7F 80 on the wire).
- On checksum failure: sliding recovery — re-scan starting ONE byte past the
  failed candidate's sync byte.
- The multi-frame streak is a UI confidence indicator only.
- OS serial stacks deliver buffered chunks with no reliable per-byte
  timestamps, so timing is a coarse HINT (frame-rate/staleness statistics),
  never a framing criterion.
- This decoder's ability to recover says nothing about the LOGGER's — only
  the logger designer's answers do (link_protocol.md questionnaire).

Bytes that are not part of a valid frame are surfaced as 'text' so the
bench console output (which shares the line under mutual exclusion) can be
displayed. ASCII text can never contain 0x7F, so console output cannot fake
a sync byte.
"""

SYNC = 0x7F
FRAME_LEN = 4          # sync + MSB + LSB + checksum

# Status-code page (must match app/link_frame.h)
CODE_NAMES = {
    0xFF01: "NO_READING",
    0xFF02: "UNCAL",
    0xFF03: "DISAGREE",
    0xFF04: "ADC_STALL",
    0xFF05: "VDDEXT",
    0xFF06: "OVER_RANGE",
    0xFF07: "UNDER_RANGE",
}
VALUE_MAX = 10000      # deci-bar


def checksum(msb, lsb):
    return (~(msb + lsb)) & 0xFF


def decode_code(code):
    """Classify a 16-bit wire code.

    Returns (kind, value): ("pressure", bar float) | ("status", name str) |
    ("invalid", code int) for codes outside both defined regions."""
    if code <= VALUE_MAX:
        return ("pressure", code / 10.0)
    if code in CODE_NAMES:
        return ("status", CODE_NAMES[code])
    return ("invalid", code)


class LinkDecoder:
    """Incremental decoder. feed(data, t=None) -> list of events:

        ("frame", code:int)   a checksum-valid frame
        ("text",  bytes)      bytes that are not part of a valid frame

    Counters: frames_ok, checksum_errors, resyncs, text_bytes, streak
    (consecutive valid frames — confidence only). Timing (coarse, from the
    chunk timestamps passed to feed): last_frame_t, frame_intervals (last
    64, seconds)."""

    def __init__(self):
        self._buf = bytearray()
        self.frames_ok = 0
        self.checksum_errors = 0
        self.resyncs = 0
        self.text_bytes = 0
        self.streak = 0
        self.last_frame_t = None
        self.frame_intervals = []

    def feed(self, data, t=None):
        self._buf.extend(data)
        events = []
        text = bytearray()

        def flush_text():
            if text:
                self.text_bytes += len(text)
                events.append(("text", bytes(text)))
                text.clear()

        i = 0
        buf = self._buf
        while i < len(buf):
            b = buf[i]
            if b != SYNC:                      # FRAME_SEARCH
                text.append(b)
                i += 1
                continue
            # FRAME_CANDIDATE: need exactly three positional bytes after the
            # sync. If they haven't arrived yet, keep the tail for next feed.
            if i + FRAME_LEN > len(buf):
                break
            msb, lsb, chk = buf[i + 1], buf[i + 2], buf[i + 3]
            # NOTE: msb/lsb/chk == 0x7F is perfectly legal — no check here.
            if chk == checksum(msb, lsb):
                flush_text()
                code = (msb << 8) | lsb
                self.frames_ok += 1
                self.streak += 1
                if t is not None:
                    if self.last_frame_t is not None:
                        self.frame_intervals.append(t - self.last_frame_t)
                        if len(self.frame_intervals) > 64:
                            self.frame_intervals.pop(0)
                    self.last_frame_t = t
                events.append(("frame", code))
                i += FRAME_LEN
            else:
                # Sliding recovery: this 0x7F was payload, noise, or the
                # frame is corrupt. Surface the sync byte as text and re-scan
                # one byte later.
                self.checksum_errors += 1
                self.resyncs += 1
                self.streak = 0
                text.append(b)
                i += 1

        flush_text()
        del buf[:i]
        return events

    @property
    def confident(self):
        """UI hint: two consecutive valid frames (never a parsing rule)."""
        return self.streak >= 2

    def frame_rate(self):
        """Coarse packets/second from chunk timestamps (hint only)."""
        iv = [x for x in self.frame_intervals if x > 0]
        if len(iv) < 4:
            return None
        return len(iv) / sum(iv)
