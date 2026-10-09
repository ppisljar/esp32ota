# ESP32 OTA updater

Minimal OTA web server, flashed to the `ota_1` partition. Receives a firmware
or data image over plain HTTP and writes it to a target partition, then reboots.
Built for ESP-IDF v5.5.2.

## Target (chip)

**Target-neutral — the chip is NOT pinned in `sdkconfig.defaults`.** Pick it with
`idf.py set-target <chip>`; per-target flash size lives in
`sdkconfig.defaults.<target>` (`esp32` → 4 MB, `esp32s3` → 16 MB), which ESP-IDF
loads automatically on top of the shared defaults.

> **The updater image must be built for the SAME chip as the device it is flashed
> onto.** A wrong-chip image in `ota_1` leaves the device unable to update over
> the air — it has to be recovered over USB. `freeesp32_ave/flash_all.sh` handles
> this for you: it reads `CONFIG_IDF_TARGET` from the main app's sdkconfig and
> re-targets this project to match before building.

Verified on `esp32` (classic, 4 MB boards) and `esp32s3` (YB-ESP32-S3-DAC,
16 MB). Note the bootloader offset differs by chip (`0x1000` on classic ESP32,
`0x0` on S3) — but only this project's *app* image is ever written to a device,
so that only matters to whoever assembles the flash command.

## Partition table

`partitions.csv` here is a **build-time placeholder only**. The updater resolves
every partition at runtime from whatever table the device actually has (via
`esp_ota_*` / `esp_partition_*`), so it does not need to match the host
project's table — and its own table is never flashed. The placeholder exists
purely so a standalone `idf.py build` succeeds.

## WiFi

On boot it reads STA credentials from NVS namespace `"ota"` (keys `"ssid"` and
`"pass"`, plain strings — the main app populates these before rebooting into OTA
mode). If they are present it joins that network; otherwise (or on failure) it
falls back to a SoftAP:

- SSID `ESP32-AVE-Setup`, password `entrain123`, device at `http://192.168.4.1`

## HTTP endpoints

- `GET  /`        one-line status: `OTA updater ready (running: <part> @ <addr>)`
- `POST /update`  stream an image to a partition (see targeting below)
- `GET  /reboot`  set boot to the app slot and restart

### `/update` targeting

- `POST /update`                       → next app OTA partition (default; main-app slot)
- `POST /update?part=<label>`          → partition by label (app or data)
- `POST /update?type=app|data&subtype=<n>` → partition by type/subtype

App targets use `esp_ota_begin/write/end` + `esp_ota_set_boot_partition` then
reboot. Data targets (e.g. SPIFFS `storage`/`cfgfs`) are erased and written
directly with no boot change. The upload must fit within the target partition.

## Build

```bash
source ~/.espressif/v5.5.2/esp-idf/export.sh

idf.py set-target esp32      # classic 4 MB boards
#   or
idf.py set-target esp32s3    # YB-ESP32-S3-DAC, 16 MB

idf.py build                 # -> build/esp32_minimal_ota.bin
```

Switching target wipes `sdkconfig` and forces a full rebuild, so only do it when
the chip actually changes.

Normally you do not build this by hand — `freeesp32_ave/flash_all.sh` builds it
(for the right chip) and writes it to `ota_1` as part of the one-time wired
flash.
