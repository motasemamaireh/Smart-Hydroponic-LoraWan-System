require('dotenv').config();
const path = require('path');
const express = require('express');
const http = require('http');
const mqtt = require('mqtt');
const { Server } = require('socket.io');

const APP_ID  = process.env.TTN_APP_ID;
const REGION  = process.env.TTN_REGION || 'eu1';
const API_KEY = process.env.TTN_API_KEY;
const PORT    = process.env.PORT || 8080;

const NODE1_ID = process.env.NODE1_ID || 'node1';
const NODE2_ID = process.env.NODE2_ID || 'node2';

if (!APP_ID || !API_KEY) {
  console.error('❌ Missing TTN_APP_ID or TTN_API_KEY in .env');
  process.exit(1);
}

// -------------------- Express + Socket.IO --------------------
const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: '*' } });

// Serve /public
const PUBLIC_DIR = path.join(__dirname, 'public');
app.use(express.static(PUBLIC_DIR));
app.get('/', (_req, res) => res.sendFile(path.join(PUBLIC_DIR, 'index.html')));

// -------------------- MQTT (The Things Stack) --------------------
const MQTT_HOST = `${REGION}.cloud.thethings.network`;
const UPLINK_TOPIC    = `v3/${APP_ID}@ttn/devices/+/up`;
const DOWNLINK_TOPIC1 = `v3/${APP_ID}@ttn/devices/${NODE1_ID}/down/push`; // buzzer on node1

const mqttClient = mqtt.connect({
  host: MQTT_HOST,
  port: 8883,
  protocol: 'mqtts',
  username: `${APP_ID}@ttn`,
  password: API_KEY,
  keepalive: 60,
  reconnectPeriod: 3000,
});

mqttClient.on('connect', () => {
  console.log(`✅ MQTT connected: ${MQTT_HOST}`);
  mqttClient.subscribe(UPLINK_TOPIC, (err) => {
    if (err) console.error('❌ Subscribe error:', err);
    else console.log(`🔔 Subscribed: ${UPLINK_TOPIC}`);
  });
});

mqttClient.on('error', (err) => {
  console.error('❌ MQTT error:', err.message);
});

// Build radio/DR fields
function enrichRadio(uplink) {
  const meta = uplink?.rx_metadata?.[0];
  const dr   = uplink?.settings?.data_rate?.lora;
  const rssi = meta?.rssi ?? null;
  const snr  = meta?.snr ?? null;
  const sf   = dr?.spreading_factor ?? null;
  const bwkhz = dr?.bandwidth ? dr.bandwidth / 1000 : null;
  const dataRate = (sf && bwkhz) ? `SF${sf}BW${bwkhz}` : null;
  return { rssi, snr, dataRate };
}

// Keep a snapshot for new clients
let lastNode1 = null;
let lastNode2 = null;

mqttClient.on('message', (_topic, buf) => {
  try {
    const msg     = JSON.parse(buf.toString());
    const devId   = msg?.end_device_ids?.device_id;
    const uplink  = msg?.uplink_message;
    const decoded = uplink?.decoded_payload;
    if (!devId || !decoded) return;

    const radio   = enrichRadio(uplink);
    const packet  = { devId, ...decoded, ...radio, ts: Date.now() };

    // Route by explicit ID first
    if (devId === NODE1_ID) {
      lastNode1 = packet;
      io.emit('node1Data', packet);
      io.emit('sensorData', packet); // backward compatibility
      console.log(`📡 [${devId}→node1]`, packet);
      return;
    }
    if (devId === NODE2_ID) {
      lastNode2 = packet;
      io.emit('node2Data', packet);
      io.emit('sensorData', packet); // backward compatibility
      console.log(`📡 [${devId}→node2]`, packet);
      return;
    }

    // Fallback: detect by payload shape
    if (decoded.tds_ppm !== undefined) {
      lastNode2 = packet;
      io.emit('node2Data', packet);
      console.log(`📡 [${devId} auto→node2]`, packet);
    } else if (
      decoded.soil_pct !== undefined ||
      decoded.temp_c   !== undefined ||
      decoded.lux      !== undefined
    ) {
      lastNode1 = packet;
      io.emit('node1Data', packet);
      console.log(`📡 [${devId} auto→node1]`, packet);
    } else {
      io.emit('unknownNode', packet);
      console.log(`📡 [${devId} unknown]`, packet);
    }

  } catch (e) {
    console.error('❌ Parse error:', e);
  }
});

// -------------------- Downlink: Buzzer on Node1 --------------------
function publishDownlinkToNode1(byte, fport = 2) {
  // TTS v3 downlink structure
  const payload = {
    downlinks: [{
      f_port: fport,
      frm_payload: Buffer.from([byte]).toString('base64'),
      confirmed: false,
      priority: 'NORMAL'
    }]
  };
  mqttClient.publish(DOWNLINK_TOPIC1, JSON.stringify(payload), { qos: 0 }, (err) => {
    if (err) console.error('❌ Downlink publish error:', err);
    else     console.log(`⬇️  Downlink to ${NODE1_ID} (FPort ${fport}) byte=0x${byte.toString(16)}`);
  });
}

// Socket.IO command from the browser
io.on('connection', (socket) => {
  // Send snapshots on connect
  if (lastNode1) socket.emit('node1Data', lastNode1);
  if (lastNode2) socket.emit('node2Data', lastNode2);

  socket.on('buzzerCommand', ({ cmd }) => {
    const byte = (cmd === 'on') ? 0x01 : (cmd === 'off') ? 0x00 : 0x02; // default toggle
    publishDownlinkToNode1(byte, 2);
  });
});

// -------------------- Start Server --------------------
server.listen(PORT, () => {
  console.log(`🌐 Dashboard: http://localhost:${PORT}`);
  console.log(`   Region=${REGION}  AppID=${APP_ID}`);
});
