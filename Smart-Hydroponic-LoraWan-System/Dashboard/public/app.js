console.log('app.js loaded');
const socket = io();
socket.on('connect', () => console.log('socket connected'));

// ---------- helpers ----------
function setText(id, val, suffix = '') {
  const el = document.getElementById(id);
  if (el) el.textContent = (val === null || val === undefined || Number.isNaN(val)) ? '--' : `${val}${suffix}`;
}
function setPill(id, on) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = on ? 'ON' : 'OFF';
  el.className = `pill ${on ? 'pill-on' : 'pill-off'}`;
}

// ---------- Node 1 ----------
socket.on('node1Data', (d) => {
  console.log('node1Data', d);

  setText('soil',        d.soil_pct ?? d.soil);
  setText('gas',         d.gas_pct  ?? d.gas);
  const t = d.temp_c ?? d.temperature_c;
  setText('temperature', (t != null) ? Number(t).toFixed(1) : null);
  const p = d.pressure_hpa ?? d.pressure;
  setText('pressure',    (p != null) ? Number(p).toFixed(1) : null);
  const a = d.altitude_m ?? d.altitude;
  setText('altitude',    (a != null) ? Number(a).toFixed(1) : null);
  setText('lux',          d.lux);

  setText('rssi-value', d.rssi);
  setText('snr-value',  (d.snr != null) ? Number(d.snr).toFixed(1) : null);
  setText('dr-value',   d.dataRate);

  const buz = (d.buzzer === true || d.buzzer === 'ON' || d.buzzer === 1);
  setPill('buzzerStatus', buz);
});

// ---------- Node 2 ----------
socket.on('node2Data', (d) => {
  console.log('node2Data', d);

  setText('tds', d.tds_ppm);

  setText('rssi-value-2', d.rssi);
  setText('snr-value-2',  (d.snr != null) ? Number(d.snr).toFixed(1) : null);
  setText('dr-value-2',   d.dataRate);
});

// Backward compatibility (if any code still emits sensorData)
socket.on('sensorData', (d) => {
  console.log('sensorData(fallback)', d);
});

// ---------- Buzzer buttons ----------
document.getElementById('toggleBuzzerBtn')?.addEventListener('click', () => {
  socket.emit('buzzerCommand', { cmd: 'toggle' });
});
