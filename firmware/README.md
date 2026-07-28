# Firmware Binaries

Pre-built `.bin` files for flashing without PlatformIO.

## Binary → Source Mapping

| File | Project | PIO Environment | Hardware | Notes |
|------|---------|-----------------|----------|-------|
| `xiao-c5-dualband.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32c5` | XIAO ESP32-C5 | 2.4+5GHz WiFi 6, BLE 5.0 + Coded PHY |
| `xiao-s3-nimble.bin` | `remoteid-c5-5g` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz only, NimBLE + Coded PHY |
| `xiao-s3-single.bin` | `remoteid-mesh` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, single-core |
| `xiao-s3-dualcore.bin` | `remoteid-mesh-dualcore` | `seeed_xiao_esp32s3` | XIAO ESP32-S3 | 2.4GHz, classic BLE, dual-core tasks |
| `xiao-s3-node-remote.bin` | `node-mode-dualcore` | `remote_node` | XIAO ESP32-S3 | Detection node, sends JSON to Heltec V3 |
| `xiao-s3-node-home.bin` | `node-mode-dualcore` | `home_node` | XIAO ESP32-S3 | Home node, UART bridge only (no detection) |

## Where PlatformIO puts the built binary

After `pio run -e <env>`, copy:

```
remoteid-c5-5g/.pio/build/seeed_xiao_esp32c5/firmware.bin  →  xiao-c5-dualband.bin
remoteid-c5-5g/.pio/build/seeed_xiao_esp32s3/firmware.bin  →  xiao-s3-nimble.bin
remoteid-mesh/.pio/build/seeed_xiao_esp32s3/firmware.bin   →  xiao-s3-single.bin
remoteid-mesh-dualcore/.pio/build/seeed_xiao_esp32s3/firmware.bin  →  xiao-s3-dualcore.bin
node-mode-dualcore/.pio/build/remote_node/firmware.bin     →  xiao-s3-node-remote.bin
node-mode-dualcore/.pio/build/home_node/firmware.bin       →  xiao-s3-node-home.bin
```

## Flashing without PlatformIO (esptool)

```bash
esptool.py --chip esp32s3 --port COM<X> --baud 921600 write_flash 0x0 <file>.bin
esptool.py --chip esp32c5 --port COM<X> --baud 115200 write_flash 0x0 <file>.bin
```
