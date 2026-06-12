/*
 * dji_droneid.h
 * -----------------------------------------------------------------------------
 * DJI DroneID (Wi-Fi / IE221) parser for the drone-mesh-mapper firmware.
 * Drop into src/ alongside opendroneid.c / wifi.c and call from the existing
 * Wi-Fi promiscuous callback.
 *
 * WHY THIS IS SEPARATE FROM OpenDroneID:
 *   The stock firmware decodes the ASTM/OpenDroneID vendor IE (OUI FA:0B:BC).
 *   DJI broadcasts its OWN, unencrypted telemetry in a DIFFERENT vendor IE
 *   (tag 221) under OUI 26:37:12. Same transport (802.11 beacons), different
 *   payload. So this is a new *decoder*, hooked into the same IE walk.
 *
 * BYTE LAYOUT SOURCE:
 *   Kismet  dot11_parsers / dot11_ie_221_dji_droneid.ksy
 *   (reverse engineered by Freek van Tienen & Jan Dumon). All offsets,
 *   the /174533.0 lat/lon scaling, and the yaw conversion come from there.
 *
 * COVERAGE CAVEAT (read before relying on this):
 *   This catches DJI models that emit DroneID over *Wi-Fi*. The dominant
 *   transport on modern DJI (OcuSync / O3 / O4 in the video band) is OFDM and
 *   needs an SDR (~2.4295 GHz, 15.36 Msps) -- an ESP32 cannot demodulate it.
 *   Treat this as partial fleet coverage, not "all DJI".
 * -----------------------------------------------------------------------------
 */

#ifndef DJI_DRONEID_H
#define DJI_DRONEID_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* OUI that marks a DJI DroneID vendor IE (air order). */
static const uint8_t DJI_OUI[3] = { 0x26, 0x37, 0x12 };

#define DJI_SUBCMD_FLIGHT_REG   0x10   /* telemetry + location           */
#define DJI_SUBCMD_FLIGHT_PURP  0x11   /* operator-entered "purpose" txt */

typedef struct {
    bool     valid;
    uint8_t  version;
    uint16_t seq;
    uint16_t state_info;      /* bitfield: bit0 serial_valid, bit5 in_air, ... */
    char     serial[17];      /* 16 chars + NUL                                 */
    double   lat, lon;        /* degrees                                        */
    double   home_lat, home_lon;
    int16_t  altitude;        /* m, barometric                                  */
    int16_t  height;          /* m AGL                                          */
    int16_t  v_north, v_east, v_up;  /* raw int16; DJI scaling ~cm/s, confirm   */
    double   yaw;             /* radians                                        */
    uint8_t  product_type;    /* numeric model id, needs your own lookup table  */
    char     uuid[21];        /* up to 20 bytes + NUL                           */
} dji_droneid_t;

/* raw lat/lon are radians * 1e7; /174533.0 yields degrees (1e7 / 57.2958). */
static inline double dji_deg(int32_t raw) { return (double)raw / 174533.0; }

/* little-endian helpers (ESP32 is LE, but be explicit so it's portable) */
static inline uint16_t dji_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t  dji_i16(const uint8_t *p) { return (int16_t)dji_u16(p); }
static inline int32_t  dji_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Use in callbacks instead of repeating inline byte literals. */
static inline bool dji_is_oui(const uint8_t *p) {
    return p[0] == DJI_OUI[0] && p[1] == DJI_OUI[1] && p[2] == DJI_OUI[2];
}

/* JSON-safe copy: escapes backslash and double-quote so d->serial is safe in JSON strings. */
static inline void dji_escape_str(const char *src, char *dst, size_t dstlen) {
    size_t i = 0, j = 0;
    while (src[i] && j + 2 < dstlen) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\';
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

/*
 * payload     = bytes AFTER the 3-byte OUI, i.e. starting at vendor_type.
 * payload_len = IE tag length minus the 3 OUI bytes.
 * Returns true only for a parsed 0x10 flight-reg (telemetry) record.
 */
static inline bool dji_parse_droneid(const uint8_t *payload, int payload_len,
                                     dji_droneid_t *out)
{
    memset(out, 0, sizeof(*out));
    if (payload_len < 4) return false;

    /* [0]=vendor_type [1]=unk1 [2]=unk2 [3]=subcommand */
    uint8_t subcommand = payload[3];
    if (subcommand != DJI_SUBCMD_FLIGHT_REG) return false; /* skip 0x11 here */

    const uint8_t *r = payload + 4;
    int rlen = payload_len - 4;

    /* fixed portion through uuid_len, in bytes:
     *   version(1) seq(2) state(2) serial(16) lon(4) lat(4)
     *   + 8 x s2le (alt,height,vN,vE,vUp,pitch,roll,yaw) = 16
     *   + home_lon(4) home_lat(4) product_type(1) uuid_len(1)
     *   = 55. uuid (nominally 20 more) is length-checked separately below. */
    const int FIXED = 1 + 2 + 2 + 16 + 4 + 4 + (2 * 8) + 4 + 4 + 1 + 1;
    if (rlen < FIXED) return false;

    int p = 0;
    out->version    = r[p];          p += 1;
    out->seq        = dji_u16(r + p); p += 2;
    out->state_info = dji_u16(r + p); p += 2;

    memcpy(out->serial, r + p, 16); out->serial[16] = 0; p += 16;

    out->lon = dji_deg(dji_i32(r + p)); p += 4;
    out->lat = dji_deg(dji_i32(r + p)); p += 4;

    out->altitude = dji_i16(r + p); p += 2;
    out->height   = dji_i16(r + p); p += 2;
    out->v_north  = dji_i16(r + p); p += 2;
    out->v_east   = dji_i16(r + p); p += 2;
    out->v_up     = dji_i16(r + p); p += 2;

    p += 2;                              /* pitch (unused)            */
    p += 2;                              /* roll  (unused)            */
    int16_t raw_yaw = dji_i16(r + p); p += 2;
    out->yaw = ((double)raw_yaw / 100.0) / 57.296;

    out->home_lon = dji_deg(dji_i32(r + p)); p += 4;
    out->home_lat = dji_deg(dji_i32(r + p)); p += 4;

    out->product_type = r[p]; p += 1;
    uint8_t uuid_len = r[p];  p += 1;
    if (uuid_len > 20) uuid_len = 20;
    if (p + uuid_len <= rlen) {
        memcpy(out->uuid, r + p, uuid_len);
        out->uuid[uuid_len] = 0;
    }

    out->valid = true;
    return true;
}

/*
 * Emit a detection in the mapper's existing serial JSON schema
 * (mac/rssi/drone_lat/drone_long/drone_altitude/pilot_lat/pilot_long/basic_id).
 *
 * NOTE ON "pilot": the Wi-Fi IE221 record does NOT carry the live operator /
 * phone location the way OpenDroneID's System message does. It only has the
 * HOME point. We surface home as the best-available operator proxy AND under
 * its own keys, plus an id_type so the mapper can tell DJI from ASTM tracks.
 * Mapper needs no change to plot it; add handling for home_/id_type if wanted.
 */
static inline void dji_emit_json(const uint8_t mac[6], int8_t rssi,
                                 const dji_droneid_t *d,
                                 char *buf, size_t buflen)
{
    char serial_esc[34]; /* 16 bytes worst-case doubled + NUL */
    dji_escape_str(d->serial, serial_esc, sizeof(serial_esc));
    snprintf(buf, buflen,
        "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,"
        "\"id_type\":\"DJI\",\"basic_id\":\"%s\","
        "\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,"
        "\"height\":%d,\"home_lat\":%.6f,\"home_long\":%.6f,"
        "\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"product_type\":%d}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi,
        serial_esc,
        d->lat, d->lon, d->altitude,
        d->height, d->home_lat, d->home_lon,
        d->home_lat, d->home_lon,        /* pilot_* = home (proxy) */
        d->product_type);
}

#endif /* DJI_DRONEID_H */
