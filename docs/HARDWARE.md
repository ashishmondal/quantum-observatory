# Quantum Observatory — On-Board Peripherals

Notes on the **Waveshare Pico-RGB-Matrix-P3-64x32** carrier board's built-in
peripherals, extracted from the vendor's Pico C++ SDK demo
(`Pico-RGB-Matrix-P3-64x32-Demo/ModuleDrivers/`). All pin numbers are GP
(GPIO) numbers on the RP2040 / Pico W.

This is a reference doc — see [PLAN.md](PLAN.md) for which phases will
actually wire these in, and [REQUIREMENTS.md](REQUIREMENTS.md) for the FRs
that motivate them.

---

## Pin map summary

| Peripheral | Pins (GP) | Bus / mode | Notes |
|---|---|---|---|
| HUB75 panel | 2,3,4,5,8,9,10,11,12,13,16,18,20,22 | bit-bang via Protomatter | already in use ([config.h](../include/config.h)) |
| **DS3231 RTC** | SDA=6, SCL=7 | I²C1 @ 400 kHz | addr `0x68`, has on-die temperature sensor |
| **Light sensor** (photoresistor) | 26 | ADC0 | 12-bit raw, demo subtracts a 700 offset |
| **Buzzer** | 27 | GPIO push-pull (active high) | demo toggles bare; no PWM driver |
| **Buttons** | KEY0=15 (MENU), KEY1=19 (DOWN), KEY2=21 (UP) | GPIO input | demo polls `gpio_get` |
| **IR receiver** | IRM=28 | GPIO input, edge-triggered | 38 kHz demod, active-low envelope, NEC/Sony/RC5 via IRremote v4 |

None of these collide with the HUB75 pin map already in
[include/config.h](../include/config.h). Safe to add when needed.

---

## DS3231 RTC (I²C, addr 0x68)

**Why we care:** independent battery-backed clock. Cuts our reliance on
the MQTT time path (FR-9.5) — when MQTT drops, the RTC keeps wall-clock
time so the room clock doesn't go to `--:--`. May change the FR-9.5
decision; worth raising when we reach Phase 6.4 (offline fallback).

### Wiring (vendor demo)

```c
#define SDA       6
#define SCL       7
#define I2C_PORT  i2c1
#define DS3231_ADDRESS 0x68
```

### Init

```c
i2c_init(I2C_PORT, 400000);
gpio_set_function(SDA, GPIO_FUNC_I2C);
gpio_set_function(SCL, GPIO_FUNC_I2C);
gpio_pull_up(SDA);
gpio_pull_up(SCL);

// Enable temperature conversion, clear status.
uint8_t v[2];
v[0] = 0x0e; v[1] = 0x20;  i2c_write_blocking(I2C_PORT, 0x68, v, 2, false);
v[0] = 0x0f; v[1] = 0x00;  i2c_write_blocking(I2C_PORT, 0x68, v, 2, false);
```

### Read time (7 BCD bytes starting at register 0x00)

```c
uint8_t reg = 0x00, data[7];
i2c_write_blocking(I2C_PORT, 0x68, &reg, 1, /*nostop=*/true);
i2c_read_blocking (I2C_PORT, 0x68, data, 7, false);

uint8_t sec  = bcd2dec(data[0]);
uint8_t min  = bcd2dec(data[1]);
uint8_t hour = bcd2dec(data[2]);          // assumes 24h mode (default)
uint8_t wday = bcd2dec(data[3]);          // 1..7, 1 = Sunday (vendor convention)
uint8_t mday = bcd2dec(data[4]);          // 1..31
uint8_t mon  = bcd2dec(data[5] & 0x1f);   // 1..12
uint16_t yr  = 2000 + bcd2dec(data[6]);   // year - 2000
```

`bcd2dec(v) = (v >> 4) * 10 + (v & 0x0f)` and inverse `dec2bcd(v) = ((v / 10) << 4) + (v % 10)`.

### Set time

Write the same 7 BCD bytes back to address 0x00 (single I²C transaction:
`{0x00, sec, min, hour, wday, mday, mon, yr-2000}`, all dec2bcd encoded).

### On-die temperature

Trigger conversion (`{0x0e, 0x20}` to control reg), then read register
`0x11` for the integer-degrees-C byte. Vendor demo throws away the
fractional byte at 0x12.

### 12-hour mode caveats

If bit 6 of `data[2]` is set, the RTC is in 12h mode: mask the low 5 bits
for the hour, `bcd2dec()` it, subtract 1, then add 12 if bit 5 (PM flag)
is set. Default state out of the factory is 24h, so just keep it there.

### Timezone strategy (decided in Phase 3.6.3)

The RTC stores **local-time epoch seconds**, not UTC. Reads (`tod::now()`)
need no tz state — the cached snapshot is already local. Writes
(`tod::set_from_mqtt(epoch_utc, tz_offset_min, …)`) convert UTC → local
before persisting via `local = epoch_utc + tz_offset_min*60`.

Trade-offs:
- ✅ Read path stays branch-free; no flash/LittleFS lookup of a stored tz.
- ✅ DST and tz changes are HA's job — push a corrected `observatory/time`
  message and the RTC follows.
- ❌ Cannot recover UTC from the RTC alone (date math that needs UTC has
  to subtract a known tz offset; for the room-clock workload nothing
  needs UTC, so we don't carry one).

**Manual bootstrap before Phase 5.6 (MQTT):** define
`RTC_SEED_LOCAL_EPOCH` (and optionally `RTC_SEED_LOCAL_EPOCH_LABEL`)
either in `secrets.h` or via a `build_flags` line in `platformio.ini`,
flash once, observe the `[boot] rtc seeded …` log, then **remove the
define and re-flash** so subsequent boots leave the (now battery-backed)
RTC alone. Pick the epoch with `date -d '2026-05-01 19:30:00' +%s` for
your local wall clock — the seed is interpreted as local time, no tz
math applied.

---

## Photoresistor / ambient light (ADC0 / GP26)

**Why we care:** auto-dim. FR-7.3 currently says "driven by HA" — but
this gives us a fully local fallback when MQTT is down (FR-5.1).

### Wiring

```c
#define Light_sensor 26
adc_init();
adc_gpio_init(Light_sensor);
adc_select_input(0);          // ADC channel 0 = GP26
```

### Read

```c
uint16_t raw = adc_read();    // 0..4095 (12-bit)
return (raw - 700);           // vendor's "useful range" offset
```

The `- 700` magic number is the demo's empirical floor — what the sensor
reads in a fully dark room. Useful as a starting point; we should
re-baseline by logging raw values in our own enclosure before relying
on it for brightness control.

**Note:** the demo also calls `stdio_init_all()` inside `adc_Init()` — that's
incidental cleanup, not required for ADC. We already init Serial in our
`setup()`.

---

## Buzzer (GP27, active-high)

**Why we care:** audible alerts. Could front-end FR-1 priority-≥4 scenes
(e.g. an ISS pass overhead) with a chirp without needing a separate device.

### Wiring

```c
#define BUZZER 27
gpio_init(BUZZER);
gpio_set_dir(BUZZER, GPIO_OUT);
gpio_put(BUZZER, 0);          // off
```

### Use

The vendor demo just toggles the pin high/low. No PWM, no timer driver
— so this is **either** an active buzzer (built-in oscillator, drive
high → it beeps at fixed pitch) **or** a passive piezo where you'd need
to bit-bang a square wave for tone control.

Empirically: vendor's main loop just sets it high for short durations
on key press, which works for both. If we want pitched tones, drive it
with `pwm_set_*` on slice 13B (GP27 = PWM 5B) instead.

**Safety:** keep ON-time short. Continuous DC into a passive piezo is
fine, but continuous DC into an active buzzer is loud and irritating.

---

## Buttons (GP15 / GP19 / GP21, active-low when pulled-up)

**Why we care:** local override. Useful for testing when MQTT/HA isn't
available, and could drive scene cycling in the room.

```c
#define KEY0_PIN 15   // labelled MENU in the demo
#define KEY1_PIN 19   // labelled DOWN
#define KEY2_PIN 21   // labelled UP
```

Demo polls `gpio_get(pin)`. Doesn't show the pull config — verify with a
multimeter or assume the board has hardware pull-ups (the demo doesn't
call `gpio_pull_up` for them, suggesting external pulls).

Earlephilhower core: `pinMode(15, INPUT_PULLUP)` then `digitalRead(15)`
— pressed = 0.

---

## IR receiver (GP28 / silkscreen "IRM")

**Why we care:** standard 38 kHz IR demodulator on the carrier — opens
the door to driving the device with any commodity IR remote (NEC / Sony /
RC5 / etc.) without adding hardware. First use case: room-side scene
navigation + brightness override + info overlay, mirroring the on-board
buttons but from across the room.

The sensor is a typical TSOP-style module: built-in bandpass filter +
AGC + demodulator. Output is active-low — idle high, pulled low for the
duration of each 38 kHz burst the remote emits. No carrier handling in
firmware; just edge-timing.

### Wiring

```c
#define IRM 28   // GP28, digital input, no pull needed (open-drain on receiver)
```

### Library

We use [Arduino-IRremote v4.x](https://github.com/Arduino-IRremote/Arduino-IRremote)
(pinned in [platformio.ini](../platformio.ini)). It registers a
pin-change ISR + microsecond timer to capture edge timings and runs the
protocol decoder synchronously inside `IrReceiver.decode()`. The ISR is
cheap (only fires on IR activity), but **must be bound on Core 0** —
our Core 1 owns Protomatter timing and cannot tolerate ISR jitter
mid-frame.

### Init (logging-only POC, phase IR.1)

```cpp
// once, from setup() — Core 0
IrReceiver.begin(PIN_IR_RX, /*enableLEDFeedback=*/false);

// every loop() iteration — Core 0
if (IrReceiver.decode()) {
  const auto& d = IrReceiver.decodedIRData;
  // d.protocol, d.address, d.command, d.flags (REPEAT, PARITY_FAILED…)
  IrReceiver.resume();   // arm for next frame
}
```

### Known-unknowns to characterise in POC

Three HUB75-EMI failure modes the bare driver alone can't predict:

1. **Vcc ripple** desensitising the AGC → receiver works panel-off,
   dies panel-bright. Mitigation: LC filter on receiver Vcc, ferrite
   bead on signal line.
2. **Radiated noise** from the HUB75 ribbon → ghost decodes / corrupted
   command bytes with no remote pressed (look for `PARITY_FAILED` or
   `UNKNOWN` protocol in the [ir] log).
3. **AGC dead-time** after sustained noise → every Nth press missed.

POC pass criteria documented in [PLAN.md](PLAN.md) phase IR.1.

### Roku remote caveat

Roku ships two families:

- **Standard IR remote** (no mic, no headphone jack) → 38 kHz NEC-ish,
  decodes fine.
- **"Enhanced" / voice remote** (mic button, headphone jack, TV power
  passthrough) → Wi-Fi Direct or Bluetooth, **no IR LED at all**. Won't
  work no matter what we do.

If the Roku remote turns out to be the enhanced one, fall back to any
cheap NEC remote (Adafruit mini, Apple Remote A1156, old TV remote) to
validate the receiver path.

---

## Sources

All facts above come from
`Pico-RGB-Matrix-P3-64x32-Demo/ModuleDrivers/`:

- `driver_ds3231.{h,cpp}` — RTC
- `driver_adc.{h,cpp}` — light sensor
- `driver_buzzer.{h,cpp}` — buzzer
- `driver_key.h` — buttons
- `config/RGBMatrixConfig.h` — `CONFIG_SUPPORT_PICO` selector

The demo also supports an ESP32-S2 build with different pins; only the
`CONFIG_SUPPORT_PICO` branches above apply to our hardware.
