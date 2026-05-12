// ── Pulse Monitor BLE Firmware ─────────────────────────────────
// MAX30102 real sensor readings (HR + SpO2) over BLE.
// Falls back to simulation if sensor not found.
// Board: ESP32 Dev Module  |  Serial: 115200 baud
// Partition: Huge APP (3MB No OTA/1MB SPIFFS)
//
// Wiring:
//   MAX30102  →  ESP32
//   VIN       →  3.3V
//   GND       →  GND
//   SDA       →  GPIO 23
//   SCL       →  GPIO 22

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "MAX30105.h"
#include "heartRate.h"

// ── I2C pins ────────────────────────────────────────────────────
#define SDA_PIN 23
#define SCL_PIN 22

// ── BLE UUIDs — must match useBluetooth.js ──────────────────────
#define SERVICE_UUID   "4fafc201-1fb5-459e-8bcc-c5c9c331914b"
#define HR_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SPO2_CHAR_UUID "13210332-9cb7-4a00-b6a4-c7aa32fc8476"

// ── Finger detection ────────────────────────────────────────────
#define FINGER_ON   7000UL
#define FINGER_OFF  4000UL

// ── SpO2 sliding window ─────────────────────────────────────────
#define WIN 50
static uint32_t irBuf[WIN];
static uint32_t rdBuf[WIN];
static uint8_t  winIdx  = 0;
static bool     winFull = false;

// ── BLE handles ─────────────────────────────────────────────────
BLECharacteristic* pHRChar   = nullptr;
BLECharacteristic* pSpO2Char = nullptr;
bool clientConnected = false;

// ── Sensor state ────────────────────────────────────────────────
MAX30105 particleSensor;
bool     sensorFound = false;
bool     fingerOn    = false;

// ── HR averaging ────────────────────────────────────────────────
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE] = {0};
byte  rateSpot   = 0;
byte  validRates = 0;
long  lastBeat   = 0;
float beatsPerMinute = 0;
int   beatAvg    = 0;

// ── Simulation state ────────────────────────────────────────────
int simHr   = 72;
int simSpo2 = 97;

// ── Timing ──────────────────────────────────────────────────────
unsigned long lastSend   = 0;
unsigned long lastStatus = 0;
uint8_t lastSentHr   = 0;
uint8_t lastSentSpo2 = 0;

// ── BLE callbacks ───────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) {
    clientConnected = true;
    Serial.println(">>> Client CONNECTED");
  }
  void onDisconnect(BLEServer*) {
    clientConnected = false;
    Serial.println(">>> Client DISCONNECTED — re-advertising");
    BLEDevice::startAdvertising();
  }
};

// ── SpO2 calculation ────────────────────────────────────────────
uint8_t calcSpO2() {
  uint32_t dcIR = 0, dcRed = 0;
  for (int i = 0; i < WIN; i++) { dcIR += irBuf[i]; dcRed += rdBuf[i]; }
  dcIR  /= WIN;
  dcRed /= WIN;
  if (dcIR == 0 || dcRed == 0) return 0;
  if (dcIR > 250000 || dcRed > 250000) return 0;  // ADC saturated

  float acIR = 0, acRed = 0;
  for (int i = 0; i < WIN; i++) {
    float di = (float)irBuf[i] - (float)dcIR;
    float dr = (float)rdBuf[i] - (float)dcRed;
    acIR  += di * di;
    acRed += dr * dr;
  }
  acIR  = sqrtf(acIR  / WIN);
  acRed = sqrtf(acRed / WIN);
  if (acIR == 0) return 0;

  float R = (acRed / (float)dcRed) / (acIR / (float)dcIR);
  float s = -45.060f * R * R + 30.354f * R + 94.845f;
  if (s < 80.0f)  s = 80.0f;
  if (s > 100.0f) s = 100.0f;
  return (uint8_t)s;
}

// ── BLE send ────────────────────────────────────────────────────
void bleSend(uint8_t hr, uint8_t spo2) {
  pHRChar->setValue(&hr, 1);
  pHRChar->notify();
  pSpO2Char->setValue(&spo2, 1);
  pSpO2Char->notify();
  lastSentHr   = hr;
  lastSentSpo2 = spo2;
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==============================");
  Serial.println("   Pulse Monitor — MAX30102");
  Serial.println("==============================");

  // ── BLE init ──────────────────────────────────────────────────
  Serial.print("[1] BLE init...     ");
  BLEDevice::init("PulseMonitor");
  Serial.println("OK");

  Serial.print("[2] Server...       ");
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  Serial.println("OK");

  Serial.print("[3] Service...      ");
  BLEService* svc = server->createService(SERVICE_UUID);

  pHRChar = svc->createCharacteristic(
    HR_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pHRChar->addDescriptor(new BLE2902());
  uint8_t z = 0;
  pHRChar->setValue(&z, 1);

  pSpO2Char = svc->createCharacteristic(
    SPO2_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pSpO2Char->addDescriptor(new BLE2902());
  pSpO2Char->setValue(&z, 1);

  svc->start();
  Serial.println("OK");

  Serial.print("[4] Advertising...  ");
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(false);
  adv->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  Serial.println("OK");

  // ── MAX30102 init — SDA=GPIO23, SCL=GPIO22 ────────────────────
  Serial.print("[5] MAX30102...     ");
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(200);

  // I2C scan for debugging
  byte i2cDevices = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) i2cDevices++;
  }

  // Try sensor init with retries
  for (int retry = 0; retry < 3 && !sensorFound; retry++) {
    if (retry > 0) delay(300);
    sensorFound = particleSensor.begin(Wire, I2C_SPEED_STANDARD);
  }

  if (sensorFound) {
    particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    lastBeat = millis();
    Serial.println("OK — LIVE SENSOR MODE");
  } else {
    Serial.print("NOT FOUND (");
    Serial.print(i2cDevices);
    Serial.println(" I2C devices on bus) — SIMULATION MODE");
    if (i2cDevices == 0) {
      Serial.println("  No I2C devices. Check wiring:");
      Serial.println("  VIN→3.3V  GND→GND  SDA→GPIO23  SCL→GPIO22");
    }
  }

  Serial.println("\nBLE advertising as \"PulseMonitor\"");
  Serial.println("Open Chrome → localhost:5173 → Connect");
  Serial.println("==============================");
}

void loop() {
  if (!clientConnected) {
    if (millis() - lastStatus > 5000) {
      lastStatus = millis();
      Serial.println("Waiting for BLE client...");
    }
    if (sensorFound) particleSensor.check();
    return;
  }

  // ═════════════════════════════════════════════════════════════
  //  REAL SENSOR MODE
  // ═════════════════════════════════════════════════════════════
  if (sensorFound) {
    particleSensor.check();

    while (particleSensor.available()) {
      uint32_t irVal  = particleSensor.getFIFOIR();
      uint32_t redVal = particleSensor.getFIFORed();
      particleSensor.nextSample();

      if (!fingerOn && irVal >= FINGER_ON) {
        fingerOn    = true;
        winIdx      = 0;
        winFull     = false;
        beatAvg     = 0;
        rateSpot    = 0;
        validRates  = 0;
        beatsPerMinute = 0;
        lastBeat    = millis();
        memset(rates, 0, sizeof(rates));
        Serial.println("[SENSOR] Finger ON — warming up...");
      } else if (fingerOn && irVal < FINGER_OFF) {
        fingerOn = false;
        Serial.println("[SENSOR] Finger OFF");
      }

      if (!fingerOn) {
        if (millis() - lastSend > 1500) {
          lastSend = millis();
          bleSend(0, 0);
        }
        continue;
      }

      // ── HR: peak detection ────────────────────────────────────
      if (checkForBeat(irVal)) {
        long delta = millis() - lastBeat;
        lastBeat = millis();
        beatsPerMinute = 60.0f / (delta / 1000.0f);

        if (beatsPerMinute > 30 && beatsPerMinute < 220) {
          rates[rateSpot] = (byte)beatsPerMinute;
          rateSpot = (rateSpot + 1) % RATE_SIZE;
          if (validRates < RATE_SIZE) validRates++;

          long sum = 0;
          for (byte i = 0; i < validRates; i++) sum += rates[i];
          beatAvg = sum / validRates;
        }
      }

      // ── SpO2: sliding window ──────────────────────────────────
      irBuf[winIdx] = irVal;
      rdBuf[winIdx] = redVal;
      winIdx = (winIdx + 1) % WIN;
      if (winIdx == 0) winFull = true;

      // ── Send every 1.5s ───────────────────────────────────────
      if (millis() - lastSend > 1500) {
        lastSend = millis();

        uint8_t hrToSend = 0;
        if (validRates >= 2 && beatAvg > 30) {
          hrToSend = (uint8_t)beatAvg;
        }
        if (hrToSend == 0 && fingerOn && lastSentHr > 0) {
          hrToSend = lastSentHr;
        }

        uint8_t spo2Val = winFull ? calcSpO2() : 0;
        if (spo2Val == 0 && fingerOn && lastSentSpo2 > 0) {
          spo2Val = lastSentSpo2;
        }

        Serial.print("[LIVE] HR: ");
        if (hrToSend > 0) { Serial.print(hrToSend); Serial.print(" BPM"); }
        else Serial.print("locking...");
        Serial.print(" | SpO2: ");
        if (spo2Val > 0) { Serial.print(spo2Val); Serial.print("%"); }
        else Serial.print("collecting...");
        Serial.print(" | IR: ");
        Serial.println(irVal);

        bleSend(hrToSend, spo2Val);
      }
    }

  // ═════════════════════════════════════════════════════════════
  //  SIMULATION MODE
  // ═════════════════════════════════════════════════════════════
  } else {
    if (millis() - lastSend > 1500) {
      lastSend = millis();

      simHr += random(-2, 3);
      if (simHr < 65) simHr = 65;
      if (simHr > 85) simHr = 85;
      simSpo2 = 96 + random(0, 4);

      Serial.print("[SIM] HR: ");
      Serial.print(simHr);
      Serial.print(" BPM | SpO2: ");
      Serial.print(simSpo2);
      Serial.println("%");

      bleSend((uint8_t)simHr, (uint8_t)simSpo2);
    }
  }
}
