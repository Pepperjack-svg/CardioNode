#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "MAX30105.h"
#include "heartRate.h"

MAX30105 particleSensor;

#define SERVICE_UUID   "4fafc201-1fb5-459e-8bcc-c5c9c331914b"
#define HR_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SPO2_CHAR_UUID "13210332-9cb7-4a00-b6a4-c7aa32fc8476"

BLEServer*         pServer   = NULL;
BLECharacteristic* pHRChar   = NULL;
BLECharacteristic* pSpO2Char = NULL;
volatile bool deviceConnected    = false;
volatile bool oldDeviceConnected = false;

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { deviceConnected = true;  Serial.println("BLE Connected."); }
  void onDisconnect(BLEServer*) { deviceConnected = false; Serial.println("BLE Disconnected."); }
};

// ── Finger detection ─────────────────────────────────────────
#define FINGER_ON  15000   // lowered for clone sensors
#define FINGER_OFF  8000

// ── Shared sensor state (written by sensorTask, read by loop) ─
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t  g_hr    = 0;
volatile uint8_t  g_spo2  = 0;
volatile bool     g_finger = false;
volatile bool     g_newData = false;   // set true each second

// ── Heart Rate ───────────────────────────────────────────────
#define RATE_SIZE 4
byte  rates[RATE_SIZE];
byte  rateSpot   = 0;
long  lastBeat   = 0;
int   beatAvg    = 0;
byte  validBeats = 0;

// ── SpO2 ─────────────────────────────────────────────────────
#define WIN 50
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
  portENTER_CRITICAL(&mux);
  g_hr = 0; g_spo2 = 0; g_finger = false; g_newData = true;
  portEXIT_CRITICAL(&mux);
}

uint8_t calcSpO2() {
  if (!winFull) return 0;
  float dcIR = 0, dcRed = 0;
  for (int i = 0; i < WIN; i++) { dcIR += irBuf[i]; dcRed += rdBuf[i]; }
  dcIR /= WIN; dcRed /= WIN;
  if (dcIR < 1000 || dcRed < 1000) return 0;

  float acIR = 0, acRed = 0;
  for (int i = 0; i < WIN; i++) {
    float di = (float)irBuf[i] - dcIR;
    float dr = (float)rdBuf[i] - dcRed;
    acIR  += di * di;
    acRed += dr * dr;
  }
  acIR  = sqrt(acIR  / WIN);
  acRed = sqrt(acRed / WIN);
  if (acIR < 10.0f || acRed < 10.0f) return 0;

  // Try both R orientations (clone sensors sometimes swap Red/IR)
  float R1 = (acRed / dcRed) / (acIR / dcIR);
  float R2 = (acIR  / dcIR)  / (acRed / dcRed);
  float R  = (R1 >= 0.4f && R1 <= 1.2f) ? R1 :
             (R2 >= 0.4f && R2 <= 1.2f) ? R2 : 0;
  if (R == 0) return 0;

  int s = (int)(-45.060f * R * R + 30.354f * R + 94.845f);
  if (s < 80 || s > 100) return 0;
  return (uint8_t)s;
}

// ── Sensor task — only reads sensor, never touches BLE ───────
void sensorTask(void* param) {
  bool     fingerOn   = false;
  unsigned long lastBLEMs = 0;

  for (;;) {
    particleSensor.check();
    int n = 0;
    while (particleSensor.available() && n < 8) {
      long ir  = particleSensor.getIR();
      long red = particleSensor.getRed();
      particleSensor.nextSample();
      n++;

      // Finger detection
      if (!fingerOn && ir > FINGER_ON) {
        fingerOn = true;
        Serial.print("Finger ON. IR="); Serial.println(ir);
      }
      if (fingerOn && ir < FINGER_OFF) {
        fingerOn = false;
        resetState();
        Serial.println("Finger OFF.");
      }
      if (!fingerOn) {
        // Print IR every 2s so user can see the raw value
        static unsigned long lastIRPrint = 0;
        if (millis() - lastIRPrint > 2000) {
          lastIRPrint = millis();
          Serial.print("IR (no finger): "); Serial.println(ir);
        }
        continue;
      }

      // Fill SpO2 window
      irBuf[winIdx] = (uint32_t)ir;
      rdBuf[winIdx] = (uint32_t)red;
      winIdx++;
      if (winIdx >= WIN) { winIdx = 0; winFull = true; }

      // Heart rate
      if (checkForBeat(ir)) {
        long now = millis(), delta = now - lastBeat;
        lastBeat = now;
        if (delta > 300 && delta < 2000) {
          int bpm = (int)(60000L / delta);
          Serial.print("Beat! BPM="); Serial.println(bpm);
          if (bpm >= 40 && bpm <= 200 && (beatAvg == 0 || abs(bpm - beatAvg) < 30)) {
            rates[rateSpot] = (byte)bpm;
            rateSpot = (rateSpot + 1) % RATE_SIZE;
            if (validBeats < RATE_SIZE) validBeats++;
            if (validBeats == RATE_SIZE) {
              int sum = 0;
              for (byte i = 0; i < RATE_SIZE; i++) sum += rates[i];
              beatAvg = sum / RATE_SIZE;
            }
          }
        }
      }
    }

    // Update shared globals every second — loop() will do the BLE send
    if (fingerOn && millis() - lastBLEMs >= 1000) {
      lastBLEMs = millis();
      uint8_t spo2 = calcSpO2();
      if (spo2 > 0) lastSpo2 = spo2;
      uint8_t hrOut = (validBeats == RATE_SIZE && beatAvg > 0) ? (uint8_t)beatAvg : 0;

      portENTER_CRITICAL(&mux);
      g_hr     = hrOut;
      g_spo2   = lastSpo2;
      g_finger = true;
      g_newData = true;
      portEXIT_CRITICAL(&mux);

      Serial.print("HR: "); Serial.print(hrOut);
      Serial.print("  SpO2: "); Serial.println(lastSpo2);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("Pulse Monitor booting...");

  Wire.begin(22, 21);  // SDA=22, SCL=21
  Wire.beginTransmission(0x57);
  bool found = (Wire.endTransmission() == 0);
  Serial.println(found ? "Sensor found at 0x57." : "Sensor not found at 0x57.");

  particleSensor.begin(Wire, I2C_SPEED_FAST);
  particleSensor.setup(0x7F, 1, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x7F);
  particleSensor.setPulseAmplitudeIR(0x7F);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("Sensor ready.");

  BLEDevice::init("PulseMonitor");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  BLEService* svc = pServer->createService(SERVICE_UUID);

  pHRChar = svc->createCharacteristic(HR_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pHRChar->addDescriptor(new BLE2902());

  pSpO2Char = svc->createCharacteristic(SPO2_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pSpO2Char->addDescriptor(new BLE2902());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(false);
  adv->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising...");

  // Sensor task — 20KB stack, no BLE calls inside
  xTaskCreate(sensorTask, "sensor", 20480, NULL, 1, NULL);
}

// ── Loop — handles BLE writes and reconnection ───────────────
void loop() {
  // Reconnection
  if (!deviceConnected && oldDeviceConnected) {
    delay(300);
    pServer->startAdvertising();
    oldDeviceConnected = false;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
  }

  // BLE send — only ever called from loop(), never from sensor task
  if (deviceConnected) {
    bool send = false;
    uint8_t hr, spo2;
    portENTER_CRITICAL(&mux);
    if (g_newData) {
      hr = g_hr; spo2 = g_spo2;
      g_newData = false;
      send = true;
    }
    portEXIT_CRITICAL(&mux);

    if (send) {
      uint8_t v;
      v = hr;
      pHRChar->setValue(&v, 1);
      pHRChar->notify();
      v = spo2;
      pSpO2Char->setValue(&v, 1);
      pSpO2Char->notify();
    }
  }

  delay(50);
}
