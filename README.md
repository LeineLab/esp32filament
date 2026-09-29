# ESP32 Filament Station

An NFC scanning terminal for MakerSpaceAPI's filament roll tracking module
(see the main project's [README](../../README.md) / [CLAUDE.md](../../CLAUDE.md)
for the REST API and web UI). Scan a spool's NFC tag to check it in or out of
stock — no touchscreen typing needed for the common case.

**Hardware:** "Cheap Yellow Display" (CYD, ESP32-2432S028) + an external PN532
NFC module wired to the board's I2C expansion pins. See [Wiring](#wiring) below.

Supports four spool tag formats: **Bambu Lab**, **Creality**, **OpenSpool**,
and **OpenTag3D**. See [Tag format support](#tag-format-support) for what's
solid and what's best-effort.

## Build & Flash

```bash
# Build (pick the environment matching your board's display driver — see below)
pio run -e cyd_ili9341
pio run -e cyd_st7789

# Flash via USB (first time)
pio run -e cyd_ili9341 -t upload

# Flash via OTA (after the first USB flash — uncomment upload_protocol in
# platformio.ini and set upload_port to the device's IP or esp32-filament.local)
pio run -e cyd_ili9341 -t upload

# Serial monitor
pio device monitor --baud 115200
```

## Which environment: ILI9341 or ST7789?

The CYD ships in two display-driver revisions that are otherwise **pin- and
resolution-identical** — a "2×USB" board revision uses ST7789 instead of the
original ILI9341. If you don't know which one you have, build/flash
`cyd_ili9341` first; if colors look wrong or inverted, reflash with
`cyd_st7789` instead. (Sources: [Random Nerd Tutorials CYD pinout guide](https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/),
[ESP32s.com CYD variants guide](https://esp32s.com/blog/the-complete-esp32-cheap-yellow-display-cyd-pinout-and-gpio-guide/).)

## Configuration

Copy `src/config.h.example` to `src/config.h` and fill in:
- `WIFI_SSID` / `WIFI_PASSWORD`
- `API_BASE_URL` — MakerSpaceAPI server URL
- `API_TOKEN` — a device Bearer token. Register the station in the web UI
  under **Machines**, with device type `filament_station` (any active
  machine token works for the `/filament/*` device endpoints — there's no
  separate registration screen for this).
- `OTA_PASSWORD`
- `TFT_BL_ACTIVE_LOW` — backlight polarity is reported inconsistently across
  CYD sources/batches; flip this if the screen stays dark or brightness
  behaves backwards.
- Touch calibration (`TOUCH_CAL_*`) — adjust if touch feels off.
- `TOUCH_Y_INVERTED` / `TOUCH_X_INVERTED` — touch-axis inversion varies
  between individual CYD boards (same kind of per-unit variance as the
  backlight polarity above). Defaults assume only the Y axis is mirrored
  (press near the top registers near the bottom); if touch still feels wrong
  after flipping that, also try `TOUCH_X_INVERTED`.

`config.h` is gitignored; `config.h.example` is the template.

## Wiring

Pin assignments (see `src/config.h.example` for the full list) are sourced from
[Random Nerd Tutorials' CYD pinout page](https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/)
and [witnessmenow/ESP32-Cheap-Yellow-Display's PINS.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md) —
cross-checked between both since individual community write-ups occasionally
disagree on details like backlight polarity (hence the config flag above).

**PN532 (I2C)** — wire to the CYD's CN1/P3 expansion header, which exposes two
free general-purpose GPIOs commonly used for I2C:
- PN532 SDA → GPIO27
- PN532 SCL → GPIO22
- PN532 VCC → 3.3V, GND → GND
- **Set the PN532 module's DIP switches/jumpers to I2C mode** (most breakout
  boards support I2C/SPI/HSU and default to a different mode — check yours).

The display and touch controller are already wired internally on the CYD;
no changes needed there.

No enclosure/mounting design is included with this project yet — the PN532
module is small enough to sit loose near the board, or design your own (the
sibling `esp32rental` project's `hardware/` STL files are for a different,
larger enclosure and don't fit this board).

## Using the station

The idle screen waits for a tag. What happens next depends on the tag:

- **A tag with a real manufacturer serial** (Bambu Lab, and sometimes
  OpenTag3D) — the station looks up that specific roll's current status and
  shows the one action that makes sense (Einbuchen if it isn't in stock,
  Ausbuchen if it is). Nothing is ever booked automatically just because a
  tag was detected.
- **A tag with no trustworthy serial** (OpenSpool, and Creality — see
  [Tag format support](#tag-format-support) for why Creality's own
  serial-like field isn't used) — the station shows the matching spec's
  current stock count and asks you to choose Einbuchen or Ausbuchen
  explicitly. Ausbuchen is hidden when stock is already zero.
- **OpenSpool tags carry no weight field** — you'll be asked to pick one from
  a few presets before the check-in/out screen appears.
- **An unrecognized tag** shows "Unbekanntes Tag-Format" and takes no action —
  add that spool manually via the web UI's Filament page instead.

The idle screen dims its backlight after a period of inactivity and wakes on
any touch or tag scan.

## Tag format support

This firmware reads real, independently-published reverse-engineering
research — it is not based on any of these vendors' official documentation
(none of the proprietary formats have any), so treat the "best-effort" ones
below as a solid starting point to verify against your own spools, not a
guarantee.

| Format | Tag type | Confidence | Notes |
|---|---|---|---|
| **OpenSpool** | NTAG215/216, NDEF `application/json` | High — the format is small, open, and unambiguous. | No serial number and no weight field by design (see [spuder/OpenSpool](https://github.com/spuder/OpenSpool)) — the station asks for a weight and shows an Einbuchen/Ausbuchen choice for every scan. |
| **OpenTag3D** | NTAG213/215/216, NDEF `application/opentag3d` | High for the byte layout ([opentag3d.info/spec](https://opentag3d.info/spec)) — but the spec calls its identifier field "Serial/**Batch** ID", so it may or may not be unique per physical spool depending on what the tag's writer chose to put there. | Used as the tracking serial when present; if that turns out to be a shared batch code in practice, two different spools from the same batch would be treated as the same one. Watch for this if you deploy OpenTag3D tags at scale. |
| **Bambu Lab** | MIFARE Classic 1K, keys derived from the tag UID via HKDF-SHA256 | Confirmed against a real spool — reading (all three sectors: filament type, color/weight, tray UID) works end to end. Block layout and key derivation come from [Bambu-Research-Group/RFID-Tag-Guide](https://github.com/Bambu-Research-Group/RFID-Tag-Guide) (`BambuLabRfid.md`, `deriveKeys.py`). | Carries a genuine per-spool "Tray UID", used as the tracking serial. That field can be raw binary rather than text on some spools — the firmware hex-encodes it in that case rather than assuming ASCII. |
| **Creality** | MIFARE Classic 1K, sector 1 (blocks 4–6) | Confirmed against real spools, two independently-documented layouts — see below. | See below. |

### Creality's two layouts

Creality tags come in two layouts, both recognized automatically (the parser
tries them in an order that can't confuse one for the other):

| Layout | Confidence | Notes |
|---|---|---|
| **"Old"** ([CrealityRfid.md](https://github.com/Bambu-Research-Group/RFID-Tag-Guide/blob/main/CrealityRfid.md)) — plaintext ASCII, default key | Low-medium — even that source's own authors weren't sure about several fields. | No weight field. Also carries a 4-hex-digit "Supplier" field with no confirmed hex-to-brand mapping — not decoded (brand always reports "Creality"). |
| **"K2/CFS"** ([DnG-Crafts/K2-RFID](https://github.com/DnG-Crafts/K2-RFID)) — Creality's newer K2-printer/CFS multi-material scheme; plain **or** AES-128-ECB-encrypted (per-tag key derived from the UID, block data separately encrypted) | Confirmed against three real spools, including one whose tag was written with the third-party DnG-Crafts Android app and read back correctly by a real Creality K2 Plus printer. | Vendor code `0276` is confirmed = Creality; other codes are shown honestly as `"Creality (Vendor <code>)"` rather than guessed. Has a real weight field via a small length-code lookup table. Its own 6-digit serial is **not** trusted as the tracking serial — real factory spools were found to reuse the same serial across different physical rolls, and it doesn't match the serial printed on the spool's own label either. |

**Material and brand are resolved via a real lookup table, not left opaque.**
`scripts/gen_creality_k2_materials.py` turns a Creality K2's own
`material_database.json` (found on the printer at
`/mnt/UDISK/creality/userdata/box/material_database.json`) into
`src/creality_k2_materials.h` (committed) — a material id → brand/name table.
This is a **snapshot, not a live lookup**: Creality's cloud catalog can grow
over time, and an id missing from the table just falls back to a raw
`"Material <id>"` display. Regenerate by dropping a fresh
`material_database.json` at the project root and re-running the script.

The write wizard (see below) only ever writes the K2 layout's plain
(unencrypted) variant to a blank card — it never writes the "old" layout.
Overwriting an already-encrypted K2 tag falls back to that tag's own
derived key and writes AES-encrypted data so it stays readable afterward.

### A note on false-positive reads

`0xFFFFFFFFFFFF` (the MIFARE Classic factory default key) authenticates
against any never-rekeyed card, not just spool tags — and some PN532 clones
can even report a "successful" read against a target that implements no
MIFARE Classic authentication at all (e.g. a tokenized contactless payment
card). This firmware validates that a "successful" read actually looks like
plausible tag content (printable ASCII, hex-digit fields where the format
documents them as hex) before accepting it as a match, falling through to the
next format — and eventually to "Unbekanntes Tag-Format" — otherwise.

### Serial diagnostics

Every scan (successful or not) prints diagnostics to `Serial` (115200 baud,
`pio device monitor`): a `UID=...` header, a labeled hex dump of every raw
block/NDEF payload the firmware attempted to read (not just the one that
ended up matching), and a final decoded-result-or-`UNKNOWN` line. On a failed
authentication it also prints which block/key attempt failed and the actual
key bytes tried. This is plain `Serial.printf()`, not gated behind a build
flag — useful for capturing a real spool's data when reporting an
unrecognized or misread tag.

### Known issues

- **Intermittent PN532/I2C read failures on some boards.** arduino-esp32
  3.x's I2C-NG driver has a known bug
  ([espressif/esp-idf#19041](https://github.com/espressif/esp-idf/issues/19041))
  that can cause sporadic `ESP_ERR_INVALID_STATE` errors, including during
  idle polling. The firmware mitigates this by recovering the I2C bus and
  retrying the affected PN532 operation (authenticate/read/write) up to 3
  times before reporting a real failure (`src/pn532_auth.h`/`.cpp`) — this
  hasn't been exhaustively confirmed to eliminate the issue under all
  conditions. If you see repeated "Authentifizierung fehlgeschlagen" errors
  against a tag that should work, check your I2C wiring/pull-ups first.
- **OpenSpool and OpenTag3D haven't been tested against real hardware** —
  the parsers follow each format's published spec closely, but only Bambu
  Lab and Creality tags have been verified against real spools so far.
- **Only tested on an ST7789 CYD board** — the ILI9341 environment compiles
  but hasn't been separately verified on real ILI9341 hardware.
- **The test-tag wizard's "Überschreiben" (force overwrite) path has only
  been verified for Creality tags** — the equivalent Bambu Lab fix was added
  by direct analogy and hasn't been tested against a real Bambu tag yet.

Any tag this firmware can't identify or parse shows "Unbekanntes Tag-Format"
and takes no action — add that spool manually via the web UI's Filament page
instead. If a real tag reads incorrectly (wrong field positions, wrong key,
etc.), the block/byte-offset constants are all named and commented in
`tag_bambu.cpp` / `tag_creality.cpp` / `tag_opentag3d.cpp`.

## Testtags programmieren

The station can also *write* a matched pair of test tags itself — useful for
testing without real vendor spools on hand. Tap **Programmieren** on the idle
screen, then:

1. Tag-Art (Bambu Lab / Creality / OpenSpool / OpenTag3D)
2. Material (PLA / PETG / ABS / TPU / ASA / PC / PA / HIPS / PVA / Holz — a
   fixed list, since this board has no keyboard for free-text entry)
3. Variante — an optional finish/effect on top of the base material (e.g.
   "PLA Luminous", "PLA Stardust", "PLA Hyper"): "-" (none), Silk, Luminous,
   Stardust, Glitter, Matte, Marmor, CF, GF, Transparent, Hyper. OpenTag3D
   stores base material and variant as two separate fields already; the
   other formats get the combined string.
4. Farbe (16 swatches)
5. Hersteller — **only asked for OpenSpool/OpenTag3D**, the two formats with
   a real, writable brand field (Generic / Creality / Bambu Lab / eSun /
   Overture / Sunlu / Prusament / Polymaker). Bambu Lab and Creality tags
   have no brand field at all — those parsers always report a fixed brand
   regardless of tag content, so there's nowhere to write a different
   manufacturer name.
6. Gewicht (only asked for Bambu/Creality/OpenTag3D — the formats with a
   weight field)
7. Bestätigen — shows a summary and, where applicable, a freshly generated
   serial that both tags will carry
8. Place the first physical tag, then the second — both get written with the
   *same* generated data, matching how a real spool carries the identical
   identifier on both faces (the tag's own hardware UID is never used as the
   tracking identity — see [Tag format support](#tag-format-support)).

**Abbrechen** works from every step, plus a 60-second inactivity timeout
returns to idle if the wizard is ever left mid-flow. This feature is pure NFC
I/O — it never calls the MakerSpaceAPI backend.

**Safety: it refuses to overwrite anything that already parses as a known
format.** Every write attempt first tries the normal read/identify path
against whatever tag is presented — if that succeeds (a real spool, or a tag
this wizard already wrote earlier), the write is refused with "Tag ist nicht
leer". A dedicated **Überschreiben** button on that screen lets you
deliberately force the overwrite instead (e.g. transferring a tag from one
spool onto another with different filament) — it only ever affects the exact
physical tag currently on the reader.

**Verification, not just "the write call returned success":** right after
writing, the firmware reads the tag back through the same parsing functions a
real scan uses and compares every field against what was requested. A tag
only shows as successfully written once that round-trip actually matches.

**What each format actually needs, physically:**

| Format | Blank card needed | What gets locked |
|---|---|---|
| **Bambu Lab** | Blank/never-rekeyed MIFARE Classic 1K | The three data blocks are written first with the MIFARE default key — only once all three succeed does it relock each sector's trailer to that sector's HKDF-derived key, so the tag becomes readable only with the derived key afterward, exactly like a genuine Bambu tag. |
| **Creality** | Blank/default-keyed MIFARE Classic 1K | Nothing — the K2/CFS layout's plain (unencrypted) variant is written, which is read with the default key too. |
| **OpenSpool** | Blank NTAG213/215/216 with its factory Capability Container intact | Nothing — a plain NDEF write. |
| **OpenTag3D** | Blank **NTAG215 or NTAG216** | Nothing — a plain NDEF write, but the format's payload needs more space than an NTAG213 has (~47 pages vs. ~36 usable) — use a bigger tag. |

Never present a tag you don't want overwritten while the wizard is waiting —
the "already written" check is the main safeguard, but it can only protect a
tag this firmware can actually recognize as already-written; a tag in some
entirely different, unrelated NFC format would still get overwritten if you
deliberately picked it.

## Architecture

| File | Responsibility |
|---|---|
| `main.cpp` | `setup()` / `loop()` — wires everything together |
| `display.h` | LovyanGFX `LGFX` class — ILI9341/ST7789 panel (build-flag selected) + XPT2046 touch, on two independent SPI buses |
| `ui.cpp` / `ui.h` | LVGL screens and the app state machine |
| `api.cpp` / `api.h` | HTTP calls to `/api/v1/filament/{scan,status,checkin,checkout}` |
| `nfc_reader.cpp/h` | PN532 polling task; dispatches a detected target to the right tag-format parser by UID length |
| `pn532_auth.cpp/h` | Retries a PN532 authenticate/read/write operation with I2C bus recovery between attempts |
| `tag_bambu.cpp/h`, `bambu_kdf.cpp/h` | Bambu Lab MIFARE Classic parsing + HKDF-SHA256 key derivation (via mbedtls) |
| `tag_creality.cpp/h` | Creality MIFARE Classic parsing — two layouts, plain or AES-encrypted (via mbedtls) |
| `creality_k2_materials.h` | Auto-generated K2/CFS material id → brand/name lookup table (see `scripts/gen_creality_k2_materials.py`) |
| `tag_openspool.cpp/h`, `tag_opentag3d.cpp/h` | NDEF-based formats, share `ndef.cpp/h` (a minimal Type-2-Tag NDEF reader) |
| `tag_types.h` | Shared `FilamentTagData` struct all four parsers produce |
| `serial_gen.h` | Random hex-string generator for the test-tag wizard's invented serials |
| `hexdump.h` | Serial hex-dump helper — see [Serial diagnostics](#serial-diagnostics) |
| `strings.h` | German-only UI strings (this is an internal tool — see `esp32rental`'s `i18n.h` for an EN/DE toggle pattern if that's ever needed here) |
| `led.h` | On-board RGB LED helpers |
| `config.h` | All pin assignments, WiFi credentials, timeouts |
| `lv_conf.h` | LVGL 8.3 configuration (project root) |

### Fonts (umlauts)

LVGL's built-in `lv_font_montserrat_*` fonts only cover `0x20`–`0x7F` plus
degree/bullet — no umlauts at all. `src/fonts/montserrat_ext_{14,16,20}.c`
are pre-generated (via `scripts/gen_fonts.sh`) and already committed, with
`-DLVGL_EXT_FONTS` on by default in `platformio.ini` — you don't need to run
anything to get proper ä/ö/ü/ß rendering. Re-run the script only if you
change the UI's font sizes or need a wider character range.

### Why the app-level HTTP calls block

`ui.cpp` calls `api_scan()`/`api_status()`/`api_checkin()`/`api_checkout()`
directly and synchronously from the tag-scanned handler, unlike
`esp32rental`'s `ui_tick()`-driven async NFC login flow. That pattern exists
there specifically to avoid blocking the PN532 hardware polling task; here,
the HTTP call happens in the *main loop* after a tag has already been fully
read out of the NFC queue by its own FreeRTOS task, so blocking it briefly (a
single POST, typically well under a second) doesn't affect NFC polling at
all — it only pauses `lv_timer_handler()` for a moment, and the
processing screen is force-rendered first so it's actually visible while the
request is in flight.
