# Changelog

## Unreleased

- GUI: tooltips on every top-bar control (server host/port, Connect, output
  format, Pause, ageout, rate) and the tab strip, explaining each function.
- GUI: an "About ADSBtoCOT" item added to the window (system) menu, opening
  an About dialog with version and project link.
- Build: compile MSVC sources with `/utf-8` so non-ASCII characters in
  labels and tooltips render correctly.
- Added the MIT license.
- Documentation: README screenshot, release/downloads/license badges, and a
  Releases section; corrected the send-rate option name ("1 Hz limit") and
  clarified that rate limiting depends on the selected send-rate mode.

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
