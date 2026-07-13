# DeepProbe — Downhole Pressure Probe Monitor (host GUI)

A small dashboard that connects to the board's bench UART and shows live
pressure / output, probe stats, a trend chart, and buttons for the common
commands — so you don't have to drive it from a terminal.

## Setup (one time)
```
pip install -r requirements.txt
```
(installs `customtkinter` + `pyserial`)

## Run
```
python pressure_monitor.py
```

## Use
1. Pick the board's **COM port** in the top-left, hit **Connect**.
   (Hit **↻** to rescan if it's not listed.)
2. It auto-sends `STATUS` then `AUTO`, so telemetry starts streaming.
3. The gauge/number shows **pressure (bar)** once calibrated, otherwise the
   **output voltage** (labelled "uncal"). The trend chart and tiles update live.

## Demo tips
- Click **Fast 10Hz** (`RATE 100`) for a snappy, smooth-moving display during
  the presentation; **Normal 1Hz** to slow it back down.
- Single probe on the bench? Click **Probe A** so the disagreement fault
  doesn't pin the output.
- If the output looks stuck, click **Output Auto** (clears a manual override).
- The **Mode** tile shows which path drives the output: `RAW` (uncalibrated),
  `CAL` (calibrated), `MAN` (manual), `FLT` (fault) — colour-coded.
- Any other command (e.g. `RANGE 0 600`, `CAL ARM`) can be typed in the box at
  the bottom; the log shows everything the board sends.

Protocol: 115200 8N1, line-based — same as the terminal. See
`../uart_command_reference.md` for the full command set.
