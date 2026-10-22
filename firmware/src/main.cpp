/*
 * Hushlog - test firmware
 * ------------------------
 * Brings up each peripheral on the board and proves it works:
 *   - ADS1220 24-bit ADC over SPI0  (reads 2 differential channels)
 *   - microSD card over SPI1        (logs timestamped samples as CSV)
 *   - SSD1306 OLED over I2C1        (shows live readings)
 *   - status LED on GPIO25          (heartbeat + SD-error indicator)
 *   - USB serial / UART             (prints every sample for debugging)
 *
 * Target: RP2040, built with the Arduino-Pico core (earlephilhower).
 *
 * This is a bring-up test, not a finished logger. It reads the ADC with
 * the PGA at a fixed gain, prints and logs the raw code and the voltage,
 * and does nothing clever with timing yet. The point is to confirm every
 * chip on the board responds before writing the real logging loop.
 *
 * Pin map (from the Hushlog schematic):
 *   ADS1220 (SPI0):  SCLK=GPIO2  MOSI=GPIO3  MISO=GPIO4  CS=GPIO5  DRDY=GPIO6
 *   microSD (SPI1):  SCLK=GPIO10 MOSI=GPIO11 MISO=GPIO12 CS=GPIO13
 *   OLED    (I2C1):  SDA=GPIO14  SCL=GPIO15
 *   status LED:      GPIO25
 *   UART0 debug:     TX=GPIO0    RX=GPIO1
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- pin definitions ----------
#define ADC_SCLK   2
#define ADC_MOSI   3
#define ADC_MISO   4
#define ADC_CS     5
#define ADC_DRDY   6

#define SD_SCLK   10
#define SD_MOSI   11
#define SD_MISO   12
#define SD_CS     13

#define OLED_SDA  14
#define OLED_SCL  15

#define LED_PIN   25

// ---------- ADS1220 commands (datasheet table 15) ----------
#define ADS1220_CMD_RESET   0x06
#define ADS1220_CMD_START   0x08
#define ADS1220_CMD_RDATA   0x10
#define ADS1220_CMD_WREG    0x40
#define ADS1220_CMD_RREG    0x20

// SPI mode 1 (CPOL=0, CPHA=1), MSB first, <= 4 MHz for the ADS1220.
SPISettings adcSpi(1000000, MSBFIRST, SPI_MODE1);

#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire1, -1);

SdFat sd;
SdFile logFile;
bool sdOK = false;

// Internal reference 2.048 V; full-scale code is +/- 2^23.
const float VREF = 2.048f;
const float FULL_SCALE = 8388608.0f;

void adcWriteReg(uint8_t reg, uint8_t value) {
  SPI.beginTransaction(adcSpi);
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(ADS1220_CMD_WREG | (reg << 2) | 0x00);
  SPI.transfer(value);
  digitalWrite(ADC_CS, HIGH);
  SPI.endTransaction();
}

uint8_t adcReadReg(uint8_t reg) {
  SPI.beginTransaction(adcSpi);
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(ADS1220_CMD_RREG | (reg << 2) | 0x00);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(ADC_CS, HIGH);
  SPI.endTransaction();
  return v;
}

void adcCommand(uint8_t cmd) {
  SPI.beginTransaction(adcSpi);
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(cmd);
  digitalWrite(ADC_CS, HIGH);
  SPI.endTransaction();
}

int32_t adcReadData() {
  SPI.beginTransaction(adcSpi);
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(ADS1220_CMD_RDATA);
  uint8_t b2 = SPI.transfer(0xFF);
  uint8_t b1 = SPI.transfer(0xFF);
  uint8_t b0 = SPI.transfer(0xFF);
  digitalWrite(ADC_CS, HIGH);
  SPI.endTransaction();
  int32_t raw = ((int32_t)b2 << 16) | ((int32_t)b1 << 8) | b0;
  if (raw & 0x800000) raw |= 0xFF000000; // sign-extend 24->32
  return raw;
}

void adcConfigure() {
  adcCommand(ADS1220_CMD_RESET);
  delay(1);
  adcWriteReg(0x00, 0x00); // MUX=AIN0-AIN1, gain 1, PGA enabled
  adcWriteReg(0x01, 0x00); // 20 SPS, single-shot
  adcWriteReg(0x02, 0x00); // internal 2.048V ref
  adcWriteReg(0x03, 0x00);
}

void adcSetChannel(uint8_t pair) {
  uint8_t mux = (pair == 0) ? 0x00 : 0x50; // AIN0/AIN1 or AIN2/AIN3
  uint8_t reg0 = adcReadReg(0x00) & 0x0F;
  adcWriteReg(0x00, mux | reg0);
}

int32_t adcReadChannel(uint8_t pair) {
  adcSetChannel(pair);
  adcCommand(ADS1220_CMD_START);
  uint32_t t0 = millis();
  while (digitalRead(ADC_DRDY) == HIGH) {
    if (millis() - t0 > 100) break;
  }
  return adcReadData();
}

float codeToVolts(int32_t code) {
  return ((float)code / FULL_SCALE) * VREF;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { }
  Serial.println();
  Serial.println("Hushlog test firmware starting...");

  // ADS1220 on SPI0
  SPI.setSCK(ADC_SCLK);
  SPI.setTX(ADC_MOSI);
  SPI.setRX(ADC_MISO);
  pinMode(ADC_CS, OUTPUT);
  digitalWrite(ADC_CS, HIGH);
  pinMode(ADC_DRDY, INPUT);
  SPI.begin();
  adcConfigure();
  uint8_t reg0 = adcReadReg(0x00);
  Serial.print("ADS1220 reg0 readback: 0x");
  Serial.println(reg0, HEX);
  Serial.println(reg0 == 0x00 ? "  ADC responding." : "  ADC NOT responding - check wiring/CS.");

  // OLED on I2C1
  Wire1.setSDA(OLED_SDA);
  Wire1.setSCL(OLED_SCL);
  Wire1.begin();
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Hushlog");
    oled.println("booting...");
    oled.display();
    Serial.println("OLED OK.");
  } else {
    Serial.println("OLED NOT found at 0x3C.");
  }

  // microSD on SPI1
  SPI1.setSCK(SD_SCLK);
  SPI1.setTX(SD_MOSI);
  SPI1.setRX(SD_MISO);
  SdSpiConfig cfg(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(8), &SPI1);
  if (sd.begin(cfg)) {
    sdOK = true;
    if (logFile.open("hushlog.csv", O_WRITE | O_CREAT | O_APPEND)) {
      logFile.println("millis,ch0_code,ch0_volts,ch1_code,ch1_volts");
      logFile.sync();
    }
    Serial.println("SD OK, logging to hushlog.csv");
  } else {
    Serial.println("SD NOT found - will print only, no logging.");
  }

  digitalWrite(LED_PIN, LOW);
  Serial.println("Setup done. Reading...");
  Serial.println("millis,ch0_code,ch0_volts,ch1_code,ch1_volts");
}

void loop() {
  digitalWrite(LED_PIN, HIGH);

  int32_t c0 = adcReadChannel(0);
  int32_t c1 = adcReadChannel(1);
  float v0 = codeToVolts(c0);
  float v1 = codeToVolts(c1);
  uint32_t ts = millis();

  Serial.print(ts);   Serial.print(',');
  Serial.print(c0);   Serial.print(',');
  Serial.print(v0, 6); Serial.print(',');
  Serial.print(c1);   Serial.print(',');
  Serial.println(v1, 6);

  if (sdOK) {
    logFile.print(ts);   logFile.print(',');
    logFile.print(c0);   logFile.print(',');
    logFile.print(v0, 6); logFile.print(',');
    logFile.print(c1);   logFile.print(',');
    logFile.println(v1, 6);
    logFile.sync();
  } else {
    digitalWrite(LED_PIN, LOW); delay(80);
    digitalWrite(LED_PIN, HIGH); delay(80);
  }

  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("Hushlog");
  oled.print("CH0 "); oled.print(v0, 4); oled.println(" V");
  oled.print("CH1 "); oled.print(v1, 4); oled.println(" V");
  oled.print(sdOK ? "SD: logging" : "SD: none");
  oled.display();

  digitalWrite(LED_PIN, LOW);
  delay(500);
}
