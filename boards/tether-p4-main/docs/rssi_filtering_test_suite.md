# RSSI Filtering Test Suite

This suite is for collecting labeled RSSI traces to tune filtering and movement classification.

## Commands

Use `s` then Enter in the monitor to start each run. The firmware waits in manual mode until you start it.

Stationary near:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label stationary-near --stats -o rssi_plots | tee walk_stationary_near.log
```

Stationary edge:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label stationary-edge --stats -o rssi_plots | tee walk_stationary_edge.log
```

Stationary far:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label stationary-far --stats -o rssi_plots | tee walk_stationary_far.log
```

Walking towards:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label towards --stats -o rssi_plots | tee walk_towards.log
```

Walking parallel:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label parallel --stats -o rssi_plots | tee walk_parallel.log
```

Walking away:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label away --stats -o rssi_plots | tee walk_away.log
```

Body-blocked or pocket:

```bash
idf.py flash monitor 2>&1 | tools/plot_rssi_trace.py - --live --passthrough --label body-blocked --stats -o rssi_plots | tee walk_body_blocked.log
```

## Timing

The device logs a 3 second countdown, then starts the actual 4 second trace.

Start walking when you see:

```text
START WALKING NOW
RSSI_TRACE_START_WALK_NOW,<peer_mac>,4000
RSSI_TRACE_BEGIN,<peer_mac>,4000
```

The raw `RSSI_TRACE,...` rows are dumped after the 4 second capture finishes. That delay is normal because the firmware buffers samples during the walk and prints them after capture.

## Baseline Set

Use one peer and the same physical path for every run.

Run these in this order:

1. Standing still, device held normally: 3 runs
2. Walking towards: 5 runs
3. Walking parallel: 5 runs
4. Walking away: 5 runs
5. Pocket/body-blocked towards: 3 runs

If time is tight, do the minimum set:

1. Walking towards: 5 runs
2. Walking parallel: 5 runs

## Control Rules

- Start each walk on `RSSI_TRACE_START_WALK_NOW`.
- Keep phone/tag orientation the same within a batch.
- Use the same walking speed.
- Use the same start and end points.
- Keep the ESP board in the same position and orientation.
- Disconnect/reconnect the peer between runs so each run gets a fresh countdown and trace.

## What To Compare

Use the generated CSV columns:

```text
rssi_dbm
rssi_kalman_dbm
```

The `--stats` flag also writes one summary row per peer/run to:

```text
rssi_plots/rssi_trace_stats.csv
```

Initial features to compare:

```text
kalman_delta = final rssi_kalman_dbm - first rssi_kalman_dbm
kalman_slope = kalman_delta / 4 seconds
raw_stddev = jitter/multipath level
kalman_stddev = smoothed movement level
residual_stddev = raw jitter after smoothing
derivative_stddev = filtered signal change energy
```

Early heuristic to test:

```text
towards if kalman_delta > +5 dB
parallel/unclear if abs(kalman_delta) <= 4 dB
away/body-blocked if kalman_delta < -5 dB
```

Do not lock these thresholds until the labels are clean.

## Kalman Tuning

Current defaults:

```text
Q = 0.5
R = 16.0
```

Tune with existing logs or CSVs:

```bash
tools/plot_rssi_trace.py walk_towards.log -o rssi_plots/kalman_tuning --kalman-q 1.0 --kalman-r 16
```

Rules of thumb:

```text
Higher Q = faster response, less smoothing
Lower Q = smoother, more lag
Higher R = more smoothing, trusts RSSI less
Lower R = less smoothing, trusts RSSI more
```

Recommended values to compare:

```text
Q=0.2, R=16   smoother
Q=0.5, R=16   current balanced default
Q=1.0, R=16   faster walking response
Q=0.5, R=9    more reactive
Q=0.5, R=25   smoother
```

## Notes

If `RSSI_TRACE_COUNTDOWN,0` appears and the `RSSI_TRACE` rows appear a few seconds later, that is expected. Walk at `START_WALK_NOW`; the rows are printed after the capture window finishes.
