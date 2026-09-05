# Firmware Binaries

Pre-built `.bin` files for flashing without PlatformIO.

> **These are default-configuration builds.** Several settings are compile-time
> `#define`s, so a prebuilt binary carries whatever the source defaults are — you
> cannot change them by flashing. Rebuild from source if you need to alter:
>
> | Setting | Default | Where | Why it matters |
> |---|---|---|---|
> | `WIFI_COUNTRY_CC` / `WIFI_CHAN_START` / `WIFI_CHAN_COUNT` | `HK`, 1, 13 | top of each detection `main.cpp` | Bounds which channels are scanned. `1–13` suits HK/EU; US/FCC is `1, 11`. Nodes are receive-only, so this has no transmit implications. |
> | `NODE_ID` | `"RX01"` | `rx5808-detection/src/main.cpp` | Must be **unique per node**, and must match the paired Meshtastic node's shortName/longName for range rings to resolve. Flashing several RX5808 nodes from the same binary makes them all claim `RX01`. |

## Binary → Source Mapping

| File | Project | PIO Environment | Hardware | Notes |
|------|---------|-----------------|----------|-------|
| `xiao-c5-dualband.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32c5` | XIAO ESP32-C5 | 2.4+5GHz WiFi 6, BLE 5.0 + Coded PHY. Heltec UART on D4/D5 (GPIO6/GPIO7), same header pins as the S3 builds. |
| `xiao-s3-nimble.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, NimBLE + Coded PHY. **Preferred S3 build** — the only one with both channel hopping and Coded PHY. |
| `xiao-s3-single.bin` | `remoteid-mesh` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, single-core |
| `xiao-s3-dualcore.bin` | `remoteid-mesh-dualcore` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, dual-core tasks |
| `xiao-s3-node-remote.bin` | `node-mode-dualcore` | `remote_node` | XIAO ESP32-S3 | Detection node, sends JSON to Heltec mesh |
| `xiao-s3-node-home.bin` | `node-mode-dualcore` | `home_node` | XIAO ESP32-S3 | Home node, UART bridge only (no detection) |
| `rx5808-s3.bin` | `rx5808-detection` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 + RX5808 | 5.8GHz analog FPV sweep. **Set `NODE_ID` first** (see above). |
| `rx5808-c5.bin` | `rx5808-detection` | `seeed_xiao_esp32c5` | XIAO ESP32-C5 + RX5808 | As above, C5 pinout |

## Where PlatformIO puts the built binary

After `pio run -e <env>`, copy:

```
remoteid-c5-5g/.pio/build/seeed_xiao_esp32c5/firmware.bin        →  xiao-c5-dualband.bin
remoteid-c5-5g/.pio/build/seeed_xiao_esp32s3/firmware.bin        →  xiao-s3-nimble.bin
remoteid-mesh/.pio/build/seeed_xiao_esp32s3/firmware.bin         →  xiao-s3-single.bin
remoteid-mesh-dualcore/.pio/build/seeed_xiao_esp32s3/firmware.bin →  xiao-s3-dualcore.bin
node-mode-dualcore/.pio/build/remote_node/firmware.bin           →  xiao-s3-node-remote.bin
node-mode-dualcore/.pio/build/home_node/firmware.bin             →  xiao-s3-node-home.bin
rx5808-detection/.pio/build/seeed_xiao_esp32s3/firmware.bin      →  rx5808-s3.bin
rx5808-detection/.pio/build/seeed_xiao_esp32c5/firmware.bin      →  rx5808-c5.bin
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
