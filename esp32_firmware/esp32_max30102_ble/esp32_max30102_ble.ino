#include <Wire.h>
#include <NimBLEDevice.h>
#include "MAX30105.h"
#include "heartRate.h"

// ── Pin Configuration ─────────────────────────────────────────
// MAX30102 Pin │ ESP32 Pin
// ─────────────┼──────────
//         SDA  │  GPIO 23
//         SCL  │  GPIO 22
//         VCC  │  3.3V
//         GND  │  GND
#define I2C_SDA 23
#define I2C_SCL 22

MAX30105 particleSensor;

#define SERVICE_UUID   "4fafc201-1fb5-459e-8bcc-c5c9c331914b"
#define HR_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SPO2_CHAR_UUID "13210332-9cb7-4a00-b6a4-c7aa32fc8476"

NimBLEServer*         pServer   = NULL;
NimBLECharacteristic* pHRChar   = NULL;
NimBLECharacteristic* pSpO2Char = NULL;
volatile bool deviceConnected    = false;
volatile bool oldDeviceConnected = false;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*)    { deviceConnected = true;  Serial.println("BLE Connected."); }
  void onDisconnect(NimBLEServer*) { deviceConnected = false; Serial.println("BLE Disconnected."); }
};

// ── Shared state: sensorTask writes, bleTask reads ───────────
volatile uint8_t g_hr      = 0;
volatile uint8_t g_spo2    = 0;
volatile bool    g_newData = false;

// ── Finger ───────────────────────────────────────────────────
#define FINGER_ON   10000
#define FINGER_OFF   5000
#define SAT_LIMIT  250000

// ── Heart Rate ───────────────────────────────────────────────
#define RATE_SIZE 2
byte  rates[RATE_SIZE];
byte  rateSpot   = 0;
long  lastBeat   = 0;
int   beatAvg    = 0;
byte  validBeats = 0;

// ── SpO2 window ──────────────────────────────────────────────
#define WIN 60
uint32_t irBuf[WIN];
uint32_t rdBuf[WIN];
byte     winIdx  = 0;
bool     winFull = false;
uint8_t  lastSpo2 = 0;

void resetState() {
  beatAvg = 0; rateSpot = 0; validBeats = 0;
  lastBeat = 0; winIdx = 0; winFull = false; lastSpo2 = 0;
  memset(rates, 0, sizeof(rates));
  memset(irBuf,  0, sizeof(irBuf));
  memset(rdBuf,  0, sizeof(rdBuf));
  g_hr = 0; g_spo2 = 0; g_newData = true;
}

uint8_t calcSpO2() {
  if (!winFull) return 0;
  float dcIR = 0, dcRed = 0;
  for (int i = 0; i < WIN; i++) { dcIR += irBuf[i]; dcRed += rdBuf[i]; }
  dcIR /= WIN; dcRed /= WIN;
  if (dcIR < 3000 || dcRed < 3000) return 0;

  float acIR = 0, acRed = 0;
  for (int i = 0; i < WIN; i++) {
    float di = (float)irBuf[i] - dcIR;
    float dr = (float)rdBuf[i] - dcRed;
    acIR  += di * di;
    acRed += dr * dr;
  }
  acIR  = sqrt(acIR  / WIN);
  acRed = sqrt(acRed / WIN);
  if (acIR < 10.0f) return 0;

  float R = (acRed / dcRed) / (acIR / dcIR);
  if (R < 0.3f || R > 2.0f) R = (acIR / dcIR) / (acRed / dcRed);
  if (R < 0.3f || R > 2.0f) return 0;

  int s = (int)(-45.060f * R * R + 30.354f * R + 94.845f);
  if (s < 80 || s > 100) return 0;
  return (uint8_t)s;
}

// ── Sensor task — ZERO BLE calls ─────────────────────────────
void sensorTask(void* param) {
  bool fingerOn = false;
  unsigned long lastBLEMs   = 0;
  unsigned long lastIRPrint = 0;

  for (;;) {
    particleSensor.check();

    while (particleSensor.available()) {
      long ir  = particleSensor.getIR();
      long red = particleSensor.getRed();
      particleSensor.nextSample();

      if (ir >= SAT_LIMIT) continue;

      if (!fingerOn && ir > FINGER_ON) {
        fingerOn = true;
        Serial.print("Finger ON  IR="); Serial.println(ir);
      }
      if (fingerOn && ir < FINGER_OFF) {
        fingerOn = false;
        resetState();
        Serial.println("Finger OFF.");
      }

      if (!fingerOn) {
        if (millis() - lastIRPrint > 3000) {
          lastIRPrint = millis();
          Serial.print("IR="); Serial.println(ir);
        }
        continue;
      }

      irBuf[winIdx] = (uint32_t)ir;
      rdBuf[winIdx] = (uint32_t)red;
      winIdx++;
      if (winIdx >= WIN) { winIdx = 0; winFull = true; }

      if (checkForBeat(ir)) {
        long now = millis(), delta = now - lastBeat;
        lastBeat = now;
        if (delta > 300 && delta < 2000) {
          int bpm = (int)(60000L / delta);
          Serial.print("Beat "); Serial.print(bpm); Serial.println(" BPM");
          if (bpm >= 40 && bpm <= 200) {
            if (beatAvg == 0 || abs(bpm - beatAvg) < 40) {
              rates[rateSpot] = (byte)bpm;
              rateSpot = (rateSpot + 1) % RATE_SIZE;
              if (validBeats < RATE_SIZE) validBeats++;
              if (validBeats >= RATE_SIZE) {
                int sum = 0;
                for (byte i = 0; i < RATE_SIZE; i++) sum += rates[i];
                beatAvg = sum / RATE_SIZE;
              }
            }
          }
        }
      }
    }

    if (fingerOn && millis() - lastBLEMs >= 1000) {
      lastBLEMs = millis();
      uint8_t spo2 = calcSpO2();
      if (spo2 > 0) lastSpo2 = spo2;
      uint8_t hrOut = (validBeats >= RATE_SIZE && beatAvg > 0) ? (uint8_t)beatAvg : 0;

      Serial.print("HR: "); Serial.print(hrOut);
      Serial.print("  SpO2: "); Serial.println(lastSpo2);

      g_hr    = hrOut;
      g_spo2  = lastSpo2;
      g_newData = true;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── BLE task — all NimBLE calls live here ────────────────────
void bleTask(void* param) {
  for (;;) {
    if (!deviceConnected && oldDeviceConnected) {
      vTaskDelay(pdMS_TO_TICKS(300));
      pServer->startAdvertising();
      oldDeviceConnected = false;
      Serial.println("Re-advertising.");
    }
    if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = true;
    }

    if (deviceConnected && g_newData) {
      g_newData = false;
      uint8_t hr   = g_hr;
      uint8_t spo2 = g_spo2;

      uint8_t v = hr;
      pHRChar->setValue(&v, 1);
      pHRChar->notify();
      v = spo2;
      pSpO2Char->setValue(&v, 1);
      pSpO2Char->notify();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("Pulse Monitor booting...");

  Wire.end();
  delay(50);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);

  // Hardware reset MAX30102
  Wire.beginTransmission(0x57);
  Wire.write(0x09); Wire.write(0x40);
  Wire.endTransmission();
  delay(100);

  Wire.beginTransmission(0x57);
  Serial.println(Wire.endTransmission() == 0 ? "Sensor found." : "Sensor NOT found.");

  particleSensor.begin(Wire, I2C_SPEED_FAST);
  particleSensor.setup(0xFF, 1, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x7F);
  particleSensor.setPulseAmplitudeIR(0xFF);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("Sensor ready.");

  // NimBLE init — much lighter and more stable than Bluedroid
  NimBLEDevice::init("PulseMonitor");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);   // max TX power

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  NimBLEService* svc = pServer->createService(SERVICE_UUID);

  // NimBLE handles CCCD automatically — no BLE2902 descriptor needed
  pHRChar = svc->createCharacteristic(HR_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  pSpO2Char = svc->createCharacteristic(SPO2_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();
  Serial.println("BLE advertising...");

  xTaskCreatePinnedToCore(sensorTask, "sensor", 12288, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(bleTask,    "ble",     8192, NULL, 2, NULL, 1);
}

void loop() {
  delay(100);
}
