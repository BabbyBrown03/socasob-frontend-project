/*
  TCP Server penerima video stream dari ESP32-CAM (plain socket, length-prefix)
  & Socket.IO server untuk real-time update ke frontend.
  
  Format tiap frame yang dikirim ESP32: [4 byte big-endian panjang][data JPEG]
*/

const net = require('net');
const express = require('express');
const { createServer } = require('http');
const { Server } = require('socket.io');

const TCP_PORT = 3003;  // ESP32-CAM connects here
const HTTP_PORT = 3001; // Next.js frontend connects here

let latestFrame = null;
let frameCount = 0;
let lastFpsLogTime = Date.now();

let activeEspSocket = null; // Store reference to the active ESP32-CAM connection
let currentEyeStatus = 'normal'; // 'normal', 'risk_myopia', 'risk_fatigue'
let currentEyeDistance = 'Dekat'; // 'Dekat', 'Jauh'

// ---------------------------------------------------------------------------
// TCP Server: menerima frame dari ESP32-CAM & mengirim status kembali
// ---------------------------------------------------------------------------
const tcpServer = net.createServer((socket) => {
  console.log(`[TCP] ESP32-CAM terhubung dari ${socket.remoteAddress}:${socket.remotePort}`);
  socket.setNoDelay(true);
  activeEspSocket = socket;

  // Send initial normal state upon connection
  socket.write('N');

  let buffer = Buffer.alloc(0);
  let expectedLen = null;

  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);

    while (true) {
      if (expectedLen === null) {
        if (buffer.length < 4) break;
        expectedLen = buffer.readUInt32BE(0);
        buffer = buffer.subarray(4);
      }

      if (buffer.length < expectedLen) break;

      const frame = buffer.subarray(0, expectedLen);
      buffer = buffer.subarray(expectedLen);
      expectedLen = null;

      latestFrame = Buffer.from(frame);
      frameCount++;

      const now = Date.now();
      if (now - lastFpsLogTime >= 1000) {
        console.log(`[FPS] ${frameCount} frame/detik, ukuran: ${latestFrame.length} bytes`);
        frameCount = 0;
        lastFpsLogTime = now;
      }
    }
  });

  socket.on('close', () => {
    console.log('[TCP] ESP32-CAM terputus');
    if (activeEspSocket === socket) {
      activeEspSocket = null;
    }
  });

  socket.on('error', (err) => {
    console.error('[TCP] Socket error:', err.message);
    if (activeEspSocket === socket) {
      activeEspSocket = null;
    }
  });
});

tcpServer.listen(TCP_PORT, '0.0.0.0', () => {
  console.log(`[TCP] Server TCP (ESP32-CAM) mendengarkan di 0.0.0.0:${TCP_PORT}`);
});

// ---------------------------------------------------------------------------
// HTTP & Socket.IO Server: Komunikasi dengan Next.js Frontend
// ---------------------------------------------------------------------------
const app = express();
const httpServer = createServer(app);
const io = new Server(httpServer, {
  cors: {
    origin: '*',
    methods: ['GET', 'POST']
  }
});

// Allow CORS for Express HTTP endpoints
app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Headers', 'Origin, X-Requested-With, Content-Type, Accept');
  next();
});

// API endpoint to manually trigger states for testing
app.get('/api/status/:state', (req, res) => {
  const state = req.params.state.toLowerCase();
  let cmd = 'N';
  let status = 'normal';

  if (state === 'fatigue' || state === 'lelah' || state === 'risk_fatigue') {
    cmd = 'F';
    status = 'risk_fatigue';
  } else if (state === 'dry' || state === 'kering' || state === 'risk_myopia') {
    cmd = 'D';
    status = 'risk_myopia';
  } else {
    cmd = 'N';
    status = 'normal';
  }

  currentEyeStatus = status;

  // 1. Send command character to ESP32 over TCP connection
  if (activeEspSocket) {
    activeEspSocket.write(cmd);
    console.log(`[TCP] Sent command '${cmd}' to ESP32 host`);
  } else {
    console.log(`[TCP] No active ESP32 host connection to send command '${cmd}'`);
  }

  // 2. Broadcast updated status to Socket.IO clients (Frontend)
  io.emit('eye-status', { status: currentEyeStatus });
  console.log(`[Socket.IO] Broadcasted eye-status: ${currentEyeStatus}`);

  res.json({
    success: true,
    message: `Status updated to ${status} (sent command '${cmd}' to ESP32)`,
    esp_connected: activeEspSocket !== null
  });
});

// Serve MJPEG video stream to browser
app.get('/video', (req, res) => {
  res.writeHead(200, {
    'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
    'Cache-Control': 'no-cache',
    Connection: 'close',
    Pragma: 'no-cache',
  });

  const interval = setInterval(() => {
    if (latestFrame) {
      res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${latestFrame.length}\r\n\r\n`);
      res.write(latestFrame);
      res.write('\r\n');
    }
  }, 50);

  req.on('close', () => clearInterval(interval));
});

app.get('/', (req, res) => {
  res.send(`
    <html>
      <body style="margin:0;background:#111;text-align:center;color:white;font-family:sans-serif;padding-top:20px;">
        <h2>SocaSob Backend Stream & Control</h2>
        <div style="margin: 20px 0;">
          <button onclick="setStatus('normal')" style="padding:10px 20px; font-size:16px; margin:5px; background:green; color:white; border:none; border-radius:5px; cursor:pointer;">Set Normal (Kedip Biasa)</button>
          <button onclick="setStatus('lelah')" style="padding:10px 20px; font-size:16px; margin:5px; background:orangered; color:white; border:none; border-radius:5px; cursor:pointer;">Set Mata Lelah (Marah)</button>
          <button onclick="setStatus('kering')" style="padding:10px 20px; font-size:16px; margin:5px; background:gold; color:black; border:none; border-radius:5px; cursor:pointer; font-weight:bold;">Set Mata Kering (Kaget)</button>
        </div>
        <script>
          function setStatus(status) {
            fetch('/api/status/' + status)
              .then(res => res.json())
              .then(data => alert(data.message));
          }
        </script>
        <img src="/video" style="max-width:90%; border:3px solid #333;" />
      </body>
    </html>
  `);
});

// Socket.IO event handler
io.on('connection', (socket) => {
  console.log(`[Socket.IO] Client connected: ${socket.id}`);
  
  // Send current state on connection
  socket.emit('eye-status', { status: currentEyeStatus });
  socket.emit('eye-distance', { distance: currentEyeDistance });
  socket.emit('timer-update', { hours: 0, minutes: 0, seconds: 0 });

  socket.on('disconnect', () => {
    console.log(`[Socket.IO] Client disconnected: ${socket.id}`);
  });
});

httpServer.listen(HTTP_PORT, '0.0.0.0', () => {
  console.log(`[HTTP] Server HTTP & Socket.IO berjalan di http://localhost:${HTTP_PORT}`);
});