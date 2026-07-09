#include "adsb_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <unordered_map>
#include <chrono>

constexpr double KNOTS_TO_MS = 0.514444;
constexpr double FEET_TO_M   = 0.3048;
constexpr int    COT_STALE_S = 120;   // event validity window
constexpr double COT_MIN_INTERVAL_S = 1.0;  // per-aircraft send throttle
constexpr double FILL_TIMEOUT_S = 60.0;  // stop filling after this quiet time
constexpr long   RECV_TIMEOUT_MS = 250;  // recv wakeups to drive fills / stop
constexpr double UNKNOWN = 9999999.0; // CoT convention for unknown hae/ce/le

// ---------------------------------------------------------------------------
// Minimal JSON field extraction. The feed is flat single-line objects, so a
// key scan is sufficient — no full parser needed.
// ---------------------------------------------------------------------------

// Returns the raw text of the value that follows "key": in `json`, or nullopt.
static std::optional<std::string> jsonRaw(const std::string& json, const char* key)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t p = pos + needle.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
        if (p >= json.size() || json[p] != ':') { pos += needle.size(); continue; }
        ++p;
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
        if (p >= json.size()) return std::nullopt;

        if (json[p] == '"') {                       // string value
            size_t end = json.find('"', p + 1);
            if (end == std::string::npos) return std::nullopt;
            return json.substr(p + 1, end - p - 1);
        }
        size_t end = p;                             // number / literal / array
        while (end < json.size() && json[end] != ',' && json[end] != '}' &&
               json[end] != '\n') ++end;
        return json.substr(p, end - p);
    }
    return std::nullopt;
}

static std::optional<double> jsonNum(const std::string& json, const char* key)
{
    auto raw = jsonRaw(json, key);
    if (!raw) return std::nullopt;
    char* endp = nullptr;
    double v = strtod(raw->c_str(), &endp);
    if (endp == raw->c_str()) return std::nullopt;  // e.g. "ground"
    return v;
}

static std::string trim(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

static std::string upper(std::string s)
{
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// ---------------------------------------------------------------------------
// Row extraction for display
// ---------------------------------------------------------------------------

std::optional<AdsbRow> parseRow(const std::string& line)
{
    auto hex = jsonRaw(line, "hex");
    if (!hex) return std::nullopt;

    AdsbRow row;
    row.hex = *hex;

    time_t now = time(nullptr);
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &lt);
    row.time = ts;

    row.flight = trim(jsonRaw(line, "flight").value_or(""));
    row.squawk = jsonRaw(line, "squawk").value_or("");
    row.type   = jsonRaw(line, "type").value_or("");

    if (auto altRaw = jsonRaw(line, "alt_baro")) {
        if (auto v = jsonNum(line, "alt_baro")) {
            char buf[16];
            snprintf(buf, sizeof buf, "%.0f", *v);
            row.alt = buf;
        } else {
            row.alt = *altRaw;                      // "ground"
        }
    }

    char buf[24];
    if (auto v = jsonNum(line, "gs"))    { snprintf(buf, sizeof buf, "%.0f", *v); row.gs   = buf; }
    if (auto v = jsonNum(line, "track")) { snprintf(buf, sizeof buf, "%.1f", *v); row.trk  = buf; }
    if (auto v = jsonNum(line, "lat"))   { snprintf(buf, sizeof buf, "%.5f", *v); row.lat  = buf; }
    if (auto v = jsonNum(line, "lon"))   { snprintf(buf, sizeof buf, "%.5f", *v); row.lon  = buf; }
    if (auto v = jsonNum(line, "rssi"))  { snprintf(buf, sizeof buf, "%.1f", *v); row.rssi = buf; }
    return row;
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

// Registers the in-progress socket in ctl.sock so requestStop() can close it
// and abort a blocking connect() (which otherwise takes ~20s to time out).
static socket_t connectTcp(const char* host, const char* port, FeedControl& ctl)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return INVALID_SOCKET;

    socket_t s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        ctl.sock = s;
        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        // exchange() decides ownership: if requestStop() already took the
        // socket it also closed it, and we must not touch it again.
        socket_t owned = ctl.sock.exchange(INVALID_SOCKET);
        if (rc == 0 && owned != INVALID_SOCKET) {
            ctl.sock = owned;                   // re-register for recv abort
            break;
        }
        if (owned != INVALID_SOCKET) closesocket(owned);
        s = INVALID_SOCKET;
        if (ctl.stop) break;
    }
    freeaddrinfo(res);
    return s;
}

// UDP sender bound to a fixed destination (unicast or multicast).
struct UdpSender {
    socket_t sock = INVALID_SOCKET;
    sockaddr_storage dest{};
    int destLen = 0;

    ~UdpSender()
    {
        if (sock != INVALID_SOCKET) closesocket(sock);
    }

    bool open(const char* host, const char* port)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* res = nullptr;
        if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
            return false;

        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(res); return false; }

        memcpy(&dest, res->ai_addr, res->ai_addrlen);
        destLen = (int)res->ai_addrlen;
        freeaddrinfo(res);

        // Multicast TTL so events reach the local network segment; the
        // option is ignored for unicast destinations.
        int ttl = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                   (const char*)&ttl, sizeof ttl);
        return true;
    }

    void send(const std::string& payload) const
    {
        if (sock == INVALID_SOCKET) return;
        sendto(sock, payload.data(), (int)payload.size(), 0,
               (const sockaddr*)&dest, destLen);
    }
};

// ---------------------------------------------------------------------------
// Cursor-on-Target — common field extraction
// ---------------------------------------------------------------------------

struct CotData {
    std::string icao, callsign, cotType, squawk;
    double lat = 0, lon = 0, hae = UNKNOWN;
    bool hasTrack = false;
    double course = 0, speedMs = 0;
};

// Map the ADS-B emitter category to a CoT atom type (neutral air track).
static const char* cotTypeFor(const std::string& category)
{
    if (category == "A7") return "a-n-A-C-H";      // rotorcraft
    if (category == "B2") return "a-n-A-C-L";      // lighter-than-air
    return "a-n-A-C-F";                            // fixed wing (default)
}

// Pull CoT-relevant fields from one aircraft record; nullopt if no position.
static std::optional<CotData> extractCot(const std::string& line)
{
    auto hex = jsonRaw(line, "hex");
    auto lat = jsonNum(line, "lat");
    auto lon = jsonNum(line, "lon");
    if (!hex || !lat || !lon) return std::nullopt;

    CotData d;
    d.icao = upper(*hex);
    std::string flight = trim(jsonRaw(line, "flight").value_or(""));
    d.callsign = flight.empty() ? d.icao : flight;
    d.cotType = cotTypeFor(jsonRaw(line, "category").value_or(""));
    d.squawk = jsonRaw(line, "squawk").value_or("");
    d.lat = *lat;
    d.lon = *lon;

    // Prefer geometric altitude (WGS-84 HAE); fall back to barometric.
    if (auto v = jsonNum(line, "alt_geom"))      d.hae = *v * FEET_TO_M;
    else if (auto v2 = jsonNum(line, "alt_baro")) d.hae = *v2 * FEET_TO_M;

    if (auto trk = jsonNum(line, "track")) {
        d.hasTrack = true;
        d.course = *trk;
        d.speedMs = jsonNum(line, "gs").value_or(0.0) * KNOTS_TO_MS;
    }
    return d;
}

// ---------------------------------------------------------------------------
// CoT XML serializer
// ---------------------------------------------------------------------------

static std::string isoTime(time_t t)
{
    tm gt{};
#ifdef _WIN32
    gmtime_s(&gt, &t);
#else
    gmtime_r(&t, &gt);
#endif
    char buf[24];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &gt);
    return buf;
}

static std::string buildCotXml(const CotData& d)
{
    time_t now = time(nullptr);
    std::string t0 = isoTime(now), t1 = isoTime(now + COT_STALE_S);

    char buf[1536];
    int n = snprintf(buf, sizeof buf,
        "<?xml version=\"1.0\"?>"
        "<event version=\"2.0\" uid=\"ICAO-%s\" type=\"%s\" how=\"m-g\""
        " time=\"%s\" start=\"%s\" stale=\"%s\">"
        "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"%.1f\""
        " ce=\"9999999.0\" le=\"9999999.0\"/>"
        "<detail>"
        "<contact callsign=\"%s\"/>",
        d.icao.c_str(), d.cotType.c_str(),
        t0.c_str(), t0.c_str(), t1.c_str(),
        d.lat, d.lon, d.hae, d.callsign.c_str());
    if (n < 0 || n >= (int)sizeof buf) return "";
    std::string xml(buf, n);

    if (d.hasTrack) {
        n = snprintf(buf, sizeof buf,
                     "<track course=\"%.1f\" speed=\"%.1f\"/>",
                     d.course, d.speedMs);
        xml.append(buf, n);
    }
    if (!d.squawk.empty()) {
        n = snprintf(buf, sizeof buf,
                     "<remarks>ICAO %s squawk %s</remarks>",
                     d.icao.c_str(), d.squawk.c_str());
        xml.append(buf, n);
    }
    xml += "</detail></event>";
    return xml;
}

// ---------------------------------------------------------------------------
// CoT protobuf serializer — TAK Protocol Version 1, mesh SA framing.
// The TakMessage schema is small and stable, so the wire format is encoded
// directly rather than pulling in libprotobuf.
// ---------------------------------------------------------------------------

static void pbVarint(std::string& out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back((char)(v | 0x80));
        v >>= 7;
    }
    out.push_back((char)v);
}

static void pbTag(std::string& out, int field, int wire)
{
    pbVarint(out, ((uint64_t)field << 3) | wire);
}

static void pbString(std::string& out, int field, const std::string& s)
{
    pbTag(out, field, 2);
    pbVarint(out, s.size());
    out += s;
}

static void pbUint(std::string& out, int field, uint64_t v)
{
    pbTag(out, field, 0);
    pbVarint(out, v);
}

static void pbDouble(std::string& out, int field, double v)
{
    pbTag(out, field, 1);
    char le[8];
    memcpy(le, &v, 8);                              // x86/ARM: little-endian
    out.append(le, 8);
}

static std::string buildCotProto(const CotData& d)
{
    uint64_t nowMs = (uint64_t)time(nullptr) * 1000;

    // detail.contact { callsign = 2 }
    std::string contact;
    pbString(contact, 2, d.callsign);

    // detail { xmlDetail = 1, contact = 2, track = 7 }
    std::string detail;
    if (!d.squawk.empty())
        pbString(detail, 1, "<remarks>ICAO " + d.icao + " squawk " +
                            d.squawk + "</remarks>");
    pbString(detail, 2, contact);
    if (d.hasTrack) {
        std::string track;                          // { speed = 1, course = 2 }
        pbDouble(track, 1, d.speedMs);
        pbDouble(track, 2, d.course);
        pbString(detail, 7, track);
    }

    // cotEvent { type=1 uid=5 sendTime=6 startTime=7 staleTime=8 how=9
    //            lat=10 lon=11 hae=12 ce=13 le=14 detail=15 }
    std::string ev;
    pbString(ev, 1, d.cotType);
    pbString(ev, 5, "ICAO-" + d.icao);
    pbUint(ev, 6, nowMs);
    pbUint(ev, 7, nowMs);
    pbUint(ev, 8, nowMs + (uint64_t)COT_STALE_S * 1000);
    pbString(ev, 9, "m-g");
    pbDouble(ev, 10, d.lat);
    pbDouble(ev, 11, d.lon);
    pbDouble(ev, 12, d.hae);
    pbDouble(ev, 13, UNKNOWN);
    pbDouble(ev, 14, UNKNOWN);
    pbString(ev, 15, detail);

    // takMessage { cotEvent = 2 }, framed with the mesh SA magic header
    std::string msg{'\xbf', '\x01', '\xbf'};
    pbString(msg, 2, ev);
    return msg;
}

// ---------------------------------------------------------------------------
// Feed loop
// ---------------------------------------------------------------------------

void FeedControl::requestStop()
{
    stop = true;
    // Closing the socket unblocks the feed thread's recv. exchange() ensures
    // exactly one side closes it.
    socket_t s = sock.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
}

bool runFeed(const FeedConfig& cfg, FeedControl& ctl,
             const std::function<void(const std::string&, CotStatus)>& onMessage,
             const std::function<void(const std::string&)>& onStatus)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        onStatus("WSAStartup failed");
        return false;
    }
#endif

    UdpSender cot;
    if (cfg.cotEnabled &&
        !cot.open(cfg.cotHost.c_str(), cfg.cotPort.c_str())) {
        onStatus("failed to open CoT UDP socket for " + cfg.cotHost + ":" +
                 cfg.cotPort);
        return false;
    }

    using clock = std::chrono::steady_clock;
    auto secsSince = [](clock::time_point t) {
        return std::chrono::duration<double>(clock::now() - t).count();
    };
    auto payloadFor = [&](const CotData& d) {
        return ctl.protobuf ? buildCotProto(d) : buildCotXml(d);
    };

    // Per-aircraft send state: last transmit and last real message times,
    // plus the latest position (kept for fill re-sends in OneHzFill mode).
    struct Track {
        clock::time_point lastSent{};
        clock::time_point lastMsg{};
        bool everSent = false;
        bool hasData = false;
        CotData data;
        std::string line;
    };
    std::unordered_map<std::string, Track> tracks;

    std::string buf;
    while (!ctl.stop) {
        onStatus("connecting to " + cfg.adsbHost + ":" + cfg.adsbPort + "...");
        socket_t sock = connectTcp(cfg.adsbHost.c_str(), cfg.adsbPort.c_str(),
                                   ctl);
        if (ctl.stop) {
            socket_t s = ctl.sock.exchange(INVALID_SOCKET);
            if (s != INVALID_SOCKET) closesocket(s);
            break;
        }
        if (sock == INVALID_SOCKET) {
            onStatus("connect to " + cfg.adsbHost + ":" + cfg.adsbPort +
                     " failed, retrying in 5s...");
            for (int i = 0; i < 20 && !ctl.stop; ++i)
#ifdef _WIN32
                Sleep(250);
#else
                usleep(250000);
#endif
            continue;
        }
        onStatus("connected to " + cfg.adsbHost + ":" + cfg.adsbPort);

        // Wake recv periodically so fills can be emitted and stop requests
        // noticed even when the feed is momentarily quiet.
#ifdef _WIN32
        DWORD tv = RECV_TIMEOUT_MS;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
#else
        timeval tv{RECV_TIMEOUT_MS / 1000, (RECV_TIMEOUT_MS % 1000) * 1000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif

        char chunk[4096];
        while (!ctl.stop) {
            int n = recv(sock, chunk, sizeof chunk, 0);
            if (n == 0) break;                       // orderly close
            if (n < 0) {
#ifdef _WIN32
                if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
                // timeout: no data this interval, fall through to fills
            } else {
                buf.append(chunk, n);

                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (line.empty()) continue;

                    CotStatus status = CotStatus::Disabled;
                    if (cfg.cotEnabled) {
                        if (auto hex = jsonRaw(line, "hex")) {
                            RateMode mode = ctl.rateMode.load();
                            Track& t = tracks[*hex];
                            if (auto d = extractCot(line)) {
                                t.data = *d;
                                t.line = line;
                                t.hasData = true;
                                t.lastMsg = clock::now();
                            }
                            bool due = mode == RateMode::NoLimit ||
                                !t.everSent ||
                                secsSince(t.lastSent) >= COT_MIN_INTERVAL_S;
                            if (!t.hasData && due) {
                                status = CotStatus::NoPosition;
                            } else if (!due) {
                                status = CotStatus::Throttled;
                            } else if (t.hasData) {
                                cot.send(payloadFor(t.data));
                                t.lastSent = clock::now();
                                t.everSent = true;
                                status = CotStatus::Sent;
                            }
                        }
                    }
                    onMessage(line, status);
                }
            }

            // Fill pass: re-send the last position for aircraft that went a
            // full second without a fresh message (OneHzFill mode only).
            if (cfg.cotEnabled && ctl.rateMode.load() == RateMode::OneHzFill) {
                for (auto& [hex, t] : tracks) {
                    if (!t.hasData) continue;
                    if (secsSince(t.lastMsg) > FILL_TIMEOUT_S) continue;
                    if (t.everSent && secsSince(t.lastSent) < COT_MIN_INTERVAL_S)
                        continue;
                    cot.send(payloadFor(t.data));
                    t.lastSent = clock::now();
                    t.everSent = true;
                    onMessage(t.line, CotStatus::Fill);
                }
            }
        }
        socket_t s = ctl.sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s);
        buf.clear();
        if (!ctl.stop)
            onStatus("connection lost, reconnecting...");
    }
    onStatus("stopped");
    return true;
}
