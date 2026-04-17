# 🌱 AutoGarden

**Autonomous plant watering system** built on M5Stack StickS3 (ESP32-S3). Maintains target soil moisture using a pulse–soak–measure feedback loop, with overflow protection, weather-aware scheduling, and optional remote control via Telegram.

Designed as an offline-first embedded appliance — all watering logic runs locally with zero cloud dependency. Wi-Fi and Telegram are purely optional add-ons.

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32--S3-blue" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/framework-Arduino-teal" alt="Arduino">
  <img src="https://img.shields.io/badge/build-PlatformIO-orange" alt="PlatformIO">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="MIT">
</p>

---

## Features

### Watering Engine
- **Pulse–Soak–Measure FSM** — delivers water in small configurable pulses (10–100 ml), waits for soil absorption, then re-measures before deciding whether to continue.
- **Per-pot independence** — supports up to 2 pots, each with its own profile, sensor calibration, and pump.
- **6 built-in plant profiles** — Tomato, Pepper, Basil, Strawberry, Chili, and a fully custom profile. Each defines target/critical/max moisture, hysteresis, soak time, pulse size, and max pulses per cycle.
- **Reservoir budget tracking** — estimates remaining water from pumped volume, warns at low threshold, enters crisis mode (reduced pulses) when supply is critically low.
- **Vacation mode** — reduces watering targets, limits pulse count, extends cooldown between cycles.

### Safety
- **Hard pump timeout** (30 s) — hardware-level failsafe independent of the FSM, kills any pump that exceeds max on-time.
- **Cooldown enforcement** — minimum interval between watering cycles prevents over-watering.
- **Anti-overflow** — capacitive water-level sensors on the pot and reservoir block the pump when water is detected.
- **Asymmetric water-level filtering** — water-level sensors use fast trip and slower clear, so pumps stop quickly on edge hits without flapping UI/FSM/Telegram every 100 ms.
- **Weather blocks** — suppresses non-rescue watering during direct sun (>40 klux) or extreme heat (>41 °C).
- **Mode-switch abort** — switching from AUTO to MANUAL immediately stops any active watering cycle and turns off pumps.

### Environment Sensing
- **Soil moisture** — capacitive sensor via PbHUB ADC.
- **Temperature & humidity** — SHT30 (ENV III).
- **Barometric pressure** — QMP6988 with OTP calibration.
- **Ambient light** — BH1750 lux sensor, used for dusk/dawn detection and direct-sun blocking.
- **Runtime light recovery** — BH1750 failures are retried and re-initialized without reboot; stale light is tracked explicitly.

### Dusk/Dawn Detection & Scheduling
- **Sensor-fusion dusk detector** — combines light level, light rate-of-change, temperature, humidity, and pressure into a transition score.
- **Solar clock** — learns the day/night cycle from observed transitions without external time sync.
- **Dusk watering window** — schedules watering during the optimal evening window (configurable).
- **Fail-safe freeze policy** — if the light sensor becomes stale or recovers after a fault, day/night learning freezes on the last stable phase until valid lux returns.

### User Interface
- **On-device GUI** on the StickS3 135×240 LCD — main dashboard with live sensor data, watering progress, and reservoir level.
- **Dual-view mode** — compact (both pots) or detailed (single pot) views.
- **Guard diagnostics** — status dumps and on-device guard labels distinguish stable trigger from transient pending level transitions.
- **Settings screen** — profile selection, reservoir capacity/threshold, pulse size, mode toggle, vacation toggle, Wi-Fi provisioning, factory reset.
- **Physical controls** — front button (BtnA) + side button (BtnB) navigation; DualButton for manual pump test (blue = pump, red = emergency stop).

### Network (Optional)
- **Wi-Fi with captive portal** — AP mode for initial provisioning, auto-off after 5 min idle.
- **Telegram bot** — interactive remote control via `/ag` with inline menu, status/history/profile views, safe watering pulse, stop, refill, vacation, mode toggle, and Wi-Fi portal launch.
- **Alert deduplication** — water-level incidents generate one incident notification and one clear instead of flapping per tick.
- **Daily heartbeat report** — automated inline Telegram snapshot with 24 h watering history, drying trends, and reservoir endurance estimate; one startup check is sent after 5 minutes, then the normal daily report targets dawn + 3 h.
- **Reconnect with backoff** — Wi-Fi auto-reconnects (5 s → 5 min exponential backoff), never blocks the watering task.

### Implementation Status
| Area | Status |
|---|---|
| Core watering FSM + pump safety | Implemented |
| Multi-task runtime (Control/UI/Net/Config) | Implemented |
| Config + runtime persistence (NVS) | Implemented |
| Captive portal provisioning (embedded HTML) | Implemented |
| Telegram remote control (`/ag` + inline menu) | Implemented |
| Dusk-based scheduling and fallback timing | Implemented |

### Configuration & Persistence
- **NVS storage** — all settings persist across reboots (mode, profiles, reservoir, vacation, calibration).
- **Dedicated history partition** — large sensor history is stored in a separate NVS partition to avoid starving config/runtime namespaces.
- **Schema versioning** — automatic migration on firmware update; safe fallback to defaults on mismatch.
- **Factory reset** — hold BtnA+B for 5 seconds from the Settings screen.

### Logging
- **Single-record logger contract** — application logs go through one `AGSerial` path, one write per logical record.
- **Explicit truncation markers** — oversized or multiline records are normalized and tagged instead of silently corrupting adjacent lines.

---

## Hardware

### Required

| Component | Role | Interface |
|---|---|---|
| [M5Stack StickS3](https://docs.m5stack.com/en/core/StickS3) | Main controller (ESP32-S3-PICO, 8 MB flash, 8 MB PSRAM) | USB-C |
| [M5Stack PbHUB v1.1](https://docs.m5stack.com/en/unit/pb.hub) (SKU: U041-B) | 6-channel I²C GPIO/ADC expander (STM32F030, addr `0x61`) | I²C via Grove |
| [M5Stack Watering Unit](https://docs.m5stack.com/en/unit/watering) | Capacitive soil moisture sensor + submersible pump | PbHUB CH0 |
| [M5Stack ENV III](https://docs.m5stack.com/en/unit/envIII) | SHT30 temp/humidity (`0x44`) + QMP6988 barometer (`0x70`) | I²C |
| [M5Stack DLight](https://docs.m5stack.com/en/unit/dlight) | BH1750 ambient light sensor (`0x23`) | I²C |
| [M5Stack Dual Button](https://docs.m5stack.com/en/unit/dual_button) | Manual override (blue = pump, red = stop) | PbHUB CH5 |
| 2× Non-Contact Water Level Sensor | Pot overflow (CH2) + reservoir low (CH3) | PbHUB digital |
| [M5Stack TypeC2Grove](https://docs.m5stack.com/en/unit/TypeC2Grove) | USB-C to Grove power adapter | — |
| Grove Hub (1-to-3) | Splits Grove port to PbHUB + I²C sensors | — |

### Channel Mapping (PbHUB)

| Channel | Device | Signal |
|---|---|---|
| CH0 | Watering Unit (pot 0) | pin0 = moisture ADC, pin1 = pump enable |
| CH1 | Watering Unit (pot 1) | pin0 = moisture ADC, pin1 = pump enable |
| CH2 | Water Level Sensor | pot 0 overflow (digital) |
| CH3 | Water Level Sensor | reservoir low (digital) |
| CH4 | Water Level Sensor | pot 1 overflow (digital) |
| CH5 | Dual Button | pin0 = blue, pin1 = red |

> **Note:** Channel assignments and sensor polarities are defined as constants in `config.h` and should be verified on your wiring setup.

### Known Hardware Quirks (PbHUB v1.1)

- PbHUB requires I²C STOP+START sequences (no repeated start) with ~10 ms inter-transaction delay.

### Water Level Sensor Protection (3.3 V Zener Clamp)

Water level sensors output ~4.85 V (HIGH) which exceeds the STM32F030 3.3 V VDDA.
Each sensor channel (CH2, CH3, CH4) has a Zener clamp circuit:

```
        Sensor ──── 1 kΩ ───┬─── PbHUB CHx
                             │
                         BZX84C3V3
                         (cathode up)
                             │
                            GND
```

- **HIGH** (water detected): 4.85 V → clamped to ~3.3 V (clean logic HIGH for STM32)
- **LOW** (no water): 0 V → passes through as 0 V (logic LOW)
- Non-inverting — no firmware changes needed for sensor polarity.
- Components per channel: 1× BZX84C3V3, 1× 1 kΩ . Total: 3 channels.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    FreeRTOS Tasks                    │
├──────────────┬──────────────┬────────────┬──────────┤
│ ControlTask  │   UiTask     │  NetTask   │ CfgTask  │
│  prio 5      │   prio 3     │  prio 2    │  prio 1  │
│              │              │            │          │
│ • Sensors    │ • LCD render │ • WiFi     │ • NVS    │
│ • Watering   │ • Button nav │ • Telegram │   save   │
│   FSM        │ • Settings   │ • AP mode  │          │
│ • Safety     │              │            │          │
│ • Analysis   │              │            │          │
└──────────────┴──────────────┴────────────┴──────────┘
         ▲              ▲              ▲
         └──── Event Queue + Snapshot Mutex ────┘
```

- **Non-blocking** — no `delay()` calls; all logic runs in short timed ticks (10 ms / 100 ms / 1 s).
- **Event-driven** — ISRs set flags only; inter-task communication via FreeRTOS queues.
- **Priority isolation** — watering safety logic cannot be starved by GUI rendering or network I/O.

### Watering FSM

```
IDLE → EVALUATING → PULSE → SOAK → MEASURING ──→ DONE
                      ↑       │                     │
                      └───────┘         (target met) │
                                                     ↓
                                    OVERFLOW_WAIT   IDLE
                                        │
                                  BLOCKED (safety)
```

---

## Building

### Prerequisites

- [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension.
- Linux: add your user to the `dialout` group for serial access:
  ```bash
  sudo usermod -aG dialout $USER
  # Log out and back in
  ```

### Build, Upload, Monitor

All operations are performed through the **PlatformIO sidebar** in VS Code:

1. **Build** — click the checkmark icon or run `PlatformIO: Build` from the command palette.
2. **Upload** — click the arrow icon. If upload fails, put StickS3 into download mode by pressing the reset button on the side while holding the front button.
3. **Monitor** — click the plug icon for serial output at 115200 baud.

> The project does **not** require the PlatformIO CLI (`pio`) to be installed system-wide.

---

## Configuration

All settings are accessible from the on-device **Settings screen** (press BtnA to enter, BtnB to navigate, long-press BtnB to change values):

| Setting | Range | Default | Description |
|---|---|---|---|
| Pumps | 1–2 | 1 | Number of active pots |
| Profile 1/2 | 6 profiles | Tomato | Plant watering profile per pot |
| Reservoir | 0.5–20 L | 1.5 L | Total reservoir capacity |
| Res. min | 300–3000 ml | 400 ml | Low-water warning threshold |
| Pulse | 10–100 ml | 25 ml | Water per pulse |
| Mode | AUTO / MANUAL | AUTO | AUTO runs the watering FSM; MANUAL = button only |
| Vacation | ON / OFF | OFF | Reduced watering for extended absence |
| WiFi | — | — | Launches AP mode for Wi-Fi provisioning |
| Reset | BtnA+B 5 s | — | Factory reset (clears all NVS data) |

---

## Project Structure

```
autogarden/
├── platformio.ini          # PlatformIO build configuration
└── src/
    ├── main.cpp            # Boot, FreeRTOS tasks, tick orchestration
    ├── config.h / .cpp     # Configuration structs, NVS persistence, plant profiles
    ├── hardware.h / .cpp   # All hardware drivers (PbHUB, sensors, actuators)
    ├── watering.h / .cpp   # Watering FSM, scheduling, safety, water budget
    ├── ui.h / .cpp         # LCD GUI rendering, button handling, settings
    ├── network.h / .cpp    # WiFi, Telegram bot, AP mode, captive portal (embedded HTML)
    ├── analysis.h / .cpp   # EMA filters, trend analysis, dusk/dawn detector, solar clock
    └── events.h            # Event types and FreeRTOS event queue
```

---

## Telegram Remote Control

Once Wi-Fi is provisioned and a Telegram bot token is configured:

| Entry / Action | Description |
|---|---|
| `/ag` | Opens the main Telegram GUI |
| `📊 Status` | Current sensor readings, watering phase, budget, and action state |
| `📈 History` | Pumped water and trend summary |
| `🌿 Profiles` | Built-in plant profile list |
| `💧 Water Xml` | Starts one safe pulse using the current configured pulse size |
| `🛑 Stop` | Forces all pumps OFF and aborts active cycles |
| `🪣 Refill` | Marks the reservoir as refilled |
| `🏖 Vacation ON/OFF` | Toggles vacation mode |
| `⚙️ Mode AUTO/MANUAL` | Switches operating mode |
| `📶 WiFi` | Starts the captive portal without stopping automation |

Secrets can be provided through [include/telegram_local_config.example.h](include/telegram_local_config.example.h) copied to [include/telegram_local_config.h](include/telegram_local_config.h) or via the captive portal and NVS.

---

## License

This project is licensed under the [MIT License](LICENSE).

## Dependencies

| Library | Author | License | Source |
|---|---|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | M5Stack | MIT | Auto-installed by PlatformIO |
| [M5PM1](https://github.com/m5stack/M5PM1) | M5Stack | MIT | Auto-installed by PlatformIO |
| [Preferences](https://github.com/espressif/arduino-esp32) | Espressif | LGPL-2.1 | Part of Arduino-ESP32 |
| [WiFi / WebServer / DNSServer](https://github.com/espressif/arduino-esp32) | Espressif | LGPL-2.1 | Part of Arduino-ESP32 |

All dependencies are fetched automatically on first build — no manual installation required.
