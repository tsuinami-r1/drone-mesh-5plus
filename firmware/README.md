# Firmware Binaries — Level 1 stations

Pre-built `.bin` files for flashing without PlatformIO.

> **These are default-configuration builds**: `NODE_ID` = `RX01`,
> `STATION_HEADING_DEG` = `0`, the default 2-bit `SECTOR_SWITCH_TABLE`, and the
> unmeasured RSSI calibration line. Every one of those is a per-station value, so
> these binaries are for **bench testing only**. A fielded station is always
> rebuilt from source with its own `config.h`.

Level 2 (Wi-Fi/BLE Remote ID) and home-node binaries are on the `main` branch under
`firmware/`.

## Binary → Source Mapping

| File | Project | PIO Environment | Hardware |
|------|---------|-----------------|----------|
| `level1-v3-c5phy-factory.bin` | `level1-c5phy` | `seeed_xiao_esp32c5` | **v3 prototype**: XIAO ESP32-C5 as the receiver + SP4T. Merged image (bootloader, partition table, app), flash at `0x0` after a full erase. **Untested on hardware** |
| `level1-v2-s3.bin` | `level1-analog-fpv` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 + RX5808 + SP4T + sync separator |
| `level1-v2-c5.bin` | `level1-analog-fpv` | `seeed_xiao_esp32c5` | XIAO ESP32-C5, same hardware (see the C5 pin note in the README) |

## Where PlatformIO puts the built binary

After `pio run -e <env>`, copy:

```
level1-c5phy/.pio/build/seeed_xiao_esp32c5/firmware.factory.bin     →  level1-v3-c5phy-factory.bin
level1-analog-fpv/.pio/build/seeed_xiao_esp32s3/firmware.bin        →  level1-v2-s3.bin
level1-analog-fpv/.pio/build/seeed_xiao_esp32c5/firmware.bin        →  level1-v2-c5.bin
```

## Flashing without PlatformIO (esptool)

v3 — **erase first, always** (stale Wi-Fi PHY calibration data leaves the receiver deaf):

```bash
esptool.py --chip esp32c5 --port COM<X> erase_flash
esptool.py --chip esp32c5 --port COM<X> --baud 460800 write_flash 0x0 level1-v3-c5phy-factory.bin
```

v2:

```bash
esptool.py --chip esp32s3 --port COM<X> --baud 921600 write_flash 0x0 level1-v2-s3.bin
esptool.py --chip esp32c5 --port COM<X> --baud 115200 write_flash 0x0 level1-v2-c5.bin
```

> **ESP32-C5 boot mode:** if upload fails to connect, hold **BOOT**, tap **RESET**,
> release **BOOT**, then re-run. The S3 does not need this.

## Verifying a flashed node

On the serial monitor at 115200 you should see, within a few seconds of boot:

v3:

```json
{"info":"c5phy v3 station ready","node_id":"RX01","receiver":"c5phy","hw":"v3","channels":40,"sectors":4,"heading":0,"threshold_dbm":-87.0,"threshold_level_db":8.0,"q_min":40,"peak_pick":1,"video":1,"bw40":1,"gain_max":62,"window_us":409,"rf":true}
```

`"rf":true` is the receiver alive; `"rf":false` is followed by an `{"info":"error",...}`
line. Type `?` for the bench console.

v2:

```json
{"info":"rx5808 v2 station ready","node_id":"RX01","receiver":"rx5808","hw":"v2","channels":40,"sectors":4,"heading":0,"threshold":600,"peak_pick":1,"video":1,"fine_tune":1}
```

followed by a `{"heartbeat":true,...}` line every 60 s and `{"type":"analog_fm",...}`
lines whenever a channel clears the threshold.
