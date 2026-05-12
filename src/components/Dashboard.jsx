import React from 'react';
import { useBluetooth } from '../hooks/useBluetooth';
import { Heart, Bluetooth, BluetoothOff, Droplets, X } from 'lucide-react';

export default function Dashboard() {
  const { hr, spo2, isConnected, isConnecting, error, clearError, connect, disconnect } = useBluetooth();

  return (
    <div className="app-container">
      <header className="header">
        <div className="title-container">
          <h1 className="title">Pulse Live Monitor</h1>
          <span className="subtitle">Real-time telemetry interface</span>
        </div>
        <div className="header-actions">
          {!isConnected ? (
            <>
              <div className="status-badge">
                <div className="status-dot"></div>
                Offline
              </div>
              <button className="btn" onClick={connect} disabled={isConnecting}>
                <Bluetooth size={18} />
                {isConnecting ? 'Scanning' : 'Connect'}
              </button>
            </>
          ) : (
            <>
              <div className="status-badge">
                <div className="status-dot connected"></div>
                Connected
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                <BluetoothOff size={18} />
                Disconnect
              </button>
            </>
          )}
        </div>
      </header>

      <main>
        <div className="metrics-grid">
          <div className="card heart-rate-card">
            <div className="metric-header">
              Heart Rate
              <Heart
                size={24}
                className={isConnected && hr > 0 ? 'beating' : ''}
                color={isConnected ? 'var(--primary)' : 'var(--muted-foreground)'}
              />
            </div>
            <div className="metric-value-container">
              <div className="metric-value">{hr || '--'}</div>
              <div className="metric-unit">BPM</div>
            </div>
            {isConnected && hr === 0 && (
              <div className="waiting-label">Place finger on sensor</div>
            )}
          </div>

          <div className="card spo2-card">
            <div className="metric-header">
              Blood Oxygen
              <Droplets
                size={24}
                color={isConnected ? 'var(--chart-3)' : 'var(--muted-foreground)'}
              />
            </div>
            <div className="metric-value-container">
              <div className="metric-value">{spo2 || '--'}</div>
              <div className="metric-unit">%</div>
            </div>
            {isConnected && spo2 === 0 && (
              <div className="waiting-label">Place finger on sensor</div>
            )}
          </div>
        </div>

        {error && (
          <div className="error-message">
            <span>{error}</span>
            <button className="error-close" onClick={clearError} aria-label="Dismiss error">
              <X size={16} />
            </button>
          </div>
        )}
      </main>
    </div>
  );
}
