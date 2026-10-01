# Hardware

The board, what each pin does, and how the 4 MB flash is laid out. Two
properties of a device's first flash are one-way: once a device has them, no
firmware update can take them back. They are why this page matters before
you touch `partitions.csv` or the bootloader settings.

## Board

| Part | What the firmware uses |
|------|------------------------|
| Board | Adafruit MagTag 2.9" (2025 revision) |
| MCU | ESP32-S2, single core. The shipped config runs it at 160 MHz, the ESP-IDF default (the chip can do 240). |
| Memory | 4 MB flash, 2 MB PSRAM (`CONFIG_SPIRAM=y`; boards without it still boot) |
| Display | 2.9" e-ink, 296×128, 1 bit per pixel, SSD1680 controller on SPI2 at 4 MHz. Panel FPC-7519 rev. b, x-RAM offset 0. |
| Radio | WiFi 802.11 b/g/n, 2.4 GHz only |
| Clock | No RTC chip. The ESP32-S2's RTC timer keeps time through deep sleep and software resets; power-on loses it until NTP. |
| Power | USB-C, or a 3.7 V LiPo on the JST connector |

## Pins

| GPIO | Function | Owner |
|------|----------|-------|
| 15, 14, 12, 11 | Buttons A, B, C, D. Active low, pulled up. All four are RTC-capable, so they can wake the chip from deep sleep. | `main/buttons.c` |
| 1 | NeoPixel data (4 × RGB) | `main/neopixel.c` |
| 21 | NeoPixel power gate: LOW = on, HIGH = off | `main/neopixel.c` |
| 17 | Speaker, driven by DAC channel 0 | `main/audio.c` |
| 16 | Amplifier enable: HIGH = on | `main/audio.c` |
| 4 | Battery voltage through a 100k/100k divider (ADC1 channel 3) | `main/battery.c` |
| 3 | ALS-PT19 ambient light sensor (ADC1 channel 2) | `main/light.c` |
| 36, 35, 8, 7, 6, 5 | Display SCLK, MOSI, CS, DC, RST, BUSY | `main/display.c` → `components/ssd1680` |

GPIO 21 and GPIO 16 are held through deep sleep so neither the LEDs nor the
amplifier can drift on overnight ([invariant S42](../architecture.md#system-invariants)).
Driver details are in [peripherals.md](peripherals.md).

## Flash layout

[`partitions.csv`](../../partitions.csv) is the authority
(`CONFIG_PARTITION_TABLE_CUSTOM`), and its header comment records how the
offsets got where they are. In outline:

| Partition | Holds |
|-----------|-------|
| `nvs` | Settings, credentials, the timer snapshot, chore records, OTA status ([state.md](state.md)) |
| `phy_init` | RF calibration |
| `ota_0`, `ota_1` | Two app slots of 1.75 MB (`0x1C0000`) each. OTA writes the one not running. |
| `otadata` | Which slot boots, and its verify state |
| `assets` | The optional custom alert WAV (376 KB, about 12 s at 16 kHz), written by `tools/flash_assets.sh` |
| `coredump` | 64 KB at the end of flash. Unused unless `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` is turned on, which `sdkconfig.defaults` does not do. |

After any partition-table change, re-run `tools/flash_assets.sh` on a device
with a custom WAV ([developer_setup.md](../developer_setup.md#custom-alert-wav)).

### The two one-way constraints

**The app slot size is frozen.** OTA writes an app slot; it cannot rewrite
the partition table. The table a device received on its first OTA-capable
serial flash is the table it keeps, short of a cable to every device. So the
slots stay at `0x1C0000` for good, and `nvs`, `phy_init` and `ota_0` have
never moved. That is what lets a serial reflash over an in-service device keep
its NVS: settings, the day's snapshot and chore records all survive
([developer_setup.md](../developer_setup.md#what-a-flash-does-to-nvs)).

Because the headroom cannot be renegotiated, the build guards it: the build
fails once the image passes 85 % of the slot. [developer_setup.md](../developer_setup.md)
says how to read and raise the limit.

**The bootloader is not OTA-updatable, and rollback lives in it.** With
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, a newly written slot boots as
"pending verify", and the bootloader reverts to the other slot unless the app
certifies itself during that boot. Only the second-stage bootloader can do the
revert, and OTA never rewrites the bootloader. So rollback protection had to
be in the bootloader that the first serial flash wrote, and it can never be
added or removed afterward. It is set in `sdkconfig.defaults`, whose comment
above the symbol covers its cost and its precedence against a generated
`sdkconfig`. How the app certifies itself is in [ota.md](ota.md).
