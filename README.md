# Pulse Dashboard

A real-time heart rate and SpO2 monitoring system using an ESP32, MAX30102 sensor, and a React web dashboard connected via Bluetooth Low Energy (BLE).

## Overview

```
MAX30102 Sensor ──I2C──► ESP32 ──BLE──► Chrome Browser ──► React Dashboard
```

The system has two components:
1. **ESP32 Firmware** — Reads heart rate and SpO2 from a MAX30102 sensor and broadcasts via BLE. Falls back to simulated data if the sensor is not connected.
2. **Web Dashboard** — A React (Vite) frontend that connects to the ESP32 via Web Bluetooth and displays live readings.

## Hardware

### Requirements
- ESP32 Dev Module
- MAX30102 Pulse Oximeter Sensor (optional — firmware simulates data without it)
- 4× Jumper wires
- USB cable (data-capable, not charge-only)

### Pin Connections

```
MAX30102          ESP32 Dev Module
┌──────────┐      ┌──────────────┐
│ VIN  ────┼──────┤► 3.3V        │
│ GND  ────┼──────┤► GND         │
│ SDA  ────┼──────┤► GPIO 23     │
│ SCL  ────┼──────┤► GPIO 22     │
└──────────┘      └──────────────┘
```

| MAX30102 | ESP32 | Notes |
| :--- | :--- | :--- |
| **VIN** | **3.3V** | Power supply |
| **GND** | **GND** | Common ground |
| **SDA** | **GPIO 23** | I2C data |
| **SCL** | **GPIO 22** | I2C clock |

## ESP32 Firmware

Located in `esp32_firmware/esp32_max30102_ble/esp32_max30102_ble.ino`.

### Features
- Real-time HR via SparkFun peak detection algorithm
- SpO2 estimation using RED/IR ratio method
- Auto-fallback to simulation mode if sensor not detected
- Brownout protection for USB-powered operation
- Auto-reconnect after BLE disconnection

### Arduino IDE Setup
1. **Board**: `ESP32 Dev Module`
2. **Partition Scheme**: `Huge APP (3MB No OTA / 1MB SPIFFS)`
3. **Baud Rate**: `115200`

### Dependencies
Install via Arduino IDE Library Manager:
- `SparkFun MAX3010x Pulse and Proximity Sensor Library`

> The BLE library (`BLEDevice.h`) is built into the ESP32 Arduino core — no separate install needed.

### BLE Specification
| Property | Value |
| :--- | :--- |
| **Device Name** | `PulseMonitor` |
| **Service UUID** | `4fafc201-1fb5-459e-8bcc-c5c9c331914b` |
| **HR Characteristic** | `beb5483e-36e1-4688-b7f5-ea07361b26a8` (Read/Notify, uint8) |
| **SpO2 Characteristic** | `13210332-9cb7-4a00-b6a4-c7aa32fc8476` (Read/Notify, uint8) |

### Upload & Verify
1. Connect ESP32 via USB
2. Select board and port in Arduino IDE
3. Click **Upload**
4. Open Serial Monitor at **115200 baud**
5. Expected output:
   ```
   [1] BLE init...     OK
   [2] Server...       OK
   [3] Service...      OK
   [4] Advertising...  OK
   [5] MAX30102...     OK — LIVE SENSOR MODE
   ```

## Web Dashboard

Built with React 19 + Vite 8. Connects to the ESP32 via Web Bluetooth API.

### Setup
```bash
npm install
npm run dev
```

### Usage
1. Open `http://localhost:5173` in **Chrome** or **Edge**
2. Click **Connect** → select **PulseMonitor**
3. Place finger on sensor — HR and SpO2 update live

> **Note**: Web Bluetooth requires `localhost` or `HTTPS`. It does not work on iOS or Firefox.

### Tech Stack
- React 19 + Vite 8
- Recharts (charting)
- Lucide React (icons)
- Web Bluetooth API
