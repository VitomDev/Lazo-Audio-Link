#include "core/web_page.h"

namespace lazo {

namespace {

// Split in parts: MSVC limits a single string literal to about 16 KB.
constexpr const char* kPageParts[] = {
R"LAZO(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<meta name="referrer" content="no-referrer">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Lazo">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<title>Lazo — Audio Link</title>
<style>
:root {
  --bg: #f4f5f8; --card: #ffffff; --text: #1a1c22; --muted: #686d7a; --line: #e3e5eb;
  --accent: #4f5bd5; --accent-text: #ffffff; --soft: #e9ebfb; --bad: #c0392b; --ok: #1e8e3e; --track: #e3e5eb;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #14151a; --card: #1e2027; --text: #eef0f4; --muted: #9ca1ad; --line: #2d3039;
    --accent: #8f98ff; --accent-text: #11132a; --soft: #262a45; --bad: #ff7b6b; --ok: #5fd07c; --track: #2d3039;
  }
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
body {
  margin: 0; min-height: 100vh; background: var(--bg); color: var(--text);
  font: 16px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  -webkit-text-size-adjust: 100%;
  padding: max(16px, env(safe-area-inset-top)) 16px max(24px, env(safe-area-inset-bottom));
}
main { max-width: 440px; margin: 0 auto; display: flex; flex-direction: column; gap: 14px; }
header { display: flex; align-items: center; gap: 12px; margin: 4px 0 6px; }
.logo { width: 44px; height: 44px; border-radius: 12px; flex: none;
        background: linear-gradient(135deg, #5b6cf0, #2bb3c0); display: grid; place-items: center; }
.logo svg { width: 30px; height: 30px; }
h1 { font-size: 21px; margin: 0; line-height: 1.2; }
.sub { font-size: 13px; color: var(--muted); }
#listen {
  -webkit-appearance: none; appearance: none; width: 100%; border: 0; border-radius: 18px; cursor: pointer;
  padding: 22px 16px; font: inherit; font-size: 20px; font-weight: 600;
  background: var(--accent); color: var(--accent-text);
  display: flex; align-items: center; justify-content: center; gap: 10px;
}
#listen svg { width: 24px; height: 24px; }
#listen.on { background: var(--soft); color: var(--text); }
section { background: var(--card); border: 1px solid var(--line); border-radius: 16px; padding: 14px 16px; }
.row { display: flex; justify-content: space-between; gap: 12px; padding: 5px 0; }
.row span:first-child { color: var(--muted); }
.row span:last-child { text-align: right; font-variant-numeric: tabular-nums; }
.ok { color: var(--ok); } .bad { color: var(--bad); }
#meter { height: 6px; background: var(--track); border-radius: 3px; overflow: hidden; margin-top: 10px; }
#meter div { height: 100%; width: 0; background: var(--ok); transition: width .08s linear; }
label { display: flex; justify-content: space-between; font-weight: 600; margin-bottom: 10px; }
label b { font-weight: 600; font-variant-numeric: tabular-nums; }
input[type=range] { width: 100%; accent-color: var(--accent); margin: 0; }
.ends { display: flex; justify-content: space-between; font-size: 13px; color: var(--muted); margin-top: 6px; }
p { margin: 0; }
.hint { color: var(--muted); font-size: 14px; }
.notes { display: flex; flex-direction: column; gap: 8px; padding: 0 4px; }
.notes p { display: flex; gap: 10px; }
.notes p::before { content: ""; flex: none; width: 6px; height: 6px; border-radius: 3px; background: var(--muted); margin-top: 8px; }
.switch { display: flex; align-items: center; justify-content: space-between; gap: 12px; font-weight: 600; margin: 0; }
.switch small { display: block; font-weight: normal; font-size: 13px; color: var(--muted); }
.switch input { -webkit-appearance: none; appearance: none; flex: none; width: 46px; height: 28px; border-radius: 14px;
                background: var(--track); position: relative; margin: 0; cursor: pointer; transition: background .15s; }
.switch input::after { content: ""; position: absolute; top: 3px; left: 3px; width: 22px; height: 22px; border-radius: 11px;
                       background: #fff; box-shadow: 0 1px 2px rgba(0,0,0,.3); transition: left .15s; }
.switch input:checked { background: var(--accent); }
.switch input:checked::after { left: 21px; }
.gap { height: 1px; background: var(--line); margin: 14px 0; }
input[type=range]:disabled { opacity: .45; }
#expired { display: none; border-color: var(--bad); }
#expired h2 { font-size: 17px; margin: 0 0 4px; }
#expired .hint { margin: 0 0 14px; }
#getLink {
  -webkit-appearance: none; appearance: none; width: 100%; border: 0; border-radius: 14px; cursor: pointer;
  padding: 15px 16px; font: inherit; font-size: 17px; font-weight: 600; background: var(--accent); color: var(--accent-text);
}
#getLink:disabled { opacity: .6; cursor: default; }
#linkNote { margin: 10px 0 0; text-align: center; }
#linkNote.bad { color: var(--bad); }
</style>
</head>
<body>
<main>
  <header>
    <div class="logo"><svg viewBox="0 0 32 32" fill="none" stroke="#fff" stroke-width="3.2" stroke-linecap="round">
      <rect x="3" y="10" width="15" height="12" rx="6"/><rect x="14" y="10" width="15" height="12" rx="6"/></svg></div>
    <div><h1>Lazo</h1><div class="sub" id="from">Audio from your PC</div></div>
  </header>

  <button id="listen" type="button">
    <svg viewBox="0 0 24 24" fill="currentColor" id="icon"><path d="M8 5.5v13a1 1 0 0 0 1.5.86l10.4-6.5a1 1 0 0 0 0-1.72L9.5 4.64A1 1 0 0 0 8 5.5z"/></svg>
    <span id="label">Listen</span>
  </button>

  <section id="expired">
    <h2>This link has changed</h2>
    <p class="hint">Get the new one from the PC: no need to scan the code again.</p>
    <button id="getLink" type="button">Get the new link</button>
    <p class="hint" id="linkNote">Lazo will ask on the PC whether to allow this device.</p>
  </section>

  <section>
    <div class="row"><span>Status</span><span id="status">Stopped</span></div>
    <div class="row"><span>Delay</span><span id="delay">–</span></div>
    <div class="row"><span>Buffer</span><span id="buf">–</span></div>
    <div class="row"><span>Dropouts / skips</span><span id="glitch">0 / 0</span></div>
    <div id="meter"><div></div></div>
  </section>

  <section>
    <label for="volume">Volume <b><span id="volumeVal">100</span> %</b></label>
    <input id="volume" type="range" min="0" max="100" step="1" value="100">
    <div class="gap"></div>
    <label class="switch" for="saver"><span>Data saver<small>Mono, a quarter of the data. For weak Wi-Fi.</small></span>
      <input id="saver" type="checkbox" aria-label="Data saver"></label>
  </section>

  <section>
    <label class="switch" for="auto"><span>Automatic buffer<small>As little delay as your network allows.</small></span>
      <input id="auto" type="checkbox" checked aria-label="Automatic buffer"></label>
    <div class="gap"></div>
    <label for="target">Buffer <b><span id="targetVal">80</span> ms</b></label>
    <input id="target" type="range" min="40" max="300" step="10" value="80">
    <div class="ends"><span>Less delay</span><span>More stable</span></div>
    <p class="hint" style="margin-top:10px" id="bufferHint">Lazo raises it after a dropout and lowers it again while all goes well.</p>
  </section>

  <div class="notes hint">
    <p>Bluetooth headphones add their own delay (often 100–200 ms). iPhones already count it in the delay above; other devices may not.</p>
    <p>Add this page to your home screen to open it in one tap next time.</p>
  </div>
</main>
)LAZO",
R"LAZO(<script>
'use strict';
const $ = s => document.querySelector(s);
let ctx = null, ws = null, node = null, running = false, failures = 0, retry = 0;
let rate = 48000, channels = 2;
let underruns = 0, skips = 0, level = 0;
const MIN_TARGET = 0.04, MAX_TARGET = 0.3;
let manualTarget = Math.min(MAX_TARGET, Math.max(MIN_TARGET, +storageGet('lazo.target', 80) / 1000));
let autoBuffer = storageGet('lazo.auto', '1') !== '0';
let target = autoBuffer ? 0.08 : manualTarget;
let volume = Math.min(1, Math.max(0, +storageGet('lazo.volume', 100) / 100));
let saver = storageGet('lazo.saver', '0') === '1';
// Automatic buffer: up 20 ms after a dropout, down 10 ms after every 20 s
// without one, never below what dropped out in the last 5 minutes.
let calmSince = 0, floor = MIN_TARGET, floorUntil = 0;

// One continuous ring buffer: packets from the network are written into it
// and a single audio node reads it without pause. Playing every packet as
// its own buffer leaves audible seams between them (a constant crackle on
// iPhones), so this is the only path audio takes.
const CAP = 48000 * 4;            // 4 s
let ring = new Float32Array(CAP * 2);
let written = 0;                  // frames written (absolute)
let readPos = 0;                  // read position (absolute, fractional)
let buffering = true, fadeIn = 0, avgFill = 0;

function storageGet(k, d) { try { return localStorage.getItem(k) ?? d; } catch { return d; } }
function storageSet(k, v) { try { localStorage.setItem(k, v); } catch {} }

function showTarget() {
  $('#target').value = Math.round(target * 1000);
  $('#targetVal').textContent = Math.round(target * 1000);
  $('#target').disabled = autoBuffer;
  $('#bufferHint').textContent = autoBuffer
    ? 'Lazo raises it after a dropout and lowers it again while all goes well.'
    : 'If you hear dropouts, move it to the right.';
}
$('#auto').checked = autoBuffer;
$('#auto').onchange = e => {
  autoBuffer = e.target.checked;
  storageSet('lazo.auto', autoBuffer ? '1' : '0');
  if (!autoBuffer) target = manualTarget;
  calmSince = performance.now();
  showTarget();
};
$('#target').oninput = e => {
  target = manualTarget = e.target.value / 1000;
  $('#targetVal').textContent = e.target.value;
  storageSet('lazo.target', e.target.value);
};
showTarget();

$('#volume').value = Math.round(volume * 100);
$('#volumeVal').textContent = Math.round(volume * 100);
$('#volume').oninput = e => {
  volume = e.target.value / 100;
  $('#volumeVal').textContent = e.target.value;
  storageSet('lazo.volume', e.target.value);
};

$('#saver').checked = saver;
$('#saver').onchange = e => {
  saver = e.target.checked;
  storageSet('lazo.saver', saver ? '1' : '0');
  // The PC sends the other stream on a new connection.
  if (running && ws) { const old = ws; ws = null; old.onclose = null; old.close(); connect(); }
};

function setStatus(text, cls = '') { const s = $('#status'); s.textContent = text; s.className = cls; }

const PLAY = '<path d="M8 5.5v13a1 1 0 0 0 1.5.86l10.4-6.5a1 1 0 0 0 0-1.72L9.5 4.64A1 1 0 0 0 8 5.5z"/>';
const STOP = '<rect x="6" y="6" width="12" height="12" rx="2.5"/>';
function setButton(on) {
  $('#listen').classList.toggle('on', on);
  $('#label').textContent = on ? 'Stop' : 'Listen';
  $('#icon').innerHTML = on ? STOP : PLAY;
}

function resetRing() {
  ring = new Float32Array(CAP * channels);
  written = 0; readPos = 0; buffering = true; avgFill = 0;
}

$('#listen').onclick = async () => {
  if (running) return stop();
  running = true; failures = 0;
  setButton(true);
  $('#expired').style.display = 'none';

  // iOS: play even with the silent switch on.
  try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch {}

  if (!ctx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    try { ctx = new AC({ latencyHint: 'interactive', sampleRate: 48000 }); }
    catch { ctx = new AC({ latencyHint: 'interactive' }); }
    // AudioWorklet needs https; a ScriptProcessor works on a plain local address.
    node = ctx.createScriptProcessor(1024, 1, 2);
    node.onaudioprocess = render;
    node.connect(ctx.destination);
    window._lazoNode = node;      // Safari must not collect it
  }
  await ctx.resume();
  // Unlocks audio on iOS: an empty buffer played from the tap.
  const s = ctx.createBufferSource(); s.buffer = ctx.createBuffer(1, 1, ctx.sampleRate);
  s.connect(ctx.destination); s.start();

  resetRing();
  connect();
};

function stop() {
  running = false;
  clearTimeout(retry);
  setButton(false);
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
  if (ctx) ctx.suspend();
  setStatus('Stopped');
}

function connect() {
  if (!running) return;
  setStatus(failures ? 'Reconnecting…' : 'Connecting…', failures ? 'bad' : '');
  const socket = new WebSocket(`ws://${location.host}/ws${location.search}${saver ? '&q=saver' : ''}`);
  ws = socket;
  socket.binaryType = 'arraybuffer';
  socket.onmessage = e => {
    if (typeof e.data === 'string') {
      const cfg = JSON.parse(e.data);
      rate = cfg.rate; channels = cfg.channels;
      if (cfg.pc) $('#from').textContent = 'Audio from ' + cfg.pc;
      resetRing();
      failures = 0;
      // Counted per connection, as the PC shows them.
      underruns = 0; skips = 0;
      setStatus('Listening', 'ok');
      return;
    }
    push(e.data);
  };
  socket.onclose = () => {
    if (ws !== socket || !running) return;
    failures++;
    setStatus('Reconnecting…', 'bad');
    // Several failures in a row: the PC may have a new link.
    if (failures === 3) {
      fetch('/api/ping' + location.search, { cache: 'no-store' }).then(r => {
        if (r.status === 403) { stop(); showExpired(); }
      }).catch(() => {});
    }
    retry = setTimeout(connect, 1000);
  };
  socket.onerror = () => socket.close();
}

// The PC made a new link (or this one is old). Rather than scanning the code
// again, ask the PC for it: Lazo shows the request there, and only if its
// user allows it does the new link come back. Then the page reopens with it.
function showExpired() {
  $('#expired').style.display = 'block';
  $('#listen').style.display = 'none';
}

function linkNote(text, bad = false) { const n = $('#linkNote'); n.textContent = text; n.className = 'hint' + (bad ? ' bad' : ''); }

$('#getLink').onclick = async () => {
  const button = $('#getLink');
  button.disabled = true;
  button.textContent = 'Waiting for the PC\u2026';
  linkNote('Click Allow on the PC.');
  let message = 'Can\u2019t reach the PC. Is it on the same network?';
  try {
    const r = await fetch('/api/link', { method: 'POST', cache: 'no-store' });
    if (r.ok) {
      const { link } = await r.json();
      if (link) { linkNote('Opening the new link\u2026'); location.replace(link); return; }
    }
    message = r.status === 403 ? 'The PC didn\u2019t allow it.'
            : r.status === 408 ? 'No answer from the PC. Try again.'
            : r.status === 429 ? 'The PC is already being asked. Answer it there.'
            : message;
  } catch {}
  button.disabled = false;
  button.textContent = 'Try again';
  linkNote(message, true);
};

// An old link is noticed as soon as the page opens.
fetch('/api/ping' + location.search, { cache: 'no-store' }).then(r => { if (r.status === 403) showExpired(); }).catch(() => {});

// Network -> ring buffer
function push(buf) {
  const pcm = new Int16Array(buf);
  const n = pcm.length / channels;
  if (written + n - readPos > CAP - 1024) return;   // full (should not happen)
  let peak = 0;
  let w = (written % CAP) * channels;
  for (let i = 0; i < pcm.length; i++) {
    const v = pcm[i] / 32768;
    ring[w++] = v; if (w >= ring.length) w = 0;
    const a = v < 0 ? -v : v;
    if (a > peak) peak = a;
  }
  written += n;
  level = Math.max(peak, level * 0.9);

  // Far behind (the page was in the background, say): jump ahead once
  // rather than dropping packets here and there, which clicks.
  if (!buffering && (written - readPos) / rate > target + 0.3) {
    readPos = written - target * rate; skips++; fadeIn = 0;
  }
}
)LAZO",
R"LAZO(
// Ring buffer -> speaker (called by the browser every 1024 frames)
function render(e) {
  const L = e.outputBuffer.getChannelData(0);
  const R = e.outputBuffer.getChannelData(1);
  const N = L.length;
  const fill = written - readPos;

  if (buffering) {
    if (fill / rate >= target) { buffering = false; fadeIn = 0; avgFill = fill / rate; }
    else { L.fill(0); R.fill(0); return; }
  }

  // The PC's and the phone's clocks drift apart: follow by playing very
  // slightly faster or slower (at most 0.3 %, inaudible), never by dropping.
  avgFill = avgFill * 0.97 + (fill / rate) * 0.03;
  const adj = Math.max(-0.003, Math.min(0.003, (avgFill - target) * 0.03));
  const step = (rate / ctx.sampleRate) * (1 + adj);

  if (fill < N * step + 2) {          // ran dry: silence, then fill up again
    underruns++; buffering = true;
    if (autoBuffer) {
      target = Math.min(MAX_TARGET, target + 0.02);
      floor = target; floorUntil = performance.now() + 300000; calmSince = performance.now();
    }
    L.fill(0); R.fill(0); return;
  }

  const ch = channels, ringLen = ring.length;
  let pos = readPos;
  for (let i = 0; i < N; i++) {
    const idx = Math.floor(pos), f = pos - idx;
    const a = (idx % CAP) * ch;
    let b = a + ch; if (b >= ringLen) b = 0;
    let l = ring[a] + (ring[b] - ring[a]) * f;
    let r = ch > 1 ? ring[a + 1] + (ring[b + 1] - ring[a + 1]) * f : l;
    if (fadeIn < 256) { const g = fadeIn++ / 256; l *= g; r *= g; }
    l *= volume; r *= volume;
    L[i] = l; R[i] = r;
    pos += step;
  }
  readPos = pos;
}

// Back from the background: iOS may have suspended the audio.
document.addEventListener('visibilitychange', () => {
  if (running && ctx && !document.hidden && ctx.state !== 'running') ctx.resume();
});

// The PC's own share of the delay: a 10 ms packet plus capture and network.
const PC_DELAY_MS = 15;
let lastStats = 0;
setInterval(() => {
  if (!running || !ctx) { $('#meter div').style.width = '0%'; $('#delay').textContent = '–'; return; }
  const now = performance.now();
  if (autoBuffer && !buffering && now - calmSince > 20000) {
    calmSince = now;
    const lowest = now < floorUntil ? floor : MIN_TARGET;
    if (target - 0.01 >= lowest - 1e-9) { target = Math.max(MIN_TARGET, target - 0.01); showTarget(); }
  }
  if (autoBuffer && +$('#target').value !== Math.round(target * 1000)) showTarget();
  const ahead = Math.max(0, (written - readPos) / rate) * 1000;
  const out = ((ctx.outputLatency || 0) + (ctx.baseLatency || 0)) * 1000;
  const connected = $('#status').textContent === 'Listening';
  $('#delay').textContent = connected ? `\u2248 ${(ahead + out + PC_DELAY_MS).toFixed(0)} ms` : '–';
  $('#buf').textContent = `${ahead.toFixed(0)} ms` + (out ? ` + ${out.toFixed(0)} ms output` : '');
  // The PC shows each listener's delay and dropouts.
  if (connected && ws && ws.readyState === 1 && now - lastStats > 2000) {
    lastStats = now;
    ws.send(JSON.stringify({ buffer: Math.round(ahead), output: Math.round(out), dropouts: underruns, skips }));
  }
  $('#glitch').textContent = `${underruns} / ${skips}`;
  $('#meter div').style.width = Math.min(100, level * 100) + '%';
  level *= 0.9;
}, 200);
</script>
</body>
</html>
)LAZO",
};

std::string Join() {
    std::string page;
    for (const char* part : kPageParts) page += part;
    return page;
}

}  // namespace

const std::string& PageHtml() {
    static const std::string page = Join();
    return page;
}

}  // namespace lazo
