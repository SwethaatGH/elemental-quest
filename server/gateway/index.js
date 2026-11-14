// Elemental Quest gateway: lets browsers play the game.
//
// Browsers cannot speak ZeroMQ, and a host like Render exposes one public port. So
// this small service sits in front of the unchanged C++ game server:
//
//   * Rooms. POST /api/rooms starts an elemental_server process on its own private
//     ports and returns a 4-letter code. Up to 4 players share a room.
//   * Relay. A browser opens a WebSocket to /ws?room=CODE. The gateway joins the
//     room's server for it, then relays its requests (ZeroMQ REQ/REP) and the world
//     broadcast (ZeroMQ SUB) over that one WebSocket. The framing is documented in
//     engine/src/ClientWeb.cpp.
//   * Cleanup. A room nobody has been in for a while is shut down.
//
// Settings (environment variables): PORT, SERVER_BIN, SERVER_ARGS, MAX_ROOMS, ROOM_IDLE_SECONDS,
// ALLOWED_ORIGINS (comma-separated; empty allows any), STATIC_DIR (optional: also
// serve the web client, handy for local testing or single-host deployments).
'use strict';

const fs = require('fs');
const http = require('http');
const path = require('path');
const { spawn } = require('child_process');
const { WebSocketServer } = require('ws');
const zmq = require('zeromq');

const PORT = Number(process.env.PORT || 8080);
const SERVER_BIN = process.env.SERVER_BIN || path.join(__dirname, '../../build/elemental_server');
const MAX_ROOMS = Number(process.env.MAX_ROOMS || 8);
const ROOM_IDLE_SECONDS = Number(process.env.ROOM_IDLE_SECONDS || 120);
const ALLOWED_ORIGINS = (process.env.ALLOWED_ORIGINS || '').split(',').map((s) => s.trim()).filter(Boolean);
const STATIC_DIR = process.env.STATIC_DIR || '';
// Extra flags for every room's server. Delta snapshots send only what changed, which
// cuts each player's download from ~400 KB/s to a small fraction of that
const SERVER_ARGS = (process.env.SERVER_ARGS || '--delta on --keyframe 60').split(/\s+/).filter(Boolean);

const PORT_FIRST = 20000;  // room k uses PORT_FIRST + 200k .. + 200k + 103
const PORT_STRIDE = 200;
const REQUEST_TIMEOUT_MS = 3000;
const MAX_QUEUED_REQUESTS = 120;  // past this a browser's requests are failed, not queued
const MAX_BUFFERED_BYTES = 1 << 20;  // world messages are skipped while a browser is this far behind
const CODE_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ';  // no I or O: easy to read aloud

const rooms = new Map();  // code -> Room

function log(...args) {
  console.log(new Date().toISOString(), ...args);
}

// --- rooms -----------------------------------------------------------------------

function newCode() {
  for (;;) {
    let code = '';
    for (let i = 0; i < 4; i++) code += CODE_LETTERS[Math.floor(Math.random() * CODE_LETTERS.length)];
    if (!rooms.has(code)) return code;
  }
}

function freeSlot() {
  const used = new Set([...rooms.values()].map((r) => r.slot));
  for (let slot = 0; slot < MAX_ROOMS; slot++) if (!used.has(slot)) return slot;
  return -1;
}

function createRoom() {
  const slot = freeSlot();
  if (slot < 0) return null;
  const code = newCode();
  const base = PORT_FIRST + slot * PORT_STRIDE;
  const room = { code, slot, base, players: new Set(), lastActive: Date.now(), closed: false };
  room.proc = spawn(SERVER_BIN, ['--port-base', String(base), '--client-timeout-ms', '10000', ...SERVER_ARGS], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  room.ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('the game server did not start')), 10000);
    const watch = (chunk) => {
      const text = chunk.toString();
      if (text.includes('join channel listening')) {
        clearTimeout(timer);
        resolve();
      }
      for (const line of text.split('\n')) if (line.includes('[game]')) log(`[room ${code}]`, line.replace(/^.*\[game\] /, ''));
    };
    room.proc.stdout.on('data', watch);
    room.proc.stderr.on('data', watch);
    room.proc.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
  room.ready.catch((err) => log(`room ${code} failed to start:`, err.message));
  room.proc.on('exit', (status) => {
    log(`room ${code} closed (server exit ${status})`);
    room.closed = true;
    rooms.delete(code);
    for (const player of room.players) player.close('The room has closed.');
  });
  rooms.set(code, room);
  log(`room ${code} created on ports ${base}+ (${rooms.size}/${MAX_ROOMS} rooms)`);
  return room;
}

function closeRoom(room, why) {
  if (room.closed) return;
  log(`room ${room.code}: ${why}`);
  room.closed = true;
  room.proc.kill('SIGTERM');
}

// Rooms with nobody in them for a while are shut down
setInterval(() => {
  const now = Date.now();
  for (const room of rooms.values()) {
    if (room.players.size === 0 && now - room.lastActive > ROOM_IDLE_SECONDS * 1000) closeRoom(room, 'idle, shutting down');
  }
}, 15000).unref();

// --- one browser player ------------------------------------------------------------

class Player {
  constructor(ws, room) {
    this.ws = ws;
    this.room = room;
    this.queue = [];  // requests waiting for the channel, in order
    this.busy = false;
    this.session = '0';
    this.closed = false;
    this.channel = null;
    this.world = null;
  }

  sendFrame(kind, body) {
    if (this.ws.readyState !== this.ws.OPEN) return;
    const payload = typeof body === 'string' ? Buffer.from(body) : Buffer.from(body || []);
    this.ws.send(Buffer.concat([Buffer.from(kind), payload]), { binary: true });
  }

  async start() {
    try {
      await this.room.ready;
    } catch (err) {
      return this.fail('The game server could not start. Please try again.');
    }
    // Join exactly as a native client does
    const join = new zmq.Request({ receiveTimeout: REQUEST_TIMEOUT_MS, sendTimeout: REQUEST_TIMEOUT_MS, linger: 0 });
    join.connect(`tcp://127.0.0.1:${this.room.base}`);
    let welcome;
    try {
      await join.send('JOIN');
      [welcome] = await join.receive();
      welcome = welcome.toString();
    } catch (err) {
      return this.fail('The game server did not answer. Please try again.');
    } finally {
      join.close();
    }
    if (!welcome.startsWith('WELCOME')) {
      return this.fail(welcome.includes('full') ? 'This room already has 4 players.' : `The server refused: ${welcome}`);
    }
    if (this.closed) return;  // the browser left while we were joining
    const [, id, channelPort, , session] = welcome.split(/\s+/);
    this.session = session || '0';
    this.channelPort = Number(channelPort);
    this.channel = this.newChannel();
    this.world = new zmq.Subscriber({ linger: 0, receiveHighWaterMark: 64 });
    this.world.connect(`tcp://127.0.0.1:${this.room.base + 1}`);
    this.world.subscribe();
    this.sendFrame('W', welcome);
    log(`room ${this.room.code}: player ${id} joined (${this.room.players.size} connected)`);
    this.relayWorld();
    this.pump();
  }

  newChannel() {
    const channel = new zmq.Request({ receiveTimeout: REQUEST_TIMEOUT_MS, sendTimeout: REQUEST_TIMEOUT_MS, linger: 0 });
    channel.connect(`tcp://127.0.0.1:${this.channelPort}`);
    return channel;
  }

  async relayWorld() {
    const S = Buffer.from('S');
    try {
      for await (const [message] of this.world) {
        if (this.closed) break;
        // A browser that has fallen behind skips snapshots rather than queueing them
        if (this.ws.bufferedAmount > MAX_BUFFERED_BYTES) continue;
        this.ws.send(Buffer.concat([S, message]), { binary: true });
      }
    } catch (err) {
      // the socket was closed
    }
  }

  // One request at a time, as REQ/REP requires; every request gets exactly one reply
  onRequest(body) {
    if (this.queue.length >= MAX_QUEUED_REQUESTS) this.queue.push(null);  // fails, in order
    else this.queue.push(body);
    this.pump();
  }

  async pump() {
    if (this.busy || !this.channel) return;
    this.busy = true;
    while (this.queue.length && !this.closed) {
      const body = this.queue.shift();
      if (body === null) {
        this.sendFrame('X', 'busy');
        continue;
      }
      try {
        await this.channel.send(body);
        const [reply] = await this.channel.receive();
        this.sendFrame('A', reply);
      } catch (err) {
        this.sendFrame('X', 'timeout');
        // A REQ socket that timed out cannot send again
        this.channel.close();
        this.channel = this.newChannel();
      }
    }
    this.busy = false;
  }

  fail(message) {
    this.sendFrame('E', message);
    this.ws.close(4000, 'join refused');
    this.cleanup();
  }

  close(reason) {
    if (this.ws.readyState === this.ws.OPEN) {
      this.sendFrame('E', reason);
      this.ws.close(4001, 'room closed');
    }
    this.cleanup();
  }

  // The browser left: say BYE for it so its player disappears at once
  async cleanup() {
    if (this.closed) return;
    this.closed = true;
    this.room.players.delete(this);
    this.room.lastActive = Date.now();
    if (this.world) this.world.close();
    const channel = this.channel;
    this.channel = null;
    if (!channel) return;
    // Wait for a request in flight to finish before saying goodbye
    for (let i = 0; i < 40 && this.busy; i++) await new Promise((r) => setTimeout(r, 50));
    try {
      if (!channel.closed) {
        await channel.send(`\x01${this.session}\nBYE`);
        await channel.receive();
      }
    } catch (err) {
      // the server may already have timed the player out
    }
    channel.close();
    log(`room ${this.room.code}: a player left (${this.room.players.size} connected)`);
  }
}

// --- HTTP: rooms API, health check, optional static files --------------------------

function allowedOrigin(origin) {
  return !origin || ALLOWED_ORIGINS.length === 0 || ALLOWED_ORIGINS.includes(origin);
}

function sendJson(req, res, status, body) {
  const headers = { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' };
  const origin = req.headers.origin;
  if (origin && allowedOrigin(origin)) {
    headers['Access-Control-Allow-Origin'] = origin;
    headers.Vary = 'Origin';
  }
  res.writeHead(status, headers);
  res.end(JSON.stringify(body));
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm',
  '.data': 'application/octet-stream', '.png': 'image/png', '.jpeg': 'image/jpeg', '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.json': 'application/json',
};

function serveStatic(req, res) {
  const url = new URL(req.url, 'http://localhost');
  let file = path.normalize(path.join(STATIC_DIR, decodeURIComponent(url.pathname)));
  if (!file.startsWith(path.resolve(STATIC_DIR))) return sendJson(req, res, 403, { error: 'forbidden' });
  if (fs.existsSync(file) && fs.statSync(file).isDirectory()) file = path.join(file, 'index.html');
  fs.readFile(file, (err, data) => {
    if (err) return sendJson(req, res, 404, { error: 'not found' });
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (req.method === 'OPTIONS') {
    const origin = req.headers.origin;
    res.writeHead(204, allowedOrigin(origin) && origin ? {
      'Access-Control-Allow-Origin': origin, 'Access-Control-Allow-Methods': 'GET, POST',
      'Access-Control-Allow-Headers': 'Content-Type', 'Access-Control-Max-Age': '86400', Vary: 'Origin',
    } : {});
    return res.end();
  }
  if (url.pathname === '/api/health') {
    return sendJson(req, res, 200, { ok: true, rooms: rooms.size, maxRooms: MAX_ROOMS });
  }
  if (url.pathname === '/api/rooms' && req.method === 'POST') {
    if (!allowedOrigin(req.headers.origin)) return sendJson(req, res, 403, { error: 'This site may not create rooms.' });
    const room = createRoom();
    if (!room) return sendJson(req, res, 503, { error: 'The temple is full right now. Please try again in a few minutes.' });
    return sendJson(req, res, 201, { room: room.code });
  }
  const match = url.pathname.match(/^\/api\/rooms\/([A-Za-z]{4})$/);
  if (match && req.method === 'GET') {
    const room = rooms.get(match[1].toUpperCase());
    if (!room) return sendJson(req, res, 404, { error: 'No room with that code. Check it, or create a new room.' });
    return sendJson(req, res, 200, { room: room.code, players: room.players.size, maxPlayers: 4 });
  }
  if (STATIC_DIR && req.method === 'GET') return serveStatic(req, res);
  sendJson(req, res, 404, { error: 'not found' });
});

const wss = new WebSocketServer({ noServer: true, maxPayload: 512 * 1024 });

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname !== '/ws' || !allowedOrigin(req.headers.origin)) {
    socket.destroy();
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const code = (url.searchParams.get('room') || '').toUpperCase();
    const room = rooms.get(code);
    if (!room || room.closed) {
      ws.send(Buffer.from('ENo room with that code. Check it, or create a new room.'), { binary: true });
      ws.close(4004, 'no such room');
      return;
    }
    const player = new Player(ws, room);
    room.players.add(player);
    room.lastActive = Date.now();
    ws.on('message', (data) => {
      const buffer = Buffer.isBuffer(data) ? data : Buffer.from(data);
      if (buffer.length && buffer[0] === 0x51 /* 'Q' */) player.onRequest(buffer.subarray(1));
    });
    ws.on('close', () => player.cleanup());
    ws.on('error', () => player.cleanup());
    player.start();
  });
});

function shutdown() {
  log('shutting down');
  for (const room of rooms.values()) closeRoom(room, 'gateway stopping');
  server.close();
  setTimeout(() => process.exit(0), 500).unref();
}
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);

server.listen(PORT, () => {
  log(`Elemental Quest gateway on port ${PORT}; game server ${SERVER_BIN}; up to ${MAX_ROOMS} rooms` +
      (STATIC_DIR ? `; serving ${STATIC_DIR}` : ''));
});
