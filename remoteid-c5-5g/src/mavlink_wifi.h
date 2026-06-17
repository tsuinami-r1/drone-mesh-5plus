#ifndef MAVLINK_WIFI_H
#define MAVLINK_WIFI_H
/*
 * Passive MAVLink GPS extraction from 802.11 data frames captured in
 * promiscuous mode.  Parses IPv4/UDP/MAVLink v1+v2, handles QoS and from-DS
 * addressing.  WPA/WEP-protected frames are silently skipped.
 *
 * Limitation: only detects drones on the channel the ESP32 is currently
 * scanning (channel 6 by default).  Drones on other channels are missed.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define MAV_GCS_PORT                14550
#define MAV_MSG_GLOBAL_POSITION_INT 33
#define MAV_V1_STX                  0xFE
#define MAV_V2_STX                  0xFD

typedef struct {
    double lat;     /* degrees */
    double lon;     /* degrees */
    float  alt_msl; /* metres MSL */
    float  alt_agl; /* metres AGL */
    float  hdg;     /* degrees 0-360 */
} mav_gps_t;

/* Scan a MAVLink byte stream for the first GLOBAL_POSITION_INT message. */
static inline bool mav_parse_gps(const uint8_t *buf, int len, mav_gps_t *out) {
    for (int i = 0; i < len; ) {
        if (buf[i] == MAV_V1_STX) {
            if (i + 8 > len) break;
            int plen = buf[i + 1];
            if (i + 8 + plen > len) break;
            if (buf[i + 5] == MAV_MSG_GLOBAL_POSITION_INT && plen >= 28) {
                const uint8_t *p = buf + i + 6;
                int32_t lat_e7, lon_e7, alt_mm, rel_mm;
                uint16_t hdg_cdeg;
                memcpy(&lat_e7,   p + 4,  4);
                memcpy(&lon_e7,   p + 8,  4);
                memcpy(&alt_mm,   p + 12, 4);
                memcpy(&rel_mm,   p + 16, 4);
                memcpy(&hdg_cdeg, p + 26, 2);
                out->lat     = lat_e7   / 1e7;
                out->lon     = lon_e7   / 1e7;
                out->alt_msl = alt_mm   / 1000.0f;
                out->alt_agl = rel_mm   / 1000.0f;
                out->hdg     = hdg_cdeg / 100.0f;
                return true;
            }
            i += 8 + plen;
        } else if (buf[i] == MAV_V2_STX) {
            if (i + 12 > len) break;
            int plen = buf[i + 1];
            if (i + 12 + plen > len) break;
            uint32_t msg_id = (uint32_t)buf[i+7]
                            | ((uint32_t)buf[i+8] << 8)
                            | ((uint32_t)buf[i+9] << 16);
            if (msg_id == MAV_MSG_GLOBAL_POSITION_INT && plen >= 28) {
                const uint8_t *p = buf + i + 10;
                int32_t lat_e7, lon_e7, alt_mm, rel_mm;
                uint16_t hdg_cdeg;
                memcpy(&lat_e7,   p + 4,  4);
                memcpy(&lon_e7,   p + 8,  4);
                memcpy(&alt_mm,   p + 12, 4);
                memcpy(&rel_mm,   p + 16, 4);
                memcpy(&hdg_cdeg, p + 26, 2);
                out->lat     = lat_e7   / 1e7;
                out->lon     = lon_e7   / 1e7;
                out->alt_msl = alt_mm   / 1000.0f;
                out->alt_agl = rel_mm   / 1000.0f;
                out->hdg     = hdg_cdeg / 100.0f;
                return true;
            }
            i += 12 + plen;
        } else {
            i++;
        }
    }
    return false;
}

/*
 * Extract MAVLink GPS from a raw 802.11 data frame (wifi_promiscuous_pkt_t
 * payload).  Returns true on success; fills *gps and src_mac_out[6].
 */
static inline bool mav_wifi_extract(const uint8_t *payload, int length,
                                    uint8_t *src_mac_out, mav_gps_t *gps) {
    if (length < 30) return false;

    uint8_t fc0 = payload[0];
    uint8_t fc1 = payload[1];

    /* Type bits (2-3) must be 0b10 = data */
    if ((fc0 & 0x0C) != 0x08) return false;
    /* Skip WPA/WEP-protected frames — MSDU is ciphertext */
    if (fc1 & 0x40) return false;
    /* Skip 4-address WDS frames (rare, complex to parse) */
    if ((fc1 & 0x03) == 0x03) return false;

    /* Source MAC: Addr3 when from-DS (AP→STA), else Addr2 */
    memcpy(src_mac_out, payload + ((fc1 & 0x02) ? 16 : 10), 6);

    /* Header: 24 B base + 2 B QoS control + optional 4 B HTC */
    bool qos = ((fc0 >> 4) & 0x08) != 0;
    bool htc = qos && (fc1 & 0x80);
    int  hdr = 24 + (qos ? 2 : 0) + (htc ? 4 : 0);
    if (length < hdr + 8) return false;

    /* LLC/SNAP: AA AA 03 [OUI 3B] [EtherType 2B] — require IPv4 (0x08 0x00) */
    const uint8_t *llc = payload + hdr;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return false;
    if (llc[6] != 0x08 || llc[7] != 0x00) return false;

    int ip_off = hdr + 8;
    if (length < ip_off + 20) return false;

    const uint8_t *ip = payload + ip_off;
    if ((ip[0] >> 4) != 4) return false; /* IPv4 */
    if (ip[9]        != 0x11) return false; /* UDP */
    int ip_hdr = (ip[0] & 0x0F) * 4;
    if (ip_hdr < 20) return false;

    int udp_off = ip_off + ip_hdr;
    if (length < udp_off + 8) return false;

    const uint8_t *udp = payload + udp_off;
    uint16_t sport = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t dport = ((uint16_t)udp[2] << 8) | udp[3];
    if (dport != MAV_GCS_PORT && sport != MAV_GCS_PORT) return false;

    uint16_t udp_len = ((uint16_t)udp[4] << 8) | udp[5];
    if (udp_len < 8) return false;
    int mav_off = udp_off + 8;
    int mav_len = udp_len - 8;
    if (mav_off + mav_len > length) return false;

    return mav_parse_gps(payload + mav_off, mav_len, gps);
}

#endif /* MAVLINK_WIFI_H */
