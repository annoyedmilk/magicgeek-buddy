# CLAUDE.md

Guidance for Claude Code when working in this repository. The user
facing project description lives in README.md; this file is for
operational details, conventions, and traps that have already been
hit and fixed.

## Project shape

ESP-IDF v6.0.1 firmware for the GeekMagic Pro (ESP32 D0WD V3, 16MB
flash, ST7789V 240x240 panel, one capacitive touch pad). It is a
Claude desktop companion. Talks BLE (NimBLE Nordic UART Service) to
the Claude desktop apps in developer mode.

The BLE wire protocol is the one published with Anthropic's
`anthropics/claude-desktop-buddy` reference firmware. The ASCII pet
art for the 18 species was ported from that repo too. Beyond that the
two projects share no code: this firmware targets different hardware,
uses ESP-IDF + NimBLE instead of Arduino, and skips features that
depend on hardware we don't have.

## Build and flash

```
source /opt/esp-idf/export.sh        # required each shell
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

From outside a sourced shell:
```
bash -c "source /opt/esp-idf/export.sh && idf.py build"
```

The build produces `build/ClaudeBuddy.bin`.

OTA path after the first manual flash: open
`http://<device-ip>/ota` and drag a fresh `build/ClaudeBuddy.bin`
onto the upload area. The bootloader has rollback enabled, so a bad
image reverts on next boot. The same page has a Danger Zone with a
Factory Reset button (type RESET to enable, then click).

### Manual flash procedure (only needed if OTA is unavailable)

The board has no exposed BOOT or RST buttons and uses a bare FTDI
without auto reset. To enter the ROM bootloader:

1. Hold a jumper between GPIO0 and GND.
2. Briefly tap RST to GND and lift RST.
3. Keep GPIO0 grounded for about two seconds after releasing RST.
4. Lift GPIO0 and run `idf.py flash` (or esptool) within its
   connect-wait window.

## Hardware pin map

| GPIO | Function                                       |
|------|------------------------------------------------|
| IO2  | Display DC (data / command)                    |
| IO4  | Display reset                                  |
| IO18 | SPI CLK                                        |
| IO23 | SPI MOSI                                       |
| IO25 | Backlight, inverted (LOW = on, HIGH = off)     |
| IO32 | Capacitive touch (channel 9)                   |

There is no IMU, no battery PMIC, no hardware RTC. Display is SPI mode
3 at 20 MHz with the panel inversion bit enabled. The full 240x240
surface is used.

## Input model (single touch pad)

| Context           | TAP                 | DOUBLE TAP      | LONG PRESS  |
|-------------------|---------------------|-----------------|-------------|
| Permission prompt | approve             | deny            | open menu   |
| Persona home      | scroll transcript   | (reserved)      | open menu   |
| Menu              | next item           | activate        | close menu  |
| Info / confirm    | no effect           | back to menu    | back        |

UI module (`ui.c`) gets first refusal on every gesture. Gestures the
UI didn't consume fall through to the bridge for approve / deny, or
to the transcript scroller on the persona screen.

## File layout

```
main/
  main.c              boot, render loop, screen selection, touch routing
  display.c/.h        ST7789V raw SPI, GPIO25 backlight via LEDC PWM
  framebuffer.c/.h    240x120 banded RGB565 (about 58 KB), 2 pass flush
  gfx.c/.h            8x8 font, fills, text composing into the FB
  touch_button.c/.h   tap, double tap, long press gestures
  storage.c/.h        NVS key value wrapper
  wifi_manager.c/.h   STA, captive portal, shared HTTP server
  ble_nus.c/.h        NimBLE NUS, LE Secure Connections, bonded, passkey
  bridge.c/.h         cJSON heartbeat parser, command dispatch
  stats.c/.h          NVS backed stats, settings, owner, level math
  buddy.c/.h          ASCII pet registry, drawing primitives
  buddies/*.c         18 species
  ui.c/.h             menu, info, factory reset overlays
  ota_server.c/.h     Claude themed OTA + factory reset on the shared httpd
```

## Architectural rules

Two of these are load bearing. Violating them caused real boot loops
during development.

1. Rendering must run in its own FreeRTOS task; `app_main` returns.
   An infinite loop in `app_main` keeps the main task alive as the
   loop, and a long first draw trips the interrupt or task watchdogs
   before the first delay. Mirror the pattern in `app_main`: do init,
   `xTaskCreate(buddy_task, ...)`, return.

2. Pixel buffers on the stack must be sized for the worst case scale,
   or rendered incrementally. An undersized glyph buffer in `gfx_char`
   smashed the stack and corrupted the FreeRTOS scheduler. A panic
   inside scheduler functions (`prvSelectHighestPriorityTaskSMP`,
   `vTaskSwitchContext`) is almost always stack corruption upstream,
   not a scheduler bug.

3. `display_*` SPI calls are blocking polling transmits. Heavy direct
   draws back to back without yielding can strain the 300 ms
   interrupt watchdog. The framebuffer flush is one DMA blit per
   band, so this only matters if a future caller bypasses the FB and
   talks to display.c directly.

## Render model

Two pass banded framebuffer. `fb_frame(compose_callback, ctx)` runs
the callback twice. On each pass the framebuffer's `active_band_y`
points to a different 120 row band; `fb_set_px` and `fb_fill_rect`
silently skip writes outside the active band. The callback uses
full screen Y coordinates and is idempotent.

Each band buffer is `240 * 120 * 2 = 57600` bytes (one contiguous
allocation, DMA capable). Allocated once at boot after WiFi init so
the heap has settled.

Compose callbacks in `main.c`:

| Callback              | When                                       |
|-----------------------|--------------------------------------------|
| compose_passkey       | BLE pairing 6 digit code visible           |
| compose_prompt        | desktop sent a permission prompt           |
| compose_persona       | bridge data alive, normal home             |
| compose_ble_waiting   | BLE paired but no heartbeat yet            |
| compose_wifi_online   | WiFi up but no BLE peer yet                |
| compose_setup         | captive portal AP mode                     |
| compose_connecting    | STA attempting to connect                  |

Each home composer ends with `ui_compose_overlay()` so menu / info /
reset panels layer on top.

`compose_persona` uses a pure-black canvas (the ASCII pet renders
with `BUDDY_BG = 0x0000`, so anything other than pure black leaves a
visible halo around each glyph cell). All other screens use the warm
Claude charcoal `COLOR_CLAUDE_BG`.

## WiFi behavior

WiFi is secondary to the BLE link. On boot:

1. NVS has saved creds, try STA connect in a background task (30 s
   budget). Success starts the shared httpd in STA mode. Failure
   falls back to the captive portal.
2. No creds, start the captive portal directly. SoftAP name is
   `Buddy-XXYY` from the last two SoftAP MAC bytes.

Captive portal page is Claude themed (charcoal `#1F1E1D`, paper
`#F0EEE6`, coral `#D97757`). The "Forget WiFi" button wipes NVS
creds and reboots back into setup mode.

Both paths register routes on the same `httpd_handle_t` exposed by
`wifi_manager_get_httpd()`. The captive wildcard catch all is
registered only in AP mode so it does not hijack `/ota` in STA mode.

## OTA HTTP routes

Two routes mount onto the shared httpd:

| Route        | Method | What it does                                   |
|--------------|--------|------------------------------------------------|
| `/ota`       | GET    | Serves the Claude-themed upload + reset page   |
| `/ota`       | POST   | Receives a firmware `.bin`, flashes, reboots   |
| `/ota/reset` | POST   | Factory reset (stats + bonds + WiFi) + reboot  |

The reset route does not validate the request body server-side; the
HTML form gates it behind a typed "RESET" confirm string plus a JS
`confirm()` dialog. This is the same friction as the device's own
two-step factory reset overlay.

## BLE link details

NimBLE peripheral, Nordic UART Service (UUIDs in `ble_nus.c`).
Encryption is required: NUS characteristics are flagged encrypted
only, so the first GATT access from the central triggers pairing.

Security configuration in `ble_init()`:
* `sm_io_cap = BLE_HS_IO_DISPLAY_ONLY`
* `sm_bonding = 1`, `sm_mitm = 1`, `sm_sc = 1`
* Random 6 digit passkey generated per pair attempt
* Bonds persisted in NVS via `ble_store_config`

Advertising splits across two payloads to fit the 31 byte cap:
primary advertising holds flags plus the `Claude-XXXX` name, scan
response holds the 128 bit NUS service UUID.

When the central (e.g. a Mac) sleeps the link drops normally. The
firmware re-enters advertising in the disconnect handler; the desktop
will reconnect to the bonded peer on wake, but the user may need to
click Connect again in the Hardware Buddy window (a desktop-side
limitation, not ours).

## Bridge state

`tama_state_t` (`bridge.h`) is the parsed heartbeat snapshot.
`bridge_get_state()` copies it under a mutex; the render loop reads
from this snapshot. `bridge_derive()` is a pure function from
`tama_state_t` to `persona_state_t`.

Implemented commands (from the reference project's REFERENCE.md):

| Command            | Side effect                                   |
|--------------------|-----------------------------------------------|
| heartbeat          | update `tama_state_t`, feed tokens to stats   |
| status             | reply with sec, sys, stats blocks             |
| name               | safe copy to NVS                              |
| owner              | safe copy to NVS                              |
| unpair             | `ble_clear_bonds`                             |
| char_begin..end    | rejected with ok:false (not supported here)   |
| permission (out)   | sent from touch handler when prompt active    |

## Build conventions

`buddies/*.c` is GLOB'd into SRCS in `main/CMakeLists.txt`. Adding a
new species is a file drop.

`__attribute__((unused))` for array declarations in C must go AFTER
the bracket: `T NAME[5] __attribute__((unused))`. Two species files
(`buddies/cat.c`, `buddies/chonk.c`) use this for spare frames that
the SEQ array never references.

## Heap budget

Steady state free heap with every subsystem coresident is around
50 KB. Snapshot of the progression at the last clean boot:

| Stage                      | Free heap (approx) |
|----------------------------|--------------------|
| After NVS, before FB       | 156 KB             |
| After framebuffer alloc    | 92 KB              |
| After BLE host init        | 59 KB              |
| After bridge init          | 50 KB              |
| Steady state               | 50 KB              |

cJSON parsing of a single heartbeat allocates around 2 KB. The bridge
line buffer is 1.5 KB. Tasks: `buddy` 8 KB, `bridge` 8 KB.

## Common operations

| Task                         | Where                                  |
|------------------------------|----------------------------------------|
| Add a new BLE command        | `bridge.c` `dispatch()`                |
| Add a menu item              | `ui.c` `menu_idx_t`, `MENU_LABELS`     |
| Add a screen                 | `main.c` new compose, screen enum      |
| Adjust pet animation cadence | `main.c` `next_anim_ms`, 200 ms tick   |
| Add a stat field             | `stats.h` struct, load / save in `.c`  |
| Add an ASCII species         | drop a file in `main/buddies/`         |

## Things intentionally absent

Source project features that do not apply to this hardware and are
not implemented:

* Shake to trigger dizzy state, face down to nap (no IMU).
* Battery percentage, charging indicator (no AXP).
* Hardware RTC clock face (no DS3231 or equivalent).
* Sound (no piezo or DAC speaker).
* GIF character packs (the AnimatedGIF library's LZW init wouldn't
  fit our heap budget without corrupting other tasks; the folder-push
  protocol is acked with `ok:false, error:"unsupported"` so the
  desktop shows a clean rejection toast).

The `settings_t` struct (`stats.h`) still carries fields for sound,
LED, and brightness so the source's NVS schema is preserved. They
are stored but ignored at runtime.

## Release process

Tag a commit (e.g. `v1.0.0`) and push the tag. GitHub Actions builds
the firmware with ESP-IDF v6.0.1 and attaches `ClaudeBuddy.bin` to a
new Release. See `.github/workflows/release.yml`.
