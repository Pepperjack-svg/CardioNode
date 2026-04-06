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

BLEServer*         pServer             = NULL;
BLECharacteristic* pHRCharacteristic   = NULL;
BLECharacteristic* pSpO2Characteristic = NULL;
volatile bool deviceConnected    = false;
volatile bool oldDeviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { deviceConnected = true;  Serial.println("Connected."); }
  void onDisconnect(BLEServer*) { deviceConnected = false; Serial.println("Disconnected."); }
};

// ── Safe BLE send ────────────────────────────────────────────
void bleSend(BLECharacteristic* c, uint8_t val) {
  if (!deviceConnected || c == NULL) return;
  c->setValue(&val, 1);
  c->notify();
}

// ── Finger detection ─────────────────────────────────────────
#define FINGER_ON  25000
#define FINGER_OFF 10000
bool fingerOn = false;

// ── Heart Rate (SparkFun checkForBeat) ───────────────────────
#define RATE_SIZE 4
byte  rates[RATE_SIZE];
byte  rateSpot   = 0;
long  lastBeat   = 0;
int   beatAvg    = 0;
byte  validBeats = 0;

// ── SpO2 window buffers ──────────────────────────────────────
#define WIN 100
uint32_t irBuf[WIN];
uint32_t rdBuf[WIN];
byte     winIdx  = 0;
bool     winFull = false;

uint8_t  lastSpo2  = 0;
unsigned long lastBLEMs = 0;

void resetState() {
  beatAvg    = 0;
  rateSpot   = 0;
  validBeats = 0;
  lastBeat   = 0;
  winIdx     = 0;
  winFull    = false;
  lastSpo2   = 0;
  memset(rates, 0, sizeof(rates));
  memset(irBuf,  0, sizeof(irBuf));
  memset(rdBuf,  0, sizeof(rdBuf));
}

// ── SpO2: window-mean R-ratio, both directions, pick valid ──
// Returns 0 if signal quality is insufficient.
uint8_t calcSpO2() {
  if (!winFull) return 0;

  // DC = window mean
  float dcIR = 0, dcRed = 0;
  for (int i = 0; i < WIN; i++) { dcIR += irBuf[i]; dcRed += rdBuf[i]; }
  dcIR /= WIN; dcRed /= WIN;
  if (dcIR < 1000 || dcRed < 1000) return 0;

  // AC = RMS of (sample − mean)
  float acIR = 0, acRed = 0;
  for (int i = 0; i < WIN; i++) {
    float di = (float)irBuf[i]  - dcIR;
    float dr = (float)rdBuf[i]  - dcRed;
    acIR  += di * di;
    acRed += dr * dr;
  }
  acIR  = sqrt(acIR  / WIN);
  acRed = sqrt(acRed / WIN);

  // Both AC components must be strong enough
  if (acIR < 20.0f || acRed < 20.0f) return 0;

  // Try both R directions, use whichever lands in physiological range
  float R1 = (acRed / dcRed) / (acIR / dcIR);   // normal
  float R2 = (acIR  / dcIR)  / (acRed / dcRed); // swapped (clone fix)

  float R = 0;
  if      (R1 >= 0.50f && R1 <= 1.00f) R = R1;
  else if (R2 >= 0.50f && R2 <= 1.00f) R = R2;
  else return 0; // both out of range → unreliable

  // Maxim empirical quadratic (AN6142)
  int spo2 = (int)(-45.060f * R * R + 30.354f * R + 94.845f);
  if (spo2 < 80 || spo2 > 100) return 0;
  return (uint8_t)spo2;
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("Pulse Monitor booting...");

  Wire.begin(22, 21); // SDA=GPIO22, SCL=GPIO21
  Wire.beginTransmission(0x57);
  Serial.println(Wire.endTransmission() == 0
    ? "Sensor found at 0x57." : "Sensor not found.");

  particleSensor.begin(Wire, I2C_SPEED_FAST);
  // 0x7F ~25mA — good signal without ADC saturation
  particleSensor.setup(0x7F, 1, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x7F);
  particleSensor.setPulseAmplitudeIR(0x7F);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("Sensor ready.");

  BLEDevice::init("PulseMonitor");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService* svc = pServer->createService(SERVICE_UUID);

  pHRCharacteristic = svc->createCharacteristic(HR_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pHRCharacteristic->addDescriptor(new BLE2902());

  pSpO2Characteristic = svc->createCharacteristic(SPO2_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pSpO2Characteristic->addDescriptor(new BLE2902());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(false);
  adv->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising...");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  if (!deviceConnected && oldDeviceConnected) {
    delay(300);
    pServer->startAdvertising();
    oldDeviceConnected = false;
    fingerOn = false;
    resetState();
    return;
  }
  if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = true;
  if (!deviceConnected) return;

  particleSensor.check();
  int n = 0;
  while (particleSensor.available() && n < 10) {
    long ir  = particleSensor.getIR();
    long red = particleSensor.getRed();
    particleSensor.nextSample();
    n++;

    // Finger hysteresis
    if (!fingerOn && ir > FINGER_ON)  { fingerOn = true;  Serial.println("Finger ON.");  }
    if ( fingerOn && ir < FINGER_OFF) {
      fingerOn = false;
      resetState();
      bleSend(pHRCharacteristic,   0);
      bleSend(pSpO2Characteristic, 0);
      Serial.println("Finger OFF.");
    }
    if (!fingerOn) continue;

    // Fill SpO2 buffer
    irBuf[winIdx] = (uint32_t)ir;
    rdBuf[winIdx] = (uint32_t)red;
    winIdx++;
    if (winIdx >= WIN) { winIdx = 0; winFull = true; }

    // Heart rate via SparkFun peak detector
    if (checkForBeat(ir)) {
      long now   = millis();
      long delta = now - lastBeat;
      lastBeat   = now;

      if (delta > 333 && delta < 1500) {
        int bpm = (int)(60000L / delta);
        if (bpm >= 40 && bpm <= 180) {
          if (beatAvg == 0 || abs(bpm - beatAvg) < 25) {
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
  }

  // Push BLE every 1 second — only from main loop, never from inside FIFO drain
  if (deviceConnected && fingerOn && millis() - lastBLEMs >= 1000) {
    lastBLEMs = millis();

    if (validBeats == RATE_SIZE && beatAvg > 0) {
      bleSend(pHRCharacteristic, (uint8_t)beatAvg);
      Serial.print("HR: "); Serial.print(beatAvg);
    }

    uint8_t spo2 = calcSpO2();
    if (spo2 > 0) {
      lastSpo2 = spo2;
      bleSend(pSpO2Characteristic, spo2);
      Serial.print("  SpO2: "); Serial.println(spo2);
    } else {
      Serial.println("  SpO2: ---");
    }
  }
}
