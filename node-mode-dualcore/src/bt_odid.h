/*
 * bt_odid.h
 * -------------------------------------------------------------------------
 * ASTM F3411 BLE advertising data parser for the drone-mesh firmware.
 *
 * BLE advertising data is a sequence of AD structures:
 *   [length (1B)][type (1B)][data ...]
 * The length field includes the type byte but not itself.
 *
 * We walk the sequence looking for type 0x16 (Service Data) with
 * UUID 0xFFFA (little-endian: 0xFA 0xFF).  Two ODID wire formats:
 *
 *   F3411-19:  UUID(2) counter(1) ODID-message(25)
 *   F3411-22a: UUID(2) app_code(1)=0x0D counter(1) ODID-message(25)
 *
 * Fixed-offset parsing (e.g. testing payload[1..4] directly) fails when
 * Service Data is not the first AD element — this walk fixes that.
 * -------------------------------------------------------------------------
 */

#ifndef BT_ODID_H
#define BT_ODID_H

#include <stdint.h>
#include <stdbool.h>

#define BT_ODID_SVC_TYPE    0x16    /* AD type: Service Data */
#define BT_ODID_UUID_LO     0xFA    /* UUID 0xFFFA, little-endian LSB */
#define BT_ODID_UUID_HI     0xFF    /* UUID 0xFFFA, little-endian MSB */
#define BT_ODID_APP_CODE    0x0D    /* F3411-22a application code     */

/*
 * Walk the raw BLE advertising payload and locate the ODID service data.
 *
 * On success: sets *msgs to the first byte of the first ODID message,
 * sets *msgs_len to the remaining service-data length, and returns true.
 * On failure (no ODID service data found): returns false.
 */
static inline bool bt_odid_find_odid(const uint8_t *adv, int adv_len,
                                     const uint8_t **msgs, int *msgs_len)
{
    int pos = 0;
    while (pos < adv_len) {
        int elem_len = adv[pos];
        if (elem_len == 0 || pos + 1 + elem_len > adv_len) break;

        uint8_t elem_type  = adv[pos + 1];
        const uint8_t *data = adv + pos + 2;
        int data_len = elem_len - 1;   /* type byte already consumed */

        if (elem_type == BT_ODID_SVC_TYPE && data_len >= 4) {
            if (data[0] == BT_ODID_UUID_LO && data[1] == BT_ODID_UUID_HI) {
                /* Found RemoteID service data */
                const uint8_t *svc = data + 2;   /* skip UUID */
                int svc_len = data_len - 2;
                if (svc_len < 2) break;
                if (svc[0] == BT_ODID_APP_CODE) {
                    /* F3411-22a: [app_code][counter][msgs...] */
                    if (svc_len < 3) break;
                    *msgs = svc + 2;
                    *msgs_len = svc_len - 2;
                } else {
                    /* F3411-19: [counter][msgs...] */
                    *msgs = svc + 1;
                    *msgs_len = svc_len - 1;
                }
                return (*msgs_len > 0);
            }
        }
        pos += 1 + elem_len;
    }
    return false;
}

#endif /* BT_ODID_H */
