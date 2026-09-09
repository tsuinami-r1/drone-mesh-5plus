# Firmware Binaries — Level 2 stations and home node

Pre-built `.bin` files for flashing without PlatformIO.

> **These are default-configuration builds.** Several settings are compile-time
> `#define`s, so a prebuilt binary carries whatever the source defaults are — you
> cannot change them by flashing. Rebuild from source if you need to alter:
>
> | Setting | Default | Where | Why it matters |
> |---|---|---|---|
> | `WIFI_COUNTRY_CC` / `WIFI_CHAN_START` / `WIFI_CHAN_COUNT` | `HK`, 1, 13 | top of each detection `main.cpp` | Bounds which channels are scanned. `1–13` suits HK/EU; US/FCC is `1, 11`. Nodes are receive-only, so this has no transmit implications. |

Level 1 (analog FPV, RX5808) binaries are on the `level1-station` branch under
`firmware/`, with their own `NODE_ID` caveat.

## Binary → Source Mapping

| File | Project | PIO Environment | Hardware | Notes |
|------|---------|-----------------|----------|-------|
| `xiao-c5-dualband.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32c5` | XIAO ESP32-C5 | 2.4+5GHz WiFi 6, BLE 5.0 + Coded PHY. Heltec UART on D4/D5 (GPIO6/GPIO7), same header pins as the S3 builds. |
| `xiao-s3-nimble.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, NimBLE + Coded PHY. **Preferred S3 build** — the only one with both channel hopping and Coded PHY. |
| `xiao-s3-single.bin` | `remoteid-mesh` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, single-core |
| `xiao-s3-dualcore.bin` | `remoteid-mesh-dualcore` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, dual-core tasks |
| `xiao-s3-node-remote.bin` | `node-mode-dualcore` | `remote_node` | XIAO ESP32-S3 | Detection node, sends JSON to Heltec mesh |
| `xiao-s3-node-home.bin` | `node-mode-dualcore` | `home_node` | XIAO ESP32-S3 | Home node, UART bridge only (no detection) |

## Where PlatformIO puts the built binary

After `pio run -e <env>`, copy:

```
remoteid-c5-5g/.pio/build/seeed_xiao_esp32c5/firmware.bin        →  xiao-c5-dualband.bin
remoteid-c5-5g/.pio/build/seeed_xiao_esp32s3/firmware.bin        →  xiao-s3-nimble.bin
remoteid-mesh/.pio/build/seeed_xiao_esp32s3/firmware.bin         →  xiao-s3-single.bin
remoteid-mesh-dualcore/.pio/build/seeed_xiao_esp32s3/firmware.bin →  xiao-s3-dualcore.bin
node-mode-dualcore/.pio/build/remote_node/firmware.bin           →  xiao-s3-node-remote.bin
node-mode-dualcore/.pio/build/home_node/firmware.bin             →  xiao-s3-node-home.bin
```

## Flashing without PlatformIO (esptool)

```bash
esptool.py --chip esp32s3 --port COM<X> --baud 921600 write_flash 0x0 <file>.bin
esptool.py --chip esp32c5 --port COM<X> --baud 115200 write_flash 0x0 <file>.bin
```

> **ESP32-C5 boot mode:** if upload fails to connect, hold **BOOT**, tap **RESET**,
> release **BOOT**, then re-run. The S3 does not need this.

## Verifying a flashed node

On the serial monitor at 115200 you should see, within a few seconds of boot:

- a startup banner naming the board and mode, then
- `[SCAN] Channel hopping: N primary + M secondary, dwell X ms`

Any `channel N rejected by regulatory domain` warnings mean `WIFI_CHAN_START` /
`WIFI_CHAN_COUNT` do not cover a channel in the scan list — coverage is silently
reduced until that is corrected.
