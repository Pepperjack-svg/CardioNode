// ── Pulse Monitor BLE Firmware ─────────────────────────────────
// Simulates HR & SpO2 data over BLE for the web dashboard.
// No MAX30102 sensor needed — pure BLE simulation.
// Board: ESP32 Dev Module
// Serial Monitor: 115200 baud

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// BLE UUIDs — must match useBluetooth.js
#define SERVICE_UUID   "4fafc201-1fb5-459e-8bcc-c5c9c331914b"
#define HR_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SPO2_CHAR_UUID "13210332-9cb7-4a00-b6a4-c7aa32fc8476"

BLECharacteristic* pHRChar   = NULL;
BLECharacteristic* pSpO2Char = NULL;

bool clientConnected = false;

// Simulation state
int simHr   = 72;
int simSpo2 = 97;
unsigned long lastSend = 0;

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) {
    clientConnected = true;
    Serial.println(">>> Client CONNECTED");
  }
  void onDisconnect(BLEServer*) {
    clientConnected = false;
    Serial.println(">>> Client DISCONNECTED");
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Re-advertising...");
  }
};

void setup() {
  // Disable brownout detector — required for USB-powered ESP32
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);  // Let power stabilize
  Serial.println("\n==============================");
  Serial.println("   Pulse Monitor BLE");
  Serial.println("==============================");

  // ── BLE Setup (same pattern as your working test) ──
  Serial.print("[1] BLE init...     ");
  BLEDevice::init("PulseMonitor");
  Serial.println("OK");

  Serial.print("[2] Server...       ");
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  Serial.println("OK");

  Serial.print("[3] Service...      ");
  BLEService* svc = server->createService(SERVICE_UUID);

  // Heart Rate characteristic
  pHRChar = svc->createCharacteristic(
    HR_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pHRChar->addDescriptor(new BLE2902());
  uint8_t initHr = 0;
  pHRChar->setValue(&initHr, 1);

  // SpO2 characteristic
  pSpO2Char = svc->createCharacteristic(
    SPO2_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pSpO2Char->addDescriptor(new BLE2902());
  uint8_t initSpo2 = 0;
  pSpO2Char->setValue(&initSpo2, 1);

  svc->start();
  Serial.println("OK");

  Serial.print("[4] Advertising...  ");
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(false);
  adv->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  Serial.println("OK");

  Serial.println("\nBLE advertising as \"PulseMonitor\"");
  Serial.println("Open Chrome → localhost:5173 → Connect");
  Serial.println("==============================");
}

void loop() {
  if (clientConnected) {
    // Send simulated data every 1.5 seconds
    if (millis() - lastSend > 1500) {
      lastSend = millis();

      // Simulate natural HR variation (65–85 BPM)
      simHr += random(-2, 3);
      if (simHr < 65) simHr = 65;
      if (simHr > 85) simHr = 85;

      // Simulate SpO2 (96–99%)
      simSpo2 = 96 + random(0, 4);

      // Notify HR
      uint8_t hrVal = (uint8_t)simHr;
      pHRChar->setValue(&hrVal, 1);
      pHRChar->notify();

      // Notify SpO2
      uint8_t spo2Val = (uint8_t)simSpo2;
      pSpO2Char->setValue(&spo2Val, 1);
      pSpO2Char->notify();

      Serial.print("[SIM] HR: ");
      Serial.print(simHr);
      Serial.print(" BPM | SpO2: ");
      Serial.print(simSpo2);
      Serial.println("%");
    }
  } else {
    // Status heartbeat while waiting
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 5000) {
      lastStatus = millis();
      Serial.println("Waiting for client...");
    }
  }
}
