# ESP32-S3 NeoPixel Lightsaber Firmware

A clean, beginner-friendly embedded lightsaber prop controller built with the Arduino framework and Adafruit NeoPixel library.

---

## Project Structure

```
lightsaber/
├── platformio.ini          # PlatformIO build + library config
├── src/
│   └── main.cpp            # All firmware code (single file, sectioned)
└── README.md               # This file
```

The entire firmware lives in **one well-sectioned file** — no unnecessary abstraction, easy to read top to bottom.

---

## Hardware Wiring

```
ESP32-S3 Mini
│
├── GPIO 5  ──────────────── LED Data (WS2812B / SK6812 strip)
│                             ↳ Add 300–470Ω resistor in series
│
├── GPIO 4  ──── [Button] ── GND
│               (momentary push button, other leg to GND)
│               Internal pull-up is enabled in firmware.
│               Button LOW = pressed.
│
├── 3.3V or 5V ──────────── LED Strip VCC  (use 5V for full brightness)
│                             ↳ 100µF capacitor across VCC/GND at strip
│
└── GND ─────────────────── LED Strip GND + Button GND
```

> **Power note:** WS2812B at full brightness can pull 60 mA per LED.  
> A 100-LED strip at full white = ~6 A. Use a proper LiPo + boost regulator  
> and never power the strip through the ESP32's 3.3V rail.

---

## Configuration (top of main.cpp)

| Constant | Default | Description |
|---|---|---|
| `LED_PIN` | 5 | GPIO for LED data line |
| `LED_COUNT` | 60 | Number of LEDs in strip |
| `LED_TYPE` | NEO_GRB + NEO_KHZ800 | WS2812B; change to NEO_GRBW for SK6812 |
| `BRIGHTNESS_DEFAULT` | 125 | Startup brightness (0–255) |
| `BUTTON_PIN` | 4 | GPIO for momentary button |
| `IGNITE_STEP_MS` | 12 | ms per LED during ignition |
| `RETRACT_STEP_MS` | 10 | ms per LED during retraction |
| `CONFIG_HOLD_MS` | 5000 | Hold time (ms) to enter Wi-Fi config |

All of these live in **Section 1** at the very top of `main.cpp`.

---

## State Machine

```
                     ┌─────────────────────────────────┐
                     │                                 │
                     ▼                                 │
              ┌─────────────┐                         │
              │    OFF      │ ◄── retraction done      │
              └──────┬──────┘                         │
                     │ Long press                      │
                     ▼                                 │
              ┌─────────────┐                         │
              │  IGNITING   │ (base→tip wipe)          │
              └──────┬──────┘                         │
                     │ Animation complete              │
                     ▼                                 │
              ┌─────────────┐                         │
              │     ON      │ ──── Long press ──►  RETRACTING ──┘
              └──────┬──────┘
                     │ Short press
                     ▼
               Cycle color
               (fade if live)

Any state + 5s hold ──► CONFIG_MODE
      CONFIG_MODE save ──► OFF
```

### States

| State | Description |
|---|---|
| `OFF` | Blade dark. Button input monitored. |
| `IGNITING` | Wipe animation running base→tip. Button ignored. |
| `ON` | Blade solid. Short press cycles color; long press retracts. |
| `RETRACTING` | Wipe animation running tip→base. Button ignored. |
| `CONFIG` | Wi-Fi AP active. Webserver running. Button not checked. |

---

## Button Timing Logic

The button uses **internal pull-up** (active LOW). Logic in `handleButton()`:

```
Raw GPIO read → 50ms debounce window → debounced state

On press start:  record millis() → btnPressStart
While held:      check if (millis() - btnPressStart) >= 5000 → CONFIG_MODE
On release:
   duration < 500ms  → SHORT PRESS  → cycleColor()
   duration >= 600ms → LONG PRESS   → startIgnition() or startRetraction()
```

The 500ms gap between `SHORT_PRESS_MAX` (500ms) and `LONG_PRESS_MIN` (1000ms) is intentional — it creates a dead zone that prevents accidental triggering of the wrong action.

---

## Animation Details

### Ignition (base → tip)
- `tickIgnition()` is called every loop tick
- Non-blocking: each call advances one LED if `IGNITE_STEP_MS` has elapsed
- **Brightness ramp**: the first ~25% of LEDs light up dimly; brightness ramps to full as the blade fills — simulates a capacitor charging effect
- Completes to `STATE_ON` when `animLedIndex >= LED_COUNT`

### Retraction (tip → base)
- Mirror of ignition: `animLedIndex` counts down from `LED_COUNT-1` to `0`
- Each step turns one pixel off
- Completes to `STATE_OFF`

### Color Crossfade
- When a short press occurs in `STATE_ON`, the current packed color is captured as `fadeFromColor`
- The index advances and `fadeToColor` is set
- `tickColorFade()` interpolates using `lerpColor()` (component-wise linear interpolation) over `COLOR_FADE_STEPS` frames at `COLOR_FADE_MS` per frame
- Default: 30 steps × 15ms = 450ms smooth fade — not abrupt

---

## Wi-Fi Configuration Mode

Triggered by holding the button for **5 seconds** from any non-config state.

1. Blade turns off; 5 LEDs flash purple briefly as a visual confirmation
2. ESP32 creates open AP: **`SaberConfig`**
3. Connect your phone/laptop → navigate to **`http://192.168.4.1`**
4. Use the color picker and brightness slider
5. Tap **SAVE & CLOSE**
6. Settings written to NVS; Wi-Fi shuts down; saber returns to `STATE_OFF`

The webserver uses ESP32's built-in `WebServer` class which is **non-blocking** — `server.handleClient()` in `loop()` returns immediately if no request is pending.

**Wi-Fi is completely off (`WIFI_OFF`) during normal operation** — this eliminates the ~80mA radio idle current and the significant heat the radio generates.

---

## Persistence (NVS)

Uses ESP32's `Preferences` library (wrapper around NVS flash storage):

| Key | Type | Description |
|---|---|---|
| `color_idx` | uint8 | Active palette index |
| `brightness` | uint8 | Active brightness |

Settings survive power cycles. When a custom color is chosen via the web UI, `closestPaletteIndex()` maps the arbitrary RGB to the nearest preset palette entry.

---

## Color Palette

Defined in `COLOR_PALETTE[]` in Section 6. Add colors by adding entries:

```cpp
{ "Cyan",    0x00, 0xFF, 0xFF },
{ "Orange",  0xFF, 0x50, 0x00 },
```

Short-press cycles through them in order. The palette size is computed automatically with `sizeof`.

---

## Flashing to ESP32-S3

### Prerequisites
- [PlatformIO](https://platformio.org/) installed (VS Code extension or CLI)
- USB cable connected to the ESP32-S3's **USB-OTG port** (for CDC serial) or the UART port

### Steps

```bash
# Clone / place the project folder, then:
cd lightsaber

# Build and upload
pio run --target upload

# Monitor serial output
pio device monitor --baud 115200
```

### If the upload fails
1. **Hold BOOT button** on the ESP32-S3 while pressing RESET, then release RESET — this forces download mode
2. Re-run `pio run --target upload`
3. After upload, press RESET to run

### Board selection
The `platformio.ini` uses `esp32-s3-devkitc-1`. If you have a different board (Adafruit Feather ESP32-S3, UM FeatherS3, etc.) change the `board =` line. Common options:
- `adafruit_feather_esp32s3` — Adafruit Feather ESP32-S3
- `um_feathers3` — Unexpected Maker FeatherS3
- `esp32-s3-devkitc-1` — Generic DevKit (most clones)

---

## Future Extensions

The code includes documented `TODO:` markers for:

| Feature | What to add |
|---|---|
| Sound | DFPlayer Mini on UART or I2S DAC; trigger on ignite/retract/clash |
| Clash detection | MPU6050 via I2C; `tickClash()` in `STATE_ON` |
| Blade flicker | Random brightness variation in `STATE_ON` loop |
| Gesture control | IMU-based swing detection |
| Auto-sleep | Idle timer → `goDeepSleep()` after N minutes in `STATE_OFF` |

---

## Library Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

```
adafruit/Adafruit NeoPixel @ ^1.12.0
```

Built-in ESP32 Arduino libraries used (no separate install needed):
- `Preferences` — NVS storage
- `WiFi` — Wi-Fi control
- `WebServer` — HTTP server
