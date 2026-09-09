# Firmware Binaries — Level 1 stations

Pre-built `.bin` files for flashing without PlatformIO.

> **These are default-configuration builds.** `NODE_ID` is a compile-time `#define`
> (default `"RX01"`), so every board flashed from the same binary claims `RX01`. That
> breaks multi-node dedup and range-ring placement in `mesh-mapper.py`. For a fielded
> station **rebuild from source with a unique `NODE_ID`** that matches the paired
> Meshtastic node's shortName/longName. `RSSI_THRESHOLD` (default 600) is also baked
> in; calibrate on site and rebuild.

Level 2 (Wi-Fi/BLE Remote ID) and home-node binaries are on the `main` branch under
`firmware/`.

## Binary → Source Mapping

| File | Project | PIO Environment | Hardware | Notes |
|------|---------|-----------------|----------|-------|
| `rx5808-s3.bin` | `rx5808-detection` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 + RX5808 | 5.8GHz analog FPV sweep. Heltec UART on D4/D5 (GPIO5/GPIO6). |
| `rx5808-c5.bin` | `rx5808-detection` | `seeed_xiao_esp32c5` | XIAO ESP32-C5 + RX5808 | As above, C5 pinout. Heltec UART on D4/D5 (GPIO6/GPIO7). |

## Where PlatformIO puts the built binary

After `pio run -e <env>`, copy:

```
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

```json
{"info":"rx5808 scanner ready","node_id":"RX01","receiver":"rx5808","channels":40,"threshold":600,"peak_pick":1}
```

followed by a `{"heartbeat":true,...}` line every 60 s and `{"type":"analog_fm",...}`
lines whenever a channel clears the threshold.
