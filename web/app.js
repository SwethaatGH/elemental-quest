// Elemental Quest - lobby and game page
'use strict';

const params = new URLSearchParams(location.search);
const SERVER = (params.get('server') || window.EQ_SERVER || '').replace(/\/+$/, '');
const WS_SERVER = SERVER.replace(/^http/, 'ws');
const ELEMENT_CLASS = { FIRE: 'el-fire', WATER: 'el-water', EARTH: 'el-earth', AIR: 'el-air' };

const $ = (id) => document.getElementById(id);
const ui = {
  status: $('server-status'), statusText: $('server-status-text'),
  choose: $('choose'), ready: $('ready'), message: $('message'),
  create: $('create'), join: $('join'), joinForm: $('join-form'), code: $('code'),
  roomCode: $('room-code'), shareLink: $('share-link'), copy: $('copy'), enter: $('enter'), back: $('back'),
  lobby: $('lobby'), game: $('game'), barCode: $('bar-code'), barElement: $('bar-element'), leave: $('leave'),
  overlay: $('overlay'), overlayText: $('overlay-text'), overlayAction: $('overlay-action'), progress: $('progress-bar'),
  canvas: $('canvas'),
};

let room = null;
let serverAwake = false;
let gameLoading = null;  // resolves once the game's code has loaded
let started = false;

// --- small helpers -----------------------------------------------------------------

function showMessage(text) {
  ui.message.textContent = text;
  ui.message.classList.toggle('hidden', !text);
}

function lobbyUrl(code) {
  const url = new URL(location.pathname, location.origin);
  if (params.get('server')) url.searchParams.set('server', params.get('server'));
  if (code) url.searchParams.set('room', code);
  return url.toString();
}

async function api(path, options = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), options.timeout || 10000);
  try {
    const response = await fetch(SERVER + path, { ...options, signal: controller.signal });
    const body = await response.json().catch(() => ({}));
    return { ok: response.ok, status: response.status, body };
  } finally {
    clearTimeout(timer);
  }
}

// --- waking the server -------------------------------------------------------------
// A free Render service sleeps when nobody plays; the first visit wakes it (~30-60 s)

async function wake() {
  if (!SERVER) {
    ui.status.className = 'status down';
    ui.statusText.textContent = 'No game server is set up yet (see config.js).';
    return;
  }
  const startedAt = Date.now();
  for (;;) {
    try {
      const { ok } = await api('/api/health', { timeout: 8000 });
      if (ok) break;
    } catch (err) {
      // still asleep, or starting
    }
    const seconds = Math.round((Date.now() - startedAt) / 1000);
    ui.statusText.textContent = seconds < 120
      ? `Waking the temple… ${seconds} s (the free server naps when nobody plays; this can take a minute)`
      : 'The temple is not answering. It may be down; this page keeps trying.';
    if (seconds >= 120) ui.status.className = 'status down';
    await new Promise((r) => setTimeout(r, 3000));
  }
  serverAwake = true;
  ui.status.className = 'status awake';
  ui.statusText.textContent = 'The temple is open';
  ui.create.disabled = false;
  ui.join.disabled = false;
  if (params.get('room')) ui.code.focus();
}

// --- rooms -------------------------------------------------------------------------

function showRoom(code) {
  room = code;
  ui.roomCode.textContent = code;
  ui.shareLink.value = lobbyUrl(code);
  ui.choose.classList.add('hidden');
  ui.ready.classList.remove('hidden');
  showMessage('');
  history.replaceState(null, '', lobbyUrl(code));
  loadGame();  // start downloading while they read the code
  ui.enter.focus();
}

ui.create.addEventListener('click', async () => {
  ui.create.disabled = true;
  showMessage('');
  try {
    const { ok, body } = await api('/api/rooms', { method: 'POST' });
    if (!ok) return showMessage(body.error || 'Could not create a room. Please try again.');
    showRoom(body.room);
  } catch (err) {
    showMessage('Could not reach the temple. Please try again.');
  } finally {
    ui.create.disabled = !serverAwake;
  }
});

ui.joinForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const code = ui.code.value.trim().toUpperCase();
  if (!/^[A-Z]{4}$/.test(code)) return showMessage('A room code is 4 letters, like ABCD.');
  ui.join.disabled = true;
  showMessage('');
  try {
    const { ok, body } = await api(`/api/rooms/${code}`);
    if (!ok) return showMessage(body.error || 'No room with that code.');
    if (body.players >= body.maxPlayers) return showMessage('That room already has 4 players.');
    showRoom(code);
  } catch (err) {
    showMessage('Could not reach the temple. Please try again.');
  } finally {
    ui.join.disabled = !serverAwake;
  }
});

ui.code.addEventListener('input', () => {
  ui.code.value = ui.code.value.replace(/[^a-z]/gi, '').toUpperCase().slice(0, 4);
});

ui.copy.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(ui.shareLink.value);
    ui.copy.textContent = 'Copied';
  } catch (err) {
    ui.shareLink.select();
  }
  setTimeout(() => { ui.copy.textContent = 'Copy'; }, 1500);
});

ui.back.addEventListener('click', () => {
  ui.ready.classList.add('hidden');
  ui.choose.classList.remove('hidden');
  history.replaceState(null, '', lobbyUrl(null));
  room = null;
});

// --- the game ----------------------------------------------------------------------

function setOverlay(text, { progress = null, action = null } = {}) {
  ui.overlay.classList.remove('hidden');
  ui.overlayText.textContent = text;
  ui.progress.parentElement.classList.toggle('hidden', progress === null);
  if (progress !== null) ui.progress.style.width = `${Math.round(progress * 100)}%`;
  ui.overlayAction.classList.toggle('hidden', !action);
  if (action) {
    ui.overlayAction.textContent = action.label;
    ui.overlayAction.onclick = action.run;
  }
}

function loadGame() {
  if (gameLoading) return gameLoading;
  gameLoading = new Promise((resolve, reject) => {
    window.Module = {
      canvas: ui.canvas,
      noInitialRun: true,
      print: (text) => console.log(text),
      printErr: (text) => console.warn(text),
      // Emscripten reports downloads as "Downloading data... (done/total)"
      setStatus: (text) => {
        const match = text.match(/\((\d+(?:\.\d+)?)\/(\d+)\)/);
        if (match && !started) setOverlay('Loading the temple…', { progress: Number(match[1]) / Number(match[2]) });
      },
      onRuntimeInitialized: resolve,
      onAbort: (what) => reject(new Error(String(what))),
      eqNotify: onGameNotice,
    };
    const script = document.createElement('script');
    script.src = 'elemental_client.js';
    script.onerror = () => reject(new Error('could not download the game'));
    document.body.appendChild(script);
  });
  return gameLoading;
}

ui.enter.addEventListener('click', async () => {
  if (!room) return;
  ui.lobby.classList.add('hidden');
  ui.game.classList.remove('hidden');
  ui.barCode.textContent = room;
  setOverlay('Loading the temple…', { progress: 0 });
  try {
    await loadGame();
  } catch (err) {
    return setOverlay(`The game could not load (${err.message}).`, { action: backToLobby });
  }
  started = true;
  setOverlay(`Joining room ${room}…`);
  window.Module.callMain(['--connect', `${WS_SERVER}/ws?room=${encodeURIComponent(room)}`]);
});

const backToLobby = { label: 'Back to the lobby', run: () => { location.href = lobbyUrl(room); } };

function onGameNotice(kind, text) {
  if (kind === 'joined') {
    ui.overlay.classList.add('hidden');
    ui.barElement.textContent = `You are ${text}`;
    ui.barElement.className = `bar-element ${ELEMENT_CLASS[text] || ''}`;
    ui.canvas.focus();
  } else if (kind === 'error') {
    setOverlay(text || 'Could not join the room.', { action: backToLobby });
  } else if (kind === 'dropped') {
    setOverlay('The connection to the temple was lost (a tab left in the background is disconnected after a few seconds).',
      { action: { label: 'Rejoin', run: () => { location.href = lobbyUrl(room); } } });
  } else if (kind === 'ended') {
    location.href = lobbyUrl(room);
  }
}

ui.leave.addEventListener('click', () => { location.href = lobbyUrl(null); });

// Arrow keys and Space are game keys while playing, not page scrolling
window.addEventListener('keydown', (event) => {
  if (!ui.game.classList.contains('hidden') && [' ', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(event.key)) {
    event.preventDefault();
  }
}, { capture: true });

// --- start -------------------------------------------------------------------------

if (params.get('room')) ui.code.value = params.get('room').toUpperCase().slice(0, 4);
wake();
