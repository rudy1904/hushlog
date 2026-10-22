# Hushlog firmware

Test firmware for the Hushlog board. It's a **bring-up test**, not a finished
logger: it powers up each peripheral, proves it responds, and streams readings
over USB serial, SD and the OLED so the hardware can be confirmed before the
real logging code is written.

## What it does

- Configures the **ADS1220** (24-bit ADC) over SPI0: internal 2.048 V reference,
  internal oscillator, PGA gain 1, 20 SPS.
- Reads two differential channels (AIN0/AIN1 and AIN2/AIN3) in a loop.
- Converts the raw 24-bit code to volts and prints it as CSV over USB serial.
- Logs the same CSV to **microSD** (`hushlog.csv`) over SPI1, flushing each line.
- Shows live readings on the **SSD1306 OLED** over I2C1.
- Uses the **status LED** (GPIO25) as a heartbeat, blinking fast if SD is missing.

## Build

Built with the [Arduino-Pico core](https://github.com/earlephilhower/arduino-pico)
via PlatformIO.

    pio run              # compile
    pio run -t upload    # flash (RP2040 in bootloader mode)
    pio device monitor   # watch the CSV stream at 115200 baud

## Pin map (matches the schematic)

| Function | Bus | Pins |
|---|---|---|
| ADS1220 | SPI0 | SCLK=GPIO2, MOSI=GPIO3, MISO=GPIO4, CS=GPIO5, DRDY=GPIO6 |
| microSD | SPI1 | SCLK=GPIO10, MOSI=GPIO11, MISO=GPIO12, CS=GPIO13 |
| OLED | I2C1 | SDA=GPIO14, SCL=GPIO15 |
| Status LED | - | GPIO25 |
| UART debug | UART0 | TX=GPIO0, RX=GPIO1 |

## TODO for a real logger

- [ ] Don't write SD / refresh OLED mid-conversion (keeps switching noise out of
      the measurement - the reason the board has a separate analog supply).
- [ ] Use the DRDY interrupt instead of polling.
- [ ] Set PGA gain per channel for small signals.
- [ ] Configurable sample rate and a file-per-run naming scheme.
- [ ] Noise-floor test: short the inputs, log, compare to the datasheet.
