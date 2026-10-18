# Hardware setup

How to connect an ESP32 to a 64×64 HUB75 LED panel and power it. For flashing the firmware afterwards, see [Install](../README.md#install).

## What you need

| Part | Notes |
| --- | --- |
| Classic ESP32 board | ESP32-WROOM-32 with 4 MiB flash or more. See [Supported boards](#supported-boards). |
| 64×64 HUB75 panel | P2.5, P3 and P4 all work; only the pitch (and so the size) differs. It must have an **E** address line (1/32 scan). |
| 5 V power supply | 4 A for one panel at normal brightness; 5 A or more leaves room for full white. |
| 16-pin HUB75 ribbon cable | Usually comes with the panel. |
| 14 jumper wires | Female-to-female for pin headers, or a breakout/shield (see [Wiring options](#wiring-options)). |
| USB data cable | For the first flash. Charge-only cables do not work. |
| Optional: LDR and 10 kΩ resistor | For automatic brightness. |

Not supported: 64×32 panels, ESP32-S2, S3, C3 and C6 boards (the driver uses the classic ESP32's I2S peripheral), and boards with PSRAM modules (WROVER, see [Pin conflicts](#pin-conflicts)).

## Default wiring

The firmware drives the panel with these GPIOs. They are fixed in the firmware, so use exactly these pins.

| HUB75 signal | ESP32 GPIO | HUB75 signal | ESP32 GPIO |
| --- | --- | --- | --- |
| R1 | 25 | A | 23 |
| G1 | 26 | B | 19 |
| B1 | 27 | C | 5 |
| R2 | 14 | D | 17 |
| G2 | 12 | E | 18 |
| B2 | 13 | CLK | 16 |
| LAT | 4 | OE | 15 |

Two ground wires (HUB75 pins 4 and 16) go to any ESP32 GND pin. Connect both.

### HUB75 connector

Looking at the panel's **input** connector (usually marked with an arrow pointing into the panel, or "IN"), the standard 2×8 pinout is:

```
        ┌───────┐
  R1  1 │ ●   ● │ 2  G1
  B1  3 │ ●   ● │ 4  GND
  R2  5 │ ●   ● │ 6  G2
  B2  7 │ ●   ● │ 8  E
   A  9 │ ●   ● │ 10 B
   C 11 │ ●   ● │ 12 D
 CLK 13 │ ●   ● │ 14 LAT
  OE 15 │ ●   ● │ 16 GND
        └───────┘
```

The key notch (a gap in the shroud) is usually at the top. If your panel's silkscreen disagrees with this drawing, trust the silkscreen: manufacturers differ.

**Check the E line.** Most 64×64 panels have E on pin 8, as shown. A few put E on pin 4 (and a ground on pin 8) or have solder pads to choose. If the bottom half of the picture is scrambled or missing after wiring, check the panel's documentation for where E goes. Wire the E signal (GPIO 18) to the pin your panel uses.

### Pin map by board

The GPIO numbers are the same on every classic ESP32. Only the silkscreen labels differ. Look for `GPIOn`, `IOn` or `Dn` next to the pin.

| Signal | GPIO | ESP32 DevKit V1 (DOIT, 30/38 pin) | ESP32-DevKitC V4 | NodeMCU-32S |
| --- | --- | --- | --- | --- |
| R1 | 25 | D25 | IO25 | IO25 |
| G1 | 26 | D26 | IO26 | IO26 |
| B1 | 27 | D27 | IO27 | IO27 |
| R2 | 14 | D14 | IO14 | IO14 |
| G2 | 12 | D12 | IO12 | IO12 |
| B2 | 13 | D13 | IO13 | IO13 |
| A | 23 | D23 | IO23 | IO23 |
| B | 19 | D19 | IO19 | IO19 |
| C | 5 | D5 | IO5 | IO5 |
| D | 17 | TX2 | IO17 | IO17 |
| E | 18 | D18 | IO18 | IO18 |
| CLK | 16 | RX2 | IO16 | IO16 |
| LAT | 4 | D4 | IO4 | IO4 |
| OE | 15 | D15 | IO15 | IO15 |
| GND | – | GND (two) | GND (two) | GND (two) |

On the DOIT DevKit V1, GPIO 16 and 17 are labelled RX2 and TX2, which is the one trap in that table. They are ordinary GPIOs here, not a serial port.

### Wemos D1 R32 (Arduino Uno shape)

The D1 R32 (also sold as "ESP32 D1 R32" or "D1 Uno32") has the Arduino Uno layout and labels most pins `D0` to `D13` and `A0` to `A5` next to the GPIO number. The GPIOs are the same as above; only the names differ.

| Signal | GPIO | D1 R32 label | Header |
| --- | --- | --- | --- |
| R1 | 25 | D3 | digital |
| G1 | 26 | D2 | digital |
| B1 | 27 | D6 | digital |
| R2 | 14 | D7 | digital |
| G2 | 12 | D8 | digital |
| B2 | 13 | D9 | digital |
| A | 23 | D11 | digital |
| B | 19 | D12 | digital |
| C | 5 | D10 | digital |
| D | 17 | D4 | digital |
| E | 18 | D13 | digital |
| CLK | 16 | D5 | digital |
| LAT | 4 | A1 | analog |
| OE | 15 | IO15 | extra pad row, see below |
| GND | – | GND (two) | power or digital header |

- **OE (GPIO 15) is not on an Uno header.** It is on a separate 1×3 pad row beside the power header, labelled `IO15`, and it is usually not soldered. Solder a wire or a pin header there.
- **LAT is on A1.** Analog pins work as plain digital outputs here.
- **Do not use the board's A0.** It is GPIO 2, which also drives the on-board LED, and it is not used by the panel.
- **Silkscreen errors.** On some boards the labels on A2 and A4 are wrong (they print `IO36` and `IO38` but the pins are GPIO 35 and 36). These two pins are not used for the panel, but are relevant if you wire a light sensor: use GPIO 35 or 36 by number, not by the printed label.
- **Uno shields do not fit.** A normal Uno HUB75 shield assumes Uno pins. This pinout does not match any shield, so wire with jumpers.
- The light sensor default (GPIO 35) is the pin labelled `A2` on the D1 R32.

## Supported boards

The firmware is built and tested on a classic ESP32 Dev Module with 4 MiB flash. Any board with the same chip and pins works the same way, since the GPIO numbers above are all it needs.

| Board | Works | Notes |
| --- | --- | --- |
| ESP32 DevKit V1 (DOIT) | Yes | The most common board; the table above is for it. 30 and 38 pin versions both expose all 14 pins. |
| ESP32-DevKitC V4 (WROOM-32E/32D) | Yes | Espressif's own board. Pick the WROOM, not the WROVER variant. |
| NodeMCU-32S and similar clones | Yes | Same module and pins as the DevKit V1. |
| Wemos D1 R32 (Uno shape) | Yes, with a solder joint | Pin names are `D2`–`D13`, `A1` and `IO15`; see [its pin map](#wemos-d1-r32-arduino-uno-shape). `IO15` (OE) is on an unpopulated pad row. |
| ESP32 Trinity (Brian Lough) | Yes | Has a HUB75 connector wired to this exact pinout, so no jumper wires are needed. |
| Other WROOM-32 boards | Usually | Check that GPIO 4, 5, 12–19, 23, 25–27 are all brought out. Some small boards (Lolin D32, Feather-style, mini boards) do not expose every pin or label them differently. |
| ESP32-WROVER boards, ESP32-CAM | No | They use GPIO 16 and 17 for PSRAM or the camera. |
| ESP32-S2, S3, C3, C6 | No | Not supported by the panel driver. |

Boards other than the DevKit V1 class have not been tested by the project. If you get one working, a note in an issue helps others.

## Wiring options

### Jumper wires

Fine for a first test. Use female-to-female jumpers between the board's pin headers and the panel's connector, or a ribbon cable with a breakout adapter.

Keep the signal wires short (under 20 cm) and, if you can, twist a ground wire along the CLK wire. The clock runs at up to 20 MHz, and long loose wires cause flicker and noise.

### Breakout or shield

A small HUB75 adapter board (a 2×8 pin header on one side, labelled screw terminals on the other) saves most of the wiring, and gives you proper power terminals. Any board that exposes the standard HUB75 pins and lets you pick GPIOs works. Wire it to the table above.

### Dedicated ESP32 HUB75 boards

Boards built for HUB75 (for example the ESP32 Trinity) connect straight to the panel ribbon and take 5 V in one place. These are the easiest option if you want a tidy build. Check that the board's printed pin map matches the default wiring before relying on it. If it does not, you cannot change the pins in settings, so you would need to wire to the default GPIOs yourself.

## Power

An LED panel draws far more than an ESP32 can supply. Power them separately and connect the grounds.

1. Connect the 5 V supply to the panel's **power connector** (the red and black screw terminals or the four-pin power header), using the thick wires that came with it. Use both power leads if the panel has two.
2. Connect the supply's 5 V and GND to the ESP32's **5V/VIN** and **GND** pins as well, so one supply runs both. Do this with a separate short pair of wires, not by passing power through the signal cable.
3. Do **not** power the panel from the ESP32's 5 V pin. It cannot carry the current.
4. Make sure the grounds of the supply, panel and ESP32 are joined. Without a common ground, the signals have no reference and the panel shows noise or nothing.
5. Add a 1000 µF electrolytic capacitor across the 5 V and GND where the supply meets the panel if you see flicker or random resets at full brightness. Mind the polarity.

Notes:

- A 64×64 panel can pull about 4 A with every LED white. Typical clock use is far lower, but size the supply for the peak.
- If you also connect USB while the supply powers the board through VIN, it is normally fine on boards with a protection diode. If you are unsure, disconnect the supply's 5 V from VIN while flashing and power the ESP32 from USB alone (the panel stays off, which is fine).
- Use a supply with a stable 5 V. A cheap supply that sags under load shows up as dim colours or a restarting clock.

## Light sensor (optional)

Automatic brightness needs a light-dependent resistor. Wire a voltage divider:

```
3.3 V ──── LDR ────┬──── GPIO 35
                   │
                10 kΩ
                   │
                  GND
```

GPIO 35 is the default. The settings page accepts GPIO 32, 33, 34, 35, 36 or 39, which are the ADC1 pins. GPIO 34 to 39 are input-only, which suits a sensor. After wiring, open the settings page and turn on Automatic brightness. Set the dark and light calibration values while the sensor is in each condition.

## Pin conflicts

These behaviours come from the ESP32 itself, not from this firmware. They matter if the clock misbehaves at boot.

- **GPIO 12 (G2)** is a strapping pin that selects the flash voltage. Most boards with a 3.3 V flash are fine. If the panel pulls it high at power-up on a board with 1.8 V flash, boot fails; this is unusual with WROOM-32 modules.
- **GPIO 5 and 15** are strapping pins too. 15 also controls the early boot log. Panel inputs normally do not hold them in the wrong state, but if a board will not boot with the panel connected, unplug the panel to confirm.
- **GPIO 16 and 17** are used for PSRAM on WROVER modules. That is why WROVER boards do not work here.
- **GPIO 14, 12, 13** are JTAG pins, which is fine unless you use a JTAG debugger.

If the board refuses to boot only with the panel plugged in, disconnect the HUB75 ribbon, power up, and reconnect after boot to see which pin is the culprit.

## First power-up

1. Check all wires against the table once more, and that no wire is shifted by one pin (the most common mistake).
2. Flash the firmware over USB with the [browser installer](https://vandalon.github.io/ultimatrix/), or build it yourself: see [Install](../README.md#install). The panel can be connected or unpowered while flashing.
3. Power the panel and the ESP32. The U logo should wipe in, then a blue pulse while it connects to Wi-Fi. On a fresh install the clock shows setup instructions and a QR code for the access point `UM-SETUP`.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Panel stays black | No panel power or no common ground. Check the 5 V and GND connections; check that the ribbon is in the **input** connector. |
| Only the top half (or bottom half) works | R2/G2/B2 wires wrong or loose (GPIO 14, 12, 13). |
| Colours wrong (red OK, green and blue swapped) | Panel wiring order differs. Use the **swap blue and green** option on the settings page; it needs **Save & restart**. |
| Picture repeats or looks vertically scrambled | Address lines A–E wrong or loose (GPIO 23, 19, 5, 17, 18). Check E in particular for 64×64 panels. |
| Picture is mirrored or rotated | Change the display orientation on the settings page. |
| Flicker or noise, worse with long wires | Shorten and twist the signal wires; add a capacitor; the firmware also lowers colour quality automatically if it detects memory or timing trouble (Settings, Advanced, Display quality). |
| Random reboots at high brightness | Power supply too weak or wires too thin. Power the panel directly from the supply, and use a stronger supply. |
| Board does not boot with the panel connected | A strapping pin is being held. See [Pin conflicts](#pin-conflicts). |
| Dim, washed-out colours | Supply voltage sags under load. Measure 5 V at the panel connector. |

## Chaining and other sizes

The firmware supports a single 64×64 panel. Chains of several panels and 64×32 panels are not supported.
