// adsbtocot_gui — Windows UI front-end. Top bar with ADS-B server address,
// Connect button, CoT output format selector (XML / TAK protobuf), and a
// pause button. Below it, two tabs: "Log" (scrolling message log) and
// "Tracks" (one row per unique aircraft, updated in place). Status bar with
// connection state and message/CoT/track counters. Networking runs on a
// worker thread; rows are posted to the UI thread as messages.
//
// Usage: adsbtocot_gui [adsb_host] [adsb_port] [cot_host] [cot_port]
//        defaults:      192.168.1.135  30154     239.2.3.1  6969
//        pass "-" as cot_host to disable CoT output (log only)

#define UNICODE
#define _UNICODE

#include "adsb_core.h"

#include <windows.h>
#include <commctrl.h>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <ctime>

#ifndef ADSBTOCOT_VERSION
#define ADSBTOCOT_VERSION "dev"
#endif

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_ROW    = WM_APP + 1;  // lParam = RowMsg*
constexpr UINT WM_APP_STATUS = WM_APP + 2;  // lParam = std::string*
constexpr int  MAX_LOG_ROWS  = 10000;       // oldest rows dropped beyond this
constexpr int  TOPBAR_H      = 38;

constexpr int ID_EDIT_HOST  = 10;
constexpr int ID_EDIT_PORT  = 11;
constexpr int ID_BTN_CONN   = 12;
constexpr int ID_CMB_FORMAT = 13;
constexpr int ID_BTN_PAUSE   = 14;
constexpr int ID_TAB         = 15;
constexpr int ID_EDIT_AGEOUT = 16;
constexpr int ID_CMB_RATE    = 17;
constexpr int TIMER_AGE      = 1;

constexpr int COL_TRK_MSGS = 10;
constexpr int COL_TRK_LAST = 11;
constexpr int COL_TRK_AGE  = 12;

struct RowMsg {
    AdsbRow row;
    CotStatus cotStatus;
};

struct TrackInfo {
    int item;             // row index in the track list; re-synced on drops
    long long msgs;
    time_t lastSeen;
};

HWND g_wnd, g_statusBar, g_tab, g_logList, g_trackList;
HWND g_editHost, g_editPort, g_btnConnect, g_cmbFormat, g_btnPause;
HWND g_editAgeout, g_cmbRate, g_tooltip;
HFONT g_monoFont, g_uiFont;
long long g_received = 0, g_cotSent = 0;
bool g_paused = false;
bool g_feedActive = false;
std::unordered_map<std::string, TrackInfo> g_tracks;

FeedConfig g_cfg;
FeedControl g_ctl;
std::thread g_feedThread;

// Persisted UI settings (INI in %APPDATA%\ADSBtoCOT). Defaults match the
// command-line fallbacks; loadSettings() overwrites from disk if present.
struct Settings {
    std::wstring host = L"192.168.1.135", port = L"30154", ageout = L"60";
    std::wstring cotHost = L"239.2.3.1", cotPort = L"6969";
    int format = 0, rate = 1;                       // combo indices
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1010, h = 660, maximized = 0;
};
Settings g_settings;

void saveSettings();   // defined below; used by toggleConnection

// Feed data is plain ASCII, so direct conversions are safe.
std::wstring widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::string narrow(const std::wstring& s)
{
    return std::string(s.begin(), s.end());
}

std::string windowText(HWND h)
{
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    return narrow(buf);
}

void setCell(HWND list, int item, int col, const std::wstring& text)
{
    ListView_SetItemText(list, item, col,
                         const_cast<wchar_t*>(text.c_str()));
}

void updateCounters()
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%lld messages", g_received);
    SendMessageW(g_statusBar, SB_SETTEXT, 1, (LPARAM)buf);
    swprintf(buf, 64, L"%lld CoT sent", g_cotSent);
    SendMessageW(g_statusBar, SB_SETTEXT, 2, (LPARAM)buf);
    swprintf(buf, 64, L"%zu tracks", g_tracks.size());
    SendMessageW(g_statusBar, SB_SETTEXT, 3, (LPARAM)buf);
}

void updateDestLabel()
{
    std::wstring label = g_cfg.cotEnabled
        ? L"CoT \x2192 " + widen(g_cfg.cotHost + ":" + g_cfg.cotPort) +
          (g_ctl.protobuf ? L" (protobuf)" : L" (XML)")
        : L"CoT disabled";
    SendMessageW(g_statusBar, SB_SETTEXT, 4, (LPARAM)label.c_str());
}

void startFeed()
{
    g_feedActive = true;
    SetWindowTextW(g_btnConnect, L"Disconnect");
    SendMessageW(g_editHost, EM_SETREADONLY, TRUE, 0);
    SendMessageW(g_editPort, EM_SETREADONLY, TRUE, 0);
    g_ctl.stop = false;
    g_feedThread = std::thread([] {
        runFeed(g_cfg, g_ctl,
            [](const std::string& line, CotStatus cotStatus) {
                if (auto row = parseRow(line)) {
                    auto* m = new RowMsg{*row, cotStatus};
                    if (!PostMessageW(g_wnd, WM_APP_ROW, 0, (LPARAM)m))
                        delete m;
                }
            },
            [](const std::string& status) {
                auto* s = new std::string(status);
                if (!PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM)s))
                    delete s;
            });
    });
}

void stopFeed()
{
    g_ctl.requestStop();
    if (g_feedThread.joinable())
        g_feedThread.join();
    g_feedActive = false;
    SetWindowTextW(g_btnConnect, L"Connect");
    SendMessageW(g_editHost, EM_SETREADONLY, FALSE, 0);
    SendMessageW(g_editPort, EM_SETREADONLY, FALSE, 0);
}

// Connect/Disconnect toggle. Connect uses the address in the edit boxes.
void toggleConnection()
{
    if (g_feedActive) {
        stopFeed();
        return;
    }
    std::string host = windowText(g_editHost);
    std::string port = windowText(g_editPort);
    if (host.empty() || port.empty()) {
        MessageBoxW(g_wnd, L"Enter the ADS-B server IP and port.",
                    L"ADSBtoCOT", MB_ICONWARNING);
        return;
    }
    g_cfg.adsbHost = host;
    g_cfg.adsbPort = port;
    SetWindowTextW(g_wnd,
        (L"ADSBtoCOT " + widen(ADSBTOCOT_VERSION) + L" \x2014 " +
         widen(host + ":" + port)).c_str());
    saveSettings();
    startFeed();
}

// --- Log tab --------------------------------------------------------------

void appendLogRow(const RowMsg& m)
{
    int count = ListView_GetItemCount(g_logList);
    bool atBottom = ListView_GetTopIndex(g_logList) +
                    ListView_GetCountPerPage(g_logList) >= count;

    std::wstring cells[] = {
        widen(m.row.time), widen(m.row.hex),  widen(m.row.flight),
        widen(m.row.squawk), widen(m.row.alt), widen(m.row.gs),
        widen(m.row.trk), widen(m.row.lat),   widen(m.row.lon),
        widen(m.row.rssi), widen(m.row.type),
        widen(cotStatusLabel(m.cotStatus)),
    };

    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = count;
    item.pszText = const_cast<wchar_t*>(cells[0].c_str());
    int idx = ListView_InsertItem(g_logList, &item);
    for (int i = 1; i < (int)std::size(cells); ++i)
        setCell(g_logList, idx, i, cells[i]);

    // Trimming shifts every row up, which visibly scrolls the view even with
    // tail-following off — suspend it while paused and catch up afterwards.
    if (!g_paused)
        while (ListView_GetItemCount(g_logList) > MAX_LOG_ROWS)
            ListView_DeleteItem(g_logList, 0);

    // Follow the tail unless paused or the user scrolled up to read history.
    if (!g_paused && atBottom)
        ListView_EnsureVisible(g_logList,
                               ListView_GetItemCount(g_logList) - 1, FALSE);
}

// --- Tracks tab -------------------------------------------------------------

void updateTrack(const AdsbRow& row)
{
    auto it = g_tracks.find(row.hex);
    int idx;
    long long msgs;
    if (it == g_tracks.end()) {
        idx = ListView_GetItemCount(g_trackList);
        std::wstring icao = widen(row.hex);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.pszText = const_cast<wchar_t*>(icao.c_str());
        idx = ListView_InsertItem(g_trackList, &item);
        g_tracks[row.hex] = {idx, 1, time(nullptr)};
        msgs = 1;
    } else {
        idx = it->second.item;
        msgs = ++it->second.msgs;
        it->second.lastSeen = time(nullptr);
    }

    // Update fields present in this message; keep last known values for the
    // rest (position/velocity arrive in different messages than callsign).
    const std::string* vals[] = {
        &row.flight, &row.squawk, &row.alt, &row.gs, &row.trk,
        &row.lat, &row.lon, &row.rssi, &row.type,
    };
    for (int i = 0; i < (int)std::size(vals); ++i)
        if (!vals[i]->empty())
            setCell(g_trackList, idx, i + 1, widen(*vals[i]));

    wchar_t buf[24];
    swprintf(buf, 24, L"%lld", msgs);
    setCell(g_trackList, idx, COL_TRK_MSGS, buf);
    setCell(g_trackList, idx, COL_TRK_LAST, widen(row.time));
    setCell(g_trackList, idx, COL_TRK_AGE, L"0");
}

// Once per second: refresh each track's age and drop tracks older than the
// ageout threshold in the top bar.
void ageTick()
{
    wchar_t txt[16];
    GetWindowTextW(g_editAgeout, txt, 16);
    int ageout = _wtoi(txt);                        // <= 0 disables ageout

    time_t now = time(nullptr);
    std::vector<std::pair<int, std::string>> stale; // {row index, ICAO}
    for (auto& [hex, t] : g_tracks) {
        long age = (long)(now - t.lastSeen);
        if (ageout > 0 && age >= ageout) {
            stale.push_back({t.item, hex});
        } else {
            wchar_t buf[16];
            swprintf(buf, 16, L"%ld", age);
            setCell(g_trackList, t.item, COL_TRK_AGE, buf);
        }
    }
    if (stale.empty()) return;

    // Delete bottom-up, then re-sync stored indices past each removed row.
    std::sort(stale.begin(), stale.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [idx, hex] : stale) {
        ListView_DeleteItem(g_trackList, idx);
        g_tracks.erase(hex);
        for (auto& [h2, t2] : g_tracks)
            if (t2.item > idx) --t2.item;
    }
    updateCounters();
}

// Attach a (multi-line) tooltip to a control, creating the shared tooltip
// window on first use. TTF_SUBCLASS lets the tooltip relay hover messages
// itself, so no per-message forwarding is needed.
void addTooltip(HWND ctl, const wchar_t* text)
{
    if (!g_tooltip) {
        g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 320);  // enable wrap
        SendMessageW(g_tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// --- Settings persistence ----------------------------------------------------

std::wstring settingsPath()
{
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? appdata : L".";
    dir += L"\\ADSBtoCOT";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\adsbtocot.ini";
}

Settings loadSettings()
{
    Settings s;
    std::wstring p = settingsPath();
    wchar_t buf[256];
    auto str = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& d) {
        GetPrivateProfileStringW(sec, key, d.c_str(), buf, 256, p.c_str());
        return std::wstring(buf);
    };
    auto num = [&](const wchar_t* sec, const wchar_t* key, int d) {
        return (int)GetPrivateProfileIntW(sec, key, d, p.c_str());
    };
    s.host    = str(L"adsb", L"host", s.host);
    s.port    = str(L"adsb", L"port", s.port);
    s.cotHost = str(L"cot", L"host", s.cotHost);
    s.cotPort = str(L"cot", L"port", s.cotPort);
    s.ageout  = str(L"tracks", L"ageout", s.ageout);
    s.format  = num(L"cot", L"format", s.format);
    s.rate    = num(L"cot", L"rate", s.rate);
    s.x = num(L"window", L"x", s.x);
    s.y = num(L"window", L"y", s.y);
    s.w = num(L"window", L"w", s.w);
    s.h = num(L"window", L"h", s.h);
    s.maximized = num(L"window", L"max", 0);
    return s;
}

// Snapshot every control's current value to the INI. Called on changes and
// on close so settings survive a restart.
void saveSettings()
{
    std::wstring p = settingsPath();
    auto str = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        WritePrivateProfileStringW(sec, key, v.c_str(), p.c_str());
    };
    auto num = [&](const wchar_t* sec, const wchar_t* key, int v) {
        str(sec, key, std::to_wstring(v));
    };

    wchar_t buf[256];
    GetWindowTextW(g_editHost, buf, 256);   str(L"adsb", L"host", buf);
    GetWindowTextW(g_editPort, buf, 256);   str(L"adsb", L"port", buf);
    GetWindowTextW(g_editAgeout, buf, 256); str(L"tracks", L"ageout", buf);
    str(L"cot", L"host", widen(g_cfg.cotHost));
    str(L"cot", L"port", widen(g_cfg.cotPort));
    num(L"cot", L"format", (int)SendMessageW(g_cmbFormat, CB_GETCURSEL, 0, 0));
    num(L"cot", L"rate",   (int)SendMessageW(g_cmbRate,  CB_GETCURSEL, 0, 0));

    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (GetWindowPlacement(g_wnd, &wp)) {
        RECT& r = wp.rcNormalPosition;
        num(L"window", L"x", r.left);
        num(L"window", L"y", r.top);
        num(L"window", L"w", r.right - r.left);
        num(L"window", L"h", r.bottom - r.top);
        num(L"window", L"max", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
}

// --- Layout / creation -------------------------------------------------------

void layout()
{
    SendMessageW(g_statusBar, WM_SIZE, 0, 0);   // status bar self-positions
    RECT rc, sb;
    GetClientRect(g_wnd, &rc);
    GetWindowRect(g_statusBar, &sb);
    int sbH = sb.bottom - sb.top;

    MoveWindow(g_tab, 0, TOPBAR_H, rc.right,
               rc.bottom - TOPBAR_H - sbH, TRUE);

    RECT disp{0, TOPBAR_H, rc.right, rc.bottom - sbH};
    TabCtrl_AdjustRect(g_tab, FALSE, &disp);
    for (HWND list : {g_logList, g_trackList})
        MoveWindow(list, disp.left, disp.top, disp.right - disp.left,
                   disp.bottom - disp.top, TRUE);
}

void showActiveTab()
{
    int sel = TabCtrl_GetCurSel(g_tab);
    ShowWindow(g_logList, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_trackList, sel == 1 ? SW_SHOW : SW_HIDE);
}

HWND makeListView(HWND parent, HINSTANCE inst, intptr_t id)
{
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, (HMENU)id, inst, nullptr);
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(list, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    return list;
}

void addColumns(HWND list, const std::pair<const wchar_t*, int>* cols, int n)
{
    for (int i = 0; i < n; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(cols[i].first);
        c.cx = cols[i].second;
        c.iSubItem = i;
        ListView_InsertColumn(list, i, &c);
    }
}

void createControls(HWND wnd)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_monoFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    // --- top bar: server address, connect, output format, pause ----------
    auto mkStatic = [&](const wchar_t* text, int x, int w) {
        HWND h = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            x, 8, w, 22, wnd, nullptr, inst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        return h;
    };

    mkStatic(L"ADS-B server:", 8, 76);
    g_editHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        widen(g_cfg.adsbHost).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        88, 8, 120, 22, wnd, (HMENU)(INT_PTR)ID_EDIT_HOST, inst, nullptr);

    mkStatic(L"Port:", 218, 32);
    g_editPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        widen(g_cfg.adsbPort).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        252, 8, 60, 22, wnd, (HMENU)(INT_PTR)ID_EDIT_PORT, inst, nullptr);

    g_btnConnect = CreateWindowExW(0, L"BUTTON", L"Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        322, 7, 80, 24, wnd, (HMENU)(INT_PTR)ID_BTN_CONN, inst, nullptr);

    mkStatic(L"Output:", 420, 44);
    g_cmbFormat = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        466, 7, 140, 200, wnd, (HMENU)(INT_PTR)ID_CMB_FORMAT, inst, nullptr);
    SendMessageW(g_cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"CoT XML");
    SendMessageW(g_cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"TAK Protobuf");
    SendMessageW(g_cmbFormat, CB_SETCURSEL, g_settings.format, 0);
    g_ctl.protobuf = g_settings.format == 1;

    g_btnPause = CreateWindowExW(0, L"BUTTON", L"Pause",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        624, 7, 80, 24, wnd, (HMENU)(INT_PTR)ID_BTN_PAUSE, inst, nullptr);

    mkStatic(L"Ageout s:", 722, 54);
    g_editAgeout = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        g_settings.ageout.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        778, 8, 48, 22, wnd, (HMENU)(INT_PTR)ID_EDIT_AGEOUT, inst, nullptr);

    mkStatic(L"Rate:", 838, 34);
    g_cmbRate = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        874, 7, 118, 200, wnd, (HMENU)(INT_PTR)ID_CMB_RATE, inst, nullptr);
    SendMessageW(g_cmbRate, CB_ADDSTRING, 0, (LPARAM)L"No limit");
    SendMessageW(g_cmbRate, CB_ADDSTRING, 0, (LPARAM)L"1 Hz limit");
    SendMessageW(g_cmbRate, CB_ADDSTRING, 0, (LPARAM)L"1 Hz + fill");
    SendMessageW(g_cmbRate, CB_SETCURSEL, g_settings.rate, 0);
    g_ctl.rateMode = g_settings.rate == 0 ? RateMode::NoLimit
                   : g_settings.rate == 2 ? RateMode::OneHzFill
                                          : RateMode::OneHz;

    for (HWND h : {g_editHost, g_editPort, g_btnConnect, g_cmbFormat,
                   g_btnPause, g_editAgeout, g_cmbRate})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

    addTooltip(g_cmbRate,
        L"CoT send rate per aircraft.\r\n\r\n"
        L"No limit: forward every ADS-B message that has a position "
        L"(1 in / 1 out) — highest traffic.\r\n\r\n"
        L"1 Hz limit: at most one event per aircraft per second; extra "
        L"updates that second show “dup” in the log. Matches the rate TAK "
        L"clients expect.\r\n\r\n"
        L"1 Hz + fill: like 1 Hz, but when an aircraft sends no fresh "
        L"message for a second its last position is re-sent as a “fill” "
        L"heartbeat, so every track updates at a steady 1 Hz.");

    // --- tabs -------------------------------------------------------------
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, TOPBAR_H, 0, 0, wnd, (HMENU)(INT_PTR)ID_TAB, inst, nullptr);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    TCITEMW tie{};
    tie.mask = TCIF_TEXT;
    tie.pszText = const_cast<wchar_t*>(L"Log");
    TabCtrl_InsertItem(g_tab, 0, &tie);
    tie.pszText = const_cast<wchar_t*>(L"Tracks");
    TabCtrl_InsertItem(g_tab, 1, &tie);

    // --- log tab ------------------------------------------------------------
    g_logList = makeListView(wnd, inst, 1);
    const std::pair<const wchar_t*, int> logCols[] = {
        {L"Time", 75}, {L"ICAO", 65}, {L"Flight", 85}, {L"Squawk", 65},
        {L"Alt ft", 65}, {L"GS kt", 60}, {L"Track", 60}, {L"Lat", 90},
        {L"Lon", 95}, {L"RSSI", 55}, {L"Type", 100}, {L"CoT", 60},
    };
    addColumns(g_logList, logCols, (int)std::size(logCols));

    // --- tracks tab -----------------------------------------------------------
    g_trackList = makeListView(wnd, inst, 2);
    const std::pair<const wchar_t*, int> trackCols[] = {
        {L"ICAO", 65}, {L"Flight", 85}, {L"Squawk", 65}, {L"Alt ft", 65},
        {L"GS kt", 60}, {L"Track", 60}, {L"Lat", 90}, {L"Lon", 95},
        {L"RSSI", 55}, {L"Type", 100}, {L"Msgs", 60}, {L"Last seen", 80},
        {L"Age s", 55},
    };
    addColumns(g_trackList, trackCols, (int)std::size(trackCols));

    showActiveTab();

    // --- status bar ---------------------------------------------------------
    g_statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, wnd, (HMENU)3, inst, nullptr);
    int parts[5] = {280, 420, 540, 640, -1};
    SendMessageW(g_statusBar, SB_SETPARTS, 5, (LPARAM)parts);
    SendMessageW(g_statusBar, SB_SETTEXT, 0, (LPARAM)L"starting...");
    updateDestLabel();
    updateCounters();
}

LRESULT CALLBACK wndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        createControls(wnd);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_AGE) {
            ageTick();
            return 0;
        }
        break;

    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == ID_TAB &&
            ((NMHDR*)lp)->code == TCN_SELCHANGE) {
            showActiveTab();
            return 0;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_CONN && HIWORD(wp) == BN_CLICKED) {
            toggleConnection();
            return 0;
        }
        if (LOWORD(wp) == ID_CMB_FORMAT && HIWORD(wp) == CBN_SELCHANGE) {
            g_ctl.protobuf =
                SendMessageW(g_cmbFormat, CB_GETCURSEL, 0, 0) == 1;
            updateDestLabel();
            saveSettings();
            return 0;
        }
        if (LOWORD(wp) == ID_CMB_RATE && HIWORD(wp) == CBN_SELCHANGE) {
            switch (SendMessageW(g_cmbRate, CB_GETCURSEL, 0, 0)) {
            case 0:  g_ctl.rateMode = RateMode::NoLimit;   break;
            case 2:  g_ctl.rateMode = RateMode::OneHzFill; break;
            default: g_ctl.rateMode = RateMode::OneHz;     break;
            }
            saveSettings();
            return 0;
        }
        if (LOWORD(wp) == ID_BTN_PAUSE && HIWORD(wp) == BN_CLICKED) {
            g_paused = !g_paused;
            SetWindowTextW(g_btnPause, g_paused ? L"Resume" : L"Pause");
            if (!g_paused) {
                // Apply the row cap that was suspended during the pause,
                // then snap back to the tail.
                while (ListView_GetItemCount(g_logList) > MAX_LOG_ROWS)
                    ListView_DeleteItem(g_logList, 0);
                if (ListView_GetItemCount(g_logList) > 0)
                    ListView_EnsureVisible(g_logList,
                        ListView_GetItemCount(g_logList) - 1, FALSE);
            }
            return 0;
        }
        break;

    case WM_APP_ROW: {
        RowMsg* m = (RowMsg*)lp;
        bool isFill = m->cotStatus == CotStatus::Fill;
        appendLogRow(*m);
        // A fill is a synthetic heartbeat, not a received message: show it in
        // the log and count it as sent, but don't advance the message counter
        // or the track's last-seen/age (which track real receptions).
        if (!isFill) {
            updateTrack(m->row);
            ++g_received;
        }
        if (m->cotStatus == CotStatus::Sent || isFill) ++g_cotSent;
        updateCounters();
        delete m;
        return 0;
    }

    case WM_APP_STATUS: {
        std::string* s = (std::string*)lp;
        SendMessageW(g_statusBar, SB_SETTEXT, 0, (LPARAM)widen(*s).c_str());
        delete s;
        return 0;
    }

    case WM_DESTROY:
        saveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd)
{
    // Saved settings supply the defaults; explicit command-line args win.
    g_settings = loadSettings();
    g_cfg.adsbHost = __argc > 1 ? __argv[1] : narrow(g_settings.host);
    g_cfg.adsbPort = __argc > 2 ? __argv[2] : narrow(g_settings.port);
    g_cfg.cotHost  = __argc > 3 ? __argv[3] : narrow(g_settings.cotHost);
    g_cfg.cotPort  = __argc > 4 ? __argv[4] : narrow(g_settings.cotPort);
    g_cfg.cotEnabled = g_cfg.cotHost != "-";

    INITCOMMONCONTROLSEX icc{sizeof icc,
        ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
        ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AdsbToCotWnd";
    RegisterClassW(&wc);

    std::wstring title = L"ADSBtoCOT " + widen(ADSBTOCOT_VERSION) +
        L" \x2014 " + widen(g_cfg.adsbHost + ":" + g_cfg.adsbPort);
    int winW = g_settings.w > 200 ? g_settings.w : 1010;
    int winH = g_settings.h > 150 ? g_settings.h : 660;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW, g_settings.x, g_settings.y, winW, winH,
        nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, g_settings.maximized ? SW_SHOWMAXIMIZED : showCmd);
    SetTimer(g_wnd, TIMER_AGE, 1000, nullptr);

    startFeed();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Dialog navigation (Tab between controls, Enter = Connect), except
        // when a list has focus so its own keyboard handling still works.
        HWND focus = GetFocus();
        if (focus != g_logList && focus != g_trackList &&
            IsDialogMessageW(g_wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stopFeed();
    return 0;
}
