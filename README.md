# Shed Heater 2000

This is a simple mod for a cheap wall mountable heater.

I've built it because the original controller didn't work as it was supposed to. So I figured out how the "motherboard" works, and made a new controller for it using an ESP8266.

The new controller implements a very simple hysteresis based temperature control, which should keep my shed above freezing during winter.

## Hardware

The original controller was connected to the "motherboard" using a ribbon cable with 6 wires. The pinout is as follows:

| Original cable | ESP8266 | Function             |
|:--------------:|:-------:|:---------------------|
| 5: A1          | A1      | Thermistor           |
| 4: D2          | D2      | Fan Active High      |
| 3: D7          | D7      | Heater 2 Active High |
| 2: D6          | D6      | Heater 1 Active High |
| 1: GND         | GND     | Ground               |
| 0: +5          |         | +5V                  |

## BOM

- ESP8266 (I used a Wemos D1 Mini)
- 10k resistor
- Two buttons
- One panel mountable USB plug
- One laser cut piece of MDF
- One Hilink 5V 4A power supply (or similar)
