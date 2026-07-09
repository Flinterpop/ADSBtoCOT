// Shared core for adsbtocot: JSON feed parsing, TCP/UDP networking, and
// Cursor-on-Target event generation (XML and TAK protobuf). Used by both
// the console and GUI apps.
#pragma once

#include <string>
#include <optional>
#include <functional>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
inline void closesocket(socket_t s) { close(s); }
#endif

// One aircraft record, pre-formatted for display.
struct AdsbRow {
    std::string time, hex, flight, squawk, alt, gs, trk, lat, lon, rssi, type;
};

// Why a message did or didn't produce a CoT event, reported per message.
enum class CotStatus {
    Disabled,     // CoT output turned off
    Sent,         // event transmitted
    Throttled,    // suppressed: within the per-aircraft rate-limit window
    NoPosition,   // suppressed: message carried no lat/lon fix
    Fill,         // synthetic 1 Hz heartbeat re-sending the last position
};

// Short label for the log's CoT column ("" when disabled).
inline const char* cotStatusLabel(CotStatus s)
{
    switch (s) {
    case CotStatus::Sent:       return "sent";
    case CotStatus::Throttled:  return "dup";
    case CotStatus::NoPosition: return "no-pos";
    case CotStatus::Fill:       return "fill";
    case CotStatus::Disabled:   return "";
    }
    return "";
}

// How often CoT events are emitted per aircraft.
enum class RateMode {
    NoLimit,    // forward every message that has a position (1 in / 1 out)
    OneHz,      // at most one event per aircraft per second
    OneHzFill,  // one per aircraft per second; re-send last position as a
                // fill when no fresh message arrived that second
};

// Extracts display fields from one JSON line; nullopt if not an aircraft record.
std::optional<AdsbRow> parseRow(const std::string& line);

struct FeedConfig {
    std::string adsbHost, adsbPort;
    std::string cotHost, cotPort;
    bool cotEnabled = true;
};

// Live controls shared between the feed thread and its owner. `protobuf`
// switches the CoT wire format on the fly (false = XML, true = TAK protocol
// mesh protobuf). `rateMode` selects how often events are emitted per
// aircraft. requestStop() unblocks a blocking recv so the feed thread exits
// promptly and runFeed returns.
struct FeedControl {
    std::atomic<bool> stop{false};
    std::atomic<bool> protobuf{false};
    std::atomic<RateMode> rateMode{RateMode::OneHz};
    std::atomic<socket_t> sock{INVALID_SOCKET};

    void requestStop();
};

// Connects to the ADS-B feed and runs until requestStop(), reconnecting on
// failure. Calls onMessage(jsonLine, cotSent) for every received record and
// onStatus(text) on connection-state changes. Returns false only on a fatal
// setup error (socket init).
bool runFeed(const FeedConfig& cfg, FeedControl& ctl,
             const std::function<void(const std::string&, CotStatus)>& onMessage,
             const std::function<void(const std::string&)>& onStatus);
