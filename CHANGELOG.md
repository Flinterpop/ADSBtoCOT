# Changelog

## 1.0.0 — 2026-07-08

First release.

- Connects to a readsb/dump1090 newline-delimited JSON position feed and
  logs received ADS-B messages.
- Forwards each aircraft position as a Cursor-on-Target event over UDP
  (default TAK mesh SA multicast 239.2.3.1:6969).
- Two front-ends over a shared core:
  - `adsbtocot` — console log + CoT forwarder.
  - `adsbtocot_gui` — Windows UI with editable server IP/port and a
    Connect/Disconnect toggle, a Log tab (scrolling messages, pausable),
    and a Tracks tab (one row per aircraft with age and configurable
    ageout).
- CoT output formats: XML or TAK Protocol Version 1 mesh protobuf.
- Send rate selector: No limit (1 in / 1 out), 1 Hz limit, or 1 Hz + fill
  (heartbeat re-sends so every track updates at a steady 1 Hz). The log's
  CoT column shows `sent`, `dup`, `fill`, or `no-pos`.
- All GUI settings persist to `%APPDATA%\ADSBtoCOT\adsbtocot.ini`.
- No external dependencies: Winsock/BSD sockets, Win32 common controls, a
  minimal JSON extractor, and a hand-rolled protobuf encoder.
