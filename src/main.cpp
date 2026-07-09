// adsbtocot — console front-end. Connects to a readsb/dump1090 JSON position
// feed, prints a log of received ADS-B messages, and forwards each position
// as a Cursor-on-Target event over UDP (default: TAK mesh SA multicast
// 239.2.3.1:6969).
//
// Usage: adsbtocot [adsb_host] [adsb_port] [cot_host] [cot_port] [xml|proto]
//        defaults:  192.168.1.135  30154     239.2.3.1  6969       xml
//        pass "-" as cot_host to disable CoT output (log only)

#include "adsb_core.h"

#include <cstdio>
#include <cstring>

#ifndef ADSBTOCOT_VERSION
#define ADSBTOCOT_VERSION "dev"
#endif

static void printHeader()
{
    printf("%-8s  %-6s  %-9s  %-6s  %-6s  %-5s  %-6s  %-10s %-11s %-6s  %-11s %s\n",
           "TIME", "ICAO", "FLIGHT", "SQUAWK", "ALT", "GS", "TRK",
           "LAT", "LON", "RSSI", "TYPE", "COT");
    printf("--------------------------------------------------------------"
           "--------------------------------------------\n");
}

int main(int argc, char** argv)
{
    FeedConfig cfg;
    cfg.adsbHost = argc > 1 ? argv[1] : "192.168.1.135";
    cfg.adsbPort = argc > 2 ? argv[2] : "30154";
    cfg.cotHost  = argc > 3 ? argv[3] : "239.2.3.1";
    cfg.cotPort  = argc > 4 ? argv[4] : "6969";
    cfg.cotEnabled = cfg.cotHost != "-";

    static FeedControl ctl;
    ctl.protobuf = argc > 5 && strcmp(argv[5], "proto") == 0;

    printf("adsbtocot %s — ADS-B from %s:%s, CoT to %s\n\n",
           ADSBTOCOT_VERSION, cfg.adsbHost.c_str(), cfg.adsbPort.c_str(),
           cfg.cotEnabled
               ? (cfg.cotHost + ":" + cfg.cotPort +
                  (ctl.protobuf ? " (protobuf)" : " (XML)")).c_str()
               : "(disabled)");
    printHeader();

    bool ok = runFeed(cfg, ctl,
        [](const std::string& line, CotStatus cotStatus) {
            auto row = parseRow(line);
            if (!row) return;
            printf("%-8s  %-6s  %-9s  %-6s  %-6s  %-5s  %-6s  %-10s %-11s %-6s  %-11s %s\n",
                   row->time.c_str(), row->hex.c_str(), row->flight.c_str(),
                   row->squawk.c_str(), row->alt.c_str(), row->gs.c_str(),
                   row->trk.c_str(), row->lat.c_str(), row->lon.c_str(),
                   row->rssi.c_str(), row->type.c_str(),
                   cotStatusLabel(cotStatus));
            fflush(stdout);
        },
        [](const std::string& status) {
            fprintf(stderr, "%s\n", status.c_str());
        });
    return ok ? 0 : 1;
}
