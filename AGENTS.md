# Baby Camera — Agent Instructions

## Project Overview
Baby monitor firmware (ESP32-CAM + INMP441 I2S mic) + `babyfan` proxy for multi-viewer streaming. Two independent streams: MJPEG video (`:81/stream`) + endless WAV audio (`:82/audio`). Web UI on `:80`.

**New:** WiFi provisioning via AP mode (SSID: "BabyCAM", Pass: "12345678"), NVS persistence for credentials & runtime settings, mDNS (http://babycam.local/), lamp (flash LED) control via web UI, **independent audio/video toggle controls**, **battery voltage monitoring via ADC**.

## Build / Flash

```bash
cd firmware
pio run -t upload          # PlatformIO, pinned to espressif32@6.5.0
pio device monitor         # 115200 baud
```

**No need to edit config.h for WiFi** — credentials are entered via provisioning AP on first boot.

**Platform pinned to 6.5.0** (Arduino core 2.0.14) — core 3.x replaced `driver/i2s.h` API; rewrite would need re-verification against camera driver (owns I2S0).

**Partition:** `huge_app.csv` (~3 MB app, no OTA).

**Build flags** (in platformio.ini): `-DUSE_MDNS=1 -DUSE_NVS=1`

## First Boot / WiFi Provisioning
1. Power on the board (5V + GND, 470–1000 µF cap recommended)
2. Connect to WiFi AP: **BabyCAM** / **12345678**
3. Browse to `http://192.168.4.1/` (or `http://babycam.local/` after connect)
4. Click "Scan Networks", select your SSID, enter password, click "Connect"
5. Board reboots, connects to your WiFi, serves UI at its IP + `http://babycam.local/`
6. Credentials stored in NVS — survives power loss, no reflash needed

To reset WiFi: hold IO0 low at boot (or erase NVS via `/set?wifi=clear` if implemented), or flash `erase` via esptool.

## Flashing Gotchas
- **Hold IO0 to GND, THEN power up.** IO0 sampled only at reset rising edge; pressing BOOT on a running board does nothing.
- **Bad ESP32-CAM-MB shields** pass data board→PC but not PC→board (DTR/RTS never reach EN/IO0). `esptool` shows identical `No serial data received` for both "no board" and "bad shield".
- Use `tools/detect.sh` to distinguish: it listens passively first, then tries SYNC.

## Key Hardware Constraints
| Constraint | Detail |
|------------|--------|
| Microphone | INMP441 on **I2S1** (I2S0 owned by camera DVP bus) |
| Mic pins | SCK=14, WS=15, SD=13 (NOT IO12 — MTDI strap, board won't boot if driven high at reset) |
| L/R pad | Tie to GND → mic speaks left slot, but **ESP32 receiver requires `ONLY_RIGHT`** (chan=1) |
| Sample shift | 24-bit sample sits in **low 24 bits** of 32-bit slot → shift by **8**, not 16 |
| Lamp (flash LED) | GPIO4, LEDC channel 4, 0–255 PWM (very bright at max — use low values) |
| Battery ADC | GPIO33 (ADC1_CH5), voltage divider required (e.g., 200k+100k = 3x), 11dB atten |
| Power | Camera draws ~300 mA bursts. USB-serial cannot supply. Feed 5V pin + 470–1000 µF cap. Brownout detector disabled in firmware. |

## Runtime Architecture
- **Three `esp_http_server` instances** (ports 80, 81, 82), each with own `ctrl_port`. Single-threaded per server → endless handler blocks all other endpoints on that port.
- **`lru_purge_enable=true` mandatory.** Without it, server accepts then silently closes connections past `max_open_sockets` (2 forgotten browser tabs = dead endpoints while pings still work).
- **Mic task pinned to core 1** (WiFi/LwIP on core 0).
- **Audio ring buffer drops samples** when full (bounds latency); stale audio worse than momentary gap for baby monitor.
- **WAV header `riffSize=0x7FFFFFFF`** — `0xFFFFFFFF` reads as −1 in some browsers, they never start decoding.
- **NVS persistence:** gain, lamp, chan, shift, WiFi credentials survive reboot.
- **mDNS:** `http://babycam.local/` works without knowing IP (if network supports it).

## Proxy (`babyfan`)
```bash
sudo mkdir -p /opt/babyfan
sudo install -m 755 babyfan.py /opt/babyfan/
sudo install -m 644 babyfan.service /etc/systemd/system/
sudoedit /etc/systemd/system/babyfan.service  # set BABYFAN_CAMERA
sudo systemctl enable --now babyfan
```

- Listens on `127.0.0.1:8090`, serves `/audio` and `/stream`.
- **One upstream connection per stream**, fans to N viewers.
- **Grace period (default 8s):** holds camera connection after last viewer leaves (phones lock screens constantly; reconnect thrash costs more).
- **Decodes chunked encoding** from camera — hex length markers would land inside media otherwise.
- **Drops slow viewers** (>4 MB backlog).
- **Closes all viewers when upstream dies** — browser gets no error on stalled socket, keeps showing last frame forever.
- **Health endpoint:** `GET /health` returns JSON with viewer counts, upstream status, reconnect counts.
- **Proxies `/level` and `/set`** directly to camera (port 80).

## Tools
```bash
tools/detect.sh [port]          # identifies port state: RUNNING / BOOTLOADER / DEAD
tools/uart.sh [port] [baud]     # serial monitor; Ctrl-T timestamps, Ctrl-R reset pulse
```
- `detect.sh` distinguishes three states that esptool reports identically.
- `uart.sh` holds DTR/RTS deasserted (some adapters wire them to EN/IO0; defaults would hold chip in reset).

## Common Pitfalls (from README)
1. `ONLY_LEFT` yields flat zero → use `ONLY_RIGHT` even with L/R=GND.
2. Shift by 16 loses 75% signal → shift by 8.
3. Camera owns I2S0 → mic on I2S1.
4. `lru_purge_enable` not optional.
5. `<audio>` element queues forever → page measures buffered vs playing, jumps forward >1.5s; device drops samples instead of queueing.
6. WAV header `0xFFFFFFFF` breaks browsers → use `0x7FFFFFFF`.

## Endpoints
| Path | Description |
|------|-------------|
| `/` | Web UI (level meter, **audio on/off**, **video on/off**, lamp toggle + brightness) |
| `/level` | JSON: level, gain, uptime, RSSI, heap, diagnostics |
| `/set?gain=N&lamp=N&chan=N&shift=N` | Live tuning, no reflash |
| `:81/stream` | MJPEG (only while watching) |
| `:82/audio` | Endless WAV (always running) |
| **Provisioning AP (port 80)** | |
| `/` | WiFi setup page (scan, select, connect) |
| `/scan` | JSON: list of scanned networks (ssid, rssi, sec) |
| `/connect` | POST JSON `{ssid, pass}` — attempts STA connect, saves to NVS on success |
| **Proxy (babyfan, port 8090)** | |
| `/health` | JSON: uptime, viewer counts, upstream status, reconnects |
| `/level`, `/set` | Proxied to camera port 80 |
| `/audio`, `/stream` | Fanned from camera ports 82, 81 |

## Web UI Controls

### Audio (`🔊 Audio On` / `🔇 Audio Off`)
- **Click** toggles audio stream on/off
- **On:** connects to `/audio`, plays WAV, auto-reconnects on error/ended
- **Off:** pauses `<audio>`, clears `src`, resets element — no background traffic
- **Auto-starts:** No — requires user interaction (browser autoplay policy)
- **Lag detection:** seeks forward when buffer exceeds 1.5 s

### Video (`Show video` / `Hide video`)
- **Click** toggles MJPEG stream on/off
- **On:** sets `<img src>` to `/stream`, auto-reconnects with exponential backoff
- **Off:** removes `src` from `<img>` — no background traffic

### Lamp (`💡 Lamp` button)
- **Click** toggles on/off (remembers last brightness, default 40)
- **Long press (400ms):** reveals brightness slider (0–255)
- **Visual:** button turns accent color when on, shows current value (`Lamp 40`)
- **Endpoint:** `GET /set?lamp=N` (0–255), persisted to NVS automatically

### Battery Monitor
- **ADC pin:** GPIO33 (ADC1_CH5) — requires voltage divider (e.g., 200kΩ + 100kΩ = 3x division)
- **Measurement:** 64 samples averaged, updated every 30s, calibrated via `esp_adc_cal`
- **Display:** 🔋 >60%, 🪫 30–60%, 🔴 <30% with voltage and percentage in status area
- **Endpoint:** included in `/level` JSON as `batV` (volts) and `batPct` (0–100)
- **Config (config.h):** `BAT_ADC_PIN`, `BAT_ADC_ATTEN`, `BAT_DIVIDER_RATIO`, `BAT_MIN_VOLTAGE`, `BAT_MAX_VOLTAGE`

## No Tests / Lint / CI
This is a hardware-bound firmware project. Verify by flashing to device and checking serial output (`pio device monitor`) and web UI.

## Limits
- **One listener at a time on board** — mic task feeds single ring buffer; two listeners shred the stream.
- **No recording** — unsynchronised streams, RTSP would be a different project.
- **No encryption/auth** — LAN only, do not port-forward.