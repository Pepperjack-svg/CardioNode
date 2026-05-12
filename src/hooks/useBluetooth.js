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

  const deviceRef    = useRef(null);
  const hrCharRef    = useRef(null);
  const spo2CharRef  = useRef(null);
  const hrListenerRef   = useRef(null);
  const spo2ListenerRef = useRef(null);

  const onDisconnected = useCallback(() => {
    setIsConnected(false);
    setHr(0);
    setSpo2(0);
  }, []);

  // Remove notification listeners and stop notifications — safe to call any time
  const cleanupCharacteristics = useCallback(async () => {
    if (hrCharRef.current && hrListenerRef.current) {
      hrCharRef.current.removeEventListener('characteristicvaluechanged', hrListenerRef.current);
      try { await hrCharRef.current.stopNotifications(); } catch { /* ignore if already disconnected */ }
      hrCharRef.current = null;
      hrListenerRef.current = null;
    }
    if (spo2CharRef.current && spo2ListenerRef.current) {
      spo2CharRef.current.removeEventListener('characteristicvaluechanged', spo2ListenerRef.current);
      try { await spo2CharRef.current.stopNotifications(); } catch { /* ignore if already disconnected */ }
      spo2CharRef.current = null;
      spo2ListenerRef.current = null;
    }
  }, []);

  // Full cleanup on unmount
  useEffect(() => {
    return () => {
      cleanupCharacteristics();
      if (deviceRef.current?.gatt?.connected) {
        deviceRef.current.gatt.disconnect();
      }
    };
  }, [cleanupCharacteristics]);

  const connect = useCallback(async () => {
    const isLocalhost = ['localhost', '127.0.0.1', '::1'].includes(location.hostname);
    if (!window.isSecureContext && !isLocalhost) {
      setError('HTTPS required for Bluetooth. Access via https:// not http://.');
      return;
    }
    if (/ipad|iphone|ipod/i.test(navigator.userAgent)) {
      setError('Web Bluetooth is not supported on iOS. Use Android Chrome.');
      return;
    }
    if (!navigator.bluetooth) {
      setError('Web Bluetooth not available. Use Chrome or Edge on Android/Desktop.');
      return;
    }

    setIsConnecting(true);
    setError(null);

    try {
      console.log('[BLE] Requesting device...');
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [PULSE_SERVICE_UUID] }]
      });
      console.log('[BLE] Device selected:', device.name);

      // Remove stale listeners from a previous connect before re-registering
      if (deviceRef.current) {
        deviceRef.current.removeEventListener('gattserverdisconnected', onDisconnected);
      }
      await cleanupCharacteristics();

      deviceRef.current = device;
      device.addEventListener('gattserverdisconnected', onDisconnected);

      console.log('[BLE] Connecting to GATT server...');
      const server  = await device.gatt.connect();
      console.log('[BLE] GATT connected. Getting service...');
      const service = await server.getPrimaryService(PULSE_SERVICE_UUID);
      console.log('[BLE] Service found. Getting characteristics...');

      const hrChar = await service.getCharacteristic(HR_CHAR_UUID);
      console.log('[BLE] HR characteristic found.');
      const hrListener = (e) => {
        const val = e.target.value.getUint8(0);
        console.log('[BLE] HR notification:', val);
        if (val === 0 || (val >= 30 && val <= 250)) setHr(val);
      };
      hrChar.addEventListener('characteristicvaluechanged', hrListener);
      await hrChar.startNotifications();
      console.log('[BLE] HR notifications started.');
      hrCharRef.current = hrChar;
      hrListenerRef.current = hrListener;

      const spo2Char = await service.getCharacteristic(SPO2_CHAR_UUID);
      console.log('[BLE] SpO2 characteristic found.');
      const spo2Listener = (e) => {
        const val = e.target.value.getUint8(0);
        console.log('[BLE] SpO2 notification:', val);
        if (val === 0 || (val >= 80 && val <= 100)) setSpo2(val);
      };
      spo2Char.addEventListener('characteristicvaluechanged', spo2Listener);
      await spo2Char.startNotifications();
      console.log('[BLE] SpO2 notifications started.');
      spo2CharRef.current = spo2Char;
      spo2ListenerRef.current = spo2Listener;

      setIsConnected(true);
      console.log('[BLE] ✅ Fully connected and listening!');
    } catch (err) {
      await cleanupCharacteristics();
      console.error('[BLE] ❌ Connection failed:', err);
      setError(err.message || 'Failed to connect');
    } finally {
      setIsConnecting(false);
    }
  }, [onDisconnected, cleanupCharacteristics]);

  const disconnect = useCallback(async () => {
    await cleanupCharacteristics();
    if (deviceRef.current?.gatt?.connected) {
      deviceRef.current.gatt.disconnect();
    }
  }, [cleanupCharacteristics]);

  const clearError = useCallback(() => setError(null), []);

  return { hr, spo2, isConnected, isConnecting, error, clearError, connect, disconnect };
}
