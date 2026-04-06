import { useState, useCallback, useRef, useEffect } from 'react';

const PULSE_SERVICE_UUID = '4fafc201-1fb5-459e-8bcc-c5c9c331914b';
const HR_CHAR_UUID =       'beb5483e-36e1-4688-b7f5-ea07361b26a8';
const SPO2_CHAR_UUID =     '13210332-9cb7-4a00-b6a4-c7aa32fc8476';

export function useBluetooth() {
  const [isConnected, setIsConnected] = useState(false);
  const [isConnecting, setIsConnecting] = useState(false);
  const [error, setError] = useState(null);
  const [hr, setHr] = useState(0);
  const [spo2, setSpo2] = useState(0);

  const deviceRef = useRef(null);

  // Stable disconnect handler — defined once, used by both the button and the gatt event
  const onDisconnected = useCallback(() => {
    setIsConnected(false);
    setHr(0);
    setSpo2(0);
  }, []);

  // Clean up BLE connection if component unmounts while connected
  useEffect(() => {
    return () => {
      if (deviceRef.current?.gatt?.connected) {
        deviceRef.current.gatt.disconnect();
      }
    };
  }, []);

  const connect = useCallback(async () => {
    if (!navigator.bluetooth) {
      setError('Web Bluetooth is not available. Use Chrome or Edge.');
      return;
    }

    setIsConnecting(true);
    setError(null);

    try {
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: 'Pulse' }],
        optionalServices: [PULSE_SERVICE_UUID]
      });

      deviceRef.current = device;
      device.addEventListener('gattserverdisconnected', onDisconnected);

      const server  = await device.gatt.connect();
      const service = await server.getPrimaryService(PULSE_SERVICE_UUID);

      const hrChar = await service.getCharacteristic(HR_CHAR_UUID);
      await hrChar.startNotifications();
      hrChar.addEventListener('characteristicvaluechanged', (e) => {
        setHr(e.target.value.getUint8(0));
      });

      const spo2Char = await service.getCharacteristic(SPO2_CHAR_UUID);
      await spo2Char.startNotifications();
      spo2Char.addEventListener('characteristicvaluechanged', (e) => {
        setSpo2(e.target.value.getUint8(0));
      });

      setIsConnected(true);
    } catch (err) {
      console.error(err);
      setError(err.message || 'Failed to connect');
    } finally {
      setIsConnecting(false);
    }
  }, [onDisconnected]);

  const disconnect = useCallback(() => {
    if (deviceRef.current?.gatt?.connected) {
      deviceRef.current.gatt.disconnect();
    }
  }, []);

  return { hr, spo2, isConnected, isConnecting, error, connect, disconnect };
}
