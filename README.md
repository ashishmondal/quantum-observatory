# Quantum Observatory

A network-connected 64×32 RGB LED matrix that renders curated astronomical and ambient scenes — and doubles as the only clock in the room.

Hardware: **Raspberry Pi Pico W** + **Waveshare RGB-Matrix-P3 64×32** (FM6126A driver, HUB75) + on-board DS3231 RTC + buzzer + buttons.
Stack: **C++** on **PlatformIO** (earlephilhower Arduino-Pico core), Adafruit Protomatter, PubSubClient (MQTT), ArduinoJson.

## Architecture in one paragraph

Home Assistant is the **Director** — owns data, scheduling, and intent. The Pico is the **Cinematographer** — owns rendering and timing. They talk over MQTT in stateless, intent-based JSON (`{"scene_id": "iss_pass", ...}`); no raw pixel streaming. The RP2040's two cores are split: **Core 0** runs Wi-Fi / MQTT / RTC / button I/O; **Core 1** runs the matrix render pipeline at 20–30 FPS. Shared state is a single `SceneState` struct guarded by a mutex.

## Documentation

- [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) — single source of truth for what the system does (FR-* / NFR-*).
- [docs/PLAN.md](docs/PLAN.md) — phased implementation roadmap; one small demoable win per step.
- [docs/CODING_PRACTICES.md](docs/CODING_PRACTICES.md) — rules every code change must follow (memory, numeric, concurrency discipline).
- [docs/HARDWARE.md](docs/HARDWARE.md) — pin map, on-board peripherals, wiring notes.
- [docs/THEME.md](docs/THEME.md) — retro sci-fi theming system: five themes (Apollo / Nostromo / Vectrex / Blade Runner / LCARS), MQTT contract, asset authoring rules (FR-15).

## Build & flash

Requires [PlatformIO](https://platformio.org/) (the VS Code extension is the easy path).

```sh
# Copy the secrets template and fill in your Wi-Fi + MQTT broker
cp include/secrets.h.example include/secrets.h
# edit include/secrets.h

# Build
pio run --environment pico

# Flash (Pico in BOOTSEL the first time; afterwards 1200-bps reset works
# as long as the Serial Monitor is closed)
pio run --target upload --environment pico
```

The two VS Code tasks **Build (pico)** and **Upload (pico)** wrap the same commands.

## Status

Pre-1.0. Phase 5 (network MVP) in progress — see [PLAN.md](docs/PLAN.md) for the live checklist.

## License

[MIT](LICENSE).
