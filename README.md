# Pulse Dashboard

A comprehensive system for real-time heart rate and SpO2 monitoring using an ESP32, MAX30102 sensor, and a React-based web dashboard.

## Overview

The Pulse Dashboard consists of two main components:
1. **ESP32 Firmware:** Reads biometric data from a MAX30102 sensor and broadcasts it over Bluetooth Low Energy (BLE).
2. **Web Dashboard:** A React (Vite) frontend application that connects to the ESP32 via Web Bluetooth to display real-time pulse and blood oxygen saturation (SpO2) readings.

## Hardware Requirements

- ESP32 Microcontroller
- MAX30102 (or MAX30105) Pulse Oximeter and Heart-Rate Sensor
- Jumper wires

### Pin Connections

| MAX30102 Pin | ESP32 Pin |
| :--- | :--- |
| **SDA** | 23 |
| **SCL** | 22 |
| **VCC** | 3.3V (or 5V depending on your board) |
| **GND** | GND |

## ESP32 Firmware

The firmware (`esp32_firmware/esp32_max30102_ble/esp32_max30102_ble.ino`) utilizes FreeRTOS tasks to separate sensor reading and BLE communication, ensuring smooth performance without blocking delays. It leverages the lightweight and highly stable `NimBLE` library for Bluetooth communication.

### BLE Details
- **Device Name:** `PulseMonitor`
- **Service UUID:** `4fafc201-1fb5-459e-8bcc-c5c9c331914b`
- **Heart Rate Characteristic UUID:** `beb5483e-36e1-4688-b7f5-ea07361b26a8` (Read/Notify)
- **SpO2 Characteristic UUID:** `13210332-9cb7-4a00-b6a4-c7aa32fc8476` (Read/Notify)

### Dependencies
Install the following libraries in the Arduino IDE to compile the firmware:
- `NimBLE-Arduino`
- `SparkFun MAX3010x Pulse and Proximity Sensor Library` (Provides `MAX30105.h` and `heartRate.h`)

## Web Dashboard

The web dashboard is built using React and Vite. It connects directly to the ESP32 from a compatible browser (like Chrome or Edge) using the Web Bluetooth API.

### Setup and Development

1. Install dependencies:
   ```bash
   npm install
   ```
2. Start the development server:
   ```bash
   npm run dev
   ```
