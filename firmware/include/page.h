#pragma once

// Control page served at "/". Self-contained: no external scripts or fonts,
// so it works without internet. Polls /api/state and /api/log once a second.
static const char PAGE[] PROGMEM = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Receiver Control</title>
<style>
:root {
  --bg: #f4f2ee; --card: #fffdf9; --ink: #1d1b18; --muted: #6f6a62; --line: #e2ddd4;
  --accent: #c2410c; --accent-ink: #fff; --on: #15803d; --off: #8a847a; --warn: #b45309;
  --btn: #efebe4; --btn-hover: #e5e0d7;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #161513; --card: #201e1b; --ink: #ece8e1; --muted: #9b958b; --line: #34312c;
    --accent: #f97316; --accent-ink: #1a0d03; --on: #4ade80; --off: #8a847a; --warn: #fbbf24;
    --btn: #2b2925; --btn-hover: #36332e;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 16px 16px 48px; }
header { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 12px; }
h1 { font-size: 15px; font-weight: 600; letter-spacing: .04em; margin: 0; color: var(--muted); }
.link { font-size: 13px; color: var(--muted); display: flex; align-items: center; gap: 6px; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--off); }
.dot.ok { background: var(--on); }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 14px; margin-bottom: 12px; }
.hero { display: grid; gap: 10px; }
.hero .input { font-size: 32px; font-weight: 700; line-height: 1.1; }
.hero .sub { color: var(--muted); font-size: 13px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; }
.chip { font-size: 12px; padding: 3px 9px; border-radius: 999px; background: var(--btn); color: var(--muted); }
.chip.power-on { background: color-mix(in srgb, var(--on) 18%, transparent); color: var(--on); font-weight: 600; }
.chip.flag { background: color-mix(in srgb, var(--warn) 18%, transparent); color: var(--warn); font-weight: 600; }
h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .08em; color: var(--muted); margin: 0 0 10px; font-weight: 600; }
.row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.row + .row { margin-top: 8px; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(104px, 1fr)); gap: 8px; }
button { font: inherit; border: 1px solid transparent; background: var(--btn); color: var(--ink);
  border-radius: 8px; padding: 9px 12px; cursor: pointer; min-height: 40px; }
button:hover { background: var(--btn-hover); }
button.active { background: var(--accent); color: var(--accent-ink); font-weight: 600; }
button.pending { border-color: var(--accent); animation: pulse 1s infinite; }
@keyframes pulse { 50% { opacity: .55; } }
.pair { display: inline-flex; }
.pair button:first-child { border-radius: 8px 0 0 8px; }
.pair button:last-child { border-radius: 0 8px 8px 0; margin-left: 1px; }
.pair button:not(:first-child):not(:last-child) { border-radius: 0; margin-left: 1px; }
.label { font-size: 13px; color: var(--muted); min-width: 64px; }
input, select { font: inherit; color: var(--ink); background: var(--bg); border: 1px solid var(--line);
  border-radius: 8px; padding: 8px 10px; min-height: 40px; }
input[type=number] { width: 96px; }
details { background: var(--card); border: 1px solid var(--line); border-radius: 12px; margin-bottom: 12px; }
details > summary { cursor: pointer; padding: 14px; font-size: 12px; text-transform: uppercase;
  letter-spacing: .08em; color: var(--muted); font-weight: 600; list-style: none; }
details > summary::before { content: "\25B8  "; }
details[open] > summary::before { content: "\25BE  "; }
details > .body { padding: 0 14px 14px; }
.note { font-size: 12px; color: var(--muted); margin-top: 8px; }
pre { margin: 8px 0 0; max-height: 280px; overflow: auto; font: 12px/1.45 ui-monospace, Consolas, monospace;
  background: var(--bg); border: 1px solid var(--line); border-radius: 8px; padding: 8px; white-space: pre-wrap; word-break: break-all; }
.setting { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; padding: 8px 0; border-top: 1px solid var(--line); }
.setting:first-child { border-top: 0; }
.setting .name { flex: 1 1 180px; font-size: 14px; }
</style>
</head>
<body>
<main>
<header>
  <h1 id="model">RECEIVER</h1>
  <div class="link"><span class="dot" id="dot"></span><span id="linktext">connecting</span></div>
</header>

<section class="card hero">
  <div class="sub">Current input</div>
  <div class="input" id="current">-</div>
  <div class="chips" id="chips"></div>
</section>

<section class="card">
  <h2>Power &amp; volume</h2>
  <div class="row">
    <span class="pair"><button data-cmd="c02e" id="pon">On</button><button data-cmd="c02f" id="poff">Standby</button></span>
    <span class="pair"><button data-cmd="c015 c015 c015 c015 c015 c015 c015 c015 c015 c015">Vol &minus;10</button><button data-cmd="c015">Vol &minus;1</button><button data-cmd="c014">Vol +1</button><button data-cmd="c014 c014 c014 c014 c014 c014 c014 c014 c014 c014">Vol +10</button></span>
    <button id="mute">Mute</button>
    <button id="multi">5.1 input</button>
  </div>
  <div class="note">The receiver doesn't report volume, so the level isn't shown.</div>
</section>

<section class="card">
  <h2>Input</h2>
  <div class="grid" id="inputs"></div>
</section>

<details data-key="amp">
  <summary>Recording out &amp; input type</summary>
  <div class="body">
    <div class="row"><span class="label">Rec out</span><span class="row" id="second"></span></div>
    <div class="row"><span class="label">Input type</span>
      <button data-cmd="c08301 c043" data-type="01">Optical</button>
      <button data-cmd="c08302 c043" data-type="02">Coax</button>
      <button data-cmd="c08304 c043" data-type="04">Analog</button>
    </div>
    <div class="note">Input type applies to the current input.</div>
  </div>
</details>

<details data-key="tuner">
  <summary>Tuner</summary>
  <div class="body">
    <div class="row">
      <span class="pair"><button data-cmd="c10300">FM</button><button data-cmd="c10301">AM</button></span>
      <span class="pair"><button data-cmd="c107">Scan &minus;</button><button data-cmd="c106">Scan +</button></span>
      <span class="pair"><button data-cmd="c109">Preset &minus;</button><button data-cmd="c108">Preset +</button></span>
      <span class="pair"><button data-cmd="c10a">Mono</button><button data-cmd="c10b">Stereo</button></span>
    </div>
    <div class="row"><span class="label">Preset</span>
      <select id="bank"><option value="01">Bank A</option><option value="02">Bank B</option><option value="03">Bank C</option></select>
      <span class="row" id="presets"></span>
    </div>
    <div class="row"><span class="label">Tune</span>
      <select id="tuneband"><option value="00">FM MHz</option><option value="01">AM kHz</option></select>
      <input type="number" id="freq" step="0.05" value="101.1">
      <button id="tune">Tune</button>
    </div>
    <div class="note" id="tunerstatus">No tuner status received yet.</div>
  </div>
</details>

<details data-key="surround">
  <summary>Surround</summary>
  <div class="body">
    <div class="row">
      <button data-cmd="c304">Surround</button>
      <button data-cmd="c305">2ch stereo</button>
      <button data-cmd="c30d">AFD</button>
      <button data-cmd="c30b">Next field</button>
      <span class="pair"><button data-cmd="c306">EQ on</button><button data-cmd="c307">EQ off</button></span>
    </div>
    <div class="row">
      <span class="label">Rear</span><span class="pair"><button data-cmd="c311">&minus;</button><button data-cmd="c310">+</button></span>
      <span class="label">Center</span><span class="pair"><button data-cmd="c313">&minus;</button><button data-cmd="c312">+</button></span>
      <span class="label">Sub</span><span class="pair"><button data-cmd="c315">&minus;</button><button data-cmd="c314">+</button></span>
    </div>
    <div class="row"><span class="label">Field #</span>
      <input type="number" id="field" min="0" max="28" value="0"><button id="setfield">Set field</button>
    </div>
    <div class="note" id="surroundstatus">Field numbers follow the order on the remote (0 = 2ch stereo, 1 = AFD).</div>
  </div>
</details>

<details data-key="settings">
  <summary>Speaker &amp; surround settings</summary>
  <div class="body" id="settings"></div>
</details>

<details data-key="rename">
  <summary>Rename an input</summary>
  <div class="body">
    <div class="row">
      <select id="renamecode"></select>
      <input id="renametext" maxlength="8" placeholder="8 characters max">
      <button id="rename">Save name</button>
    </div>
    <div class="note">Stored in the receiver; shows on its display and here.</div>
  </div>
</details>

<details data-key="log">
  <summary>Bus log &amp; raw commands</summary>
  <div class="body">
    <div class="row">
      <input id="raw" placeholder="hex, e.g. c00f" style="flex:1 1 160px">
      <button id="sendraw">Send</button>
      <button id="refresh">Re-read names &amp; status</button>
    </div>
    <pre id="log"></pre>
  </div>
</details>
</main>

<script>
const $ = s => document.querySelector(s);
const h2 = n => (((n % 256) + 256) % 256).toString(16).padStart(2, '0');
const DEFAULT_NAMES = {0:'TUNER',1:'PHONO',2:'CD',3:'DAT',4:'MD',5:'TAPE',16:'VIDEO 1',17:'VIDEO 2',18:'VIDEO 3',22:'TV/SAT',25:'DVD'};
const SECOND_CODES = [0, 2, 4, 5, 15];  // 03 (DAT) is the same input as 04 here
const TYPES = {'01':'Optical','02':'Coax','04':'Analog'};
let state = null, pendingInput = null, pendingSince = 0, logSeq = 0, inputsKey = '';

function nameOf(code) {
  if (code === 15) return 'Source';
  return (state && state.names && state.names[code]) || DEFAULT_NAMES[code] || ('0x' + h2(code));
}

async function send(hex) {
  try { await fetch('/api/send?hex=' + encodeURIComponent(hex), {method: 'POST'}); } catch (e) {}
  pollSoon(400);
}

document.addEventListener('click', e => {
  const b = e.target.closest('button[data-cmd]');
  if (b) send(b.dataset.cmd);
});

$('#mute').onclick = () => send(state && state.mute ? 'c007' : 'c006');
$('#multi').onclick = () => send(state && state.multi ? 'c00d' : 'c00c');
$('#sendraw').onclick = () => { const v = $('#raw').value.trim(); if (v) send(v); };
$('#raw').onkeydown = e => { if (e.key === 'Enter') $('#sendraw').click(); };
$('#refresh').onclick = () => fetch('/api/refresh');
$('#setfield').onclick = () => send('c351' + h2(+$('#field').value));
$('#tune').onclick = () => {
  const band = $('#tuneband').value, f = +$('#freq').value;
  // FM: hundredths of a MHz in 50 kHz steps; AM: kHz.
  const v = band === '00' ? Math.floor(Math.round(f * 100) / 5) * 5 : Math.round(f);
  send('c150' + band + h2(v >> 8) + h2(v & 255));
};
$('#tuneband').onchange = () => { $('#freq').step = $('#tuneband').value === '00' ? '0.05' : '1'; };
for (let i = 0; i < 10; i++) {
  const b = document.createElement('button');
  b.textContent = i;
  b.onclick = () => send('c151' + $('#bank').value + h2(i));
  $('#presets').appendChild(b);
}
$('#rename').onclick = () => {
  const code = +$('#renamecode').value;
  let text = $('#renametext').value.replace(/[^\x20-\x7e]/g, '').slice(0, 8).padEnd(8, ' ');
  const bytes = [...text].map(c => h2(c.charCodeAt(0))).join('');
  send('c088' + h2(code) + bytes + ' c048' + h2(code));
};

// Parameter settings from the boehmel.de S-Link page (surround processor, device c3):
// http://web.archive.org/web/20180831072659/http://boehmel.de/slink.htm
const signed = v => h2(v);                      // two's complement byte
const signMag = v => h2(v < 0 ? 0x80 | -v : v); // MSB = sign
const SETTINGS = [
  {name: 'Speaker size', cmd: '53', fields: [
    {opts: [['Large','00'],['Small','01']]},
    {opts: [['Front','00'],['Center','01'],['Rear','02'],['Sub','03']]}]},
  {name: 'Rear level (dB)', cmd: '80', fields: [{num: [-20, 10, 0], enc: signMag}]},
  {name: 'Center level (dB)', cmd: '81', fields: [{num: [-20, 10, 0], enc: signMag}]},
  {name: 'Front balance (-9 left .. 8 right)', cmd: '99', fields: [{num: [-9, 8, 0], enc: signed}]},
  {name: 'Rear balance (-9 left .. 8 right)', cmd: '83', fields: [{num: [-9, 8, 0], enc: signed}]},
  {name: 'Dynamic range comp. (dB)', cmd: '86', fields: [{num: [0, 25.5, 0], step: 0.1, enc: v => h2(Math.round(v * 10))}]},
  {name: 'LFE mix', cmd: '87', fields: [{opts: [['0 dB','00'],['-5 dB','fb'],['-10 dB','f6'],['-15 dB','f1'],['-20 dB','ec'],['Off','80']]}]},
  {name: 'dts LFE mix (dB)', cmd: '9f', fields: [{num: [-20, 0, 0], enc: signed}]},
  {name: 'Crossover', cmd: '97', fields: [{opts: [['60 Hz','00'],['90 Hz','4c'],['120 Hz','6a'],['150 Hz','88'],['180 Hz','a6']]}]},
  {name: 'Rear placement', cmd: '96', fields: [
    {opts: [['Side','00'],['Mid','01'],['Behind','02']]},
    {opts: [['Low','00'],['High','01']]}]},
  {name: 'Distance units', cmd: '95', fields: [{opts: [['Meters','01'],['Feet','02']]}]},
  {name: 'Front bass (query)', cmd: '90', fields: []},
];
for (const s of SETTINGS) {
  const row = document.createElement('div');
  row.className = 'setting';
  row.innerHTML = '<span class="name"></span>';
  row.querySelector('.name').textContent = s.name;
  const getters = s.fields.map(f => {
    let el;
    if (f.opts) {
      el = document.createElement('select');
      for (const [label, val] of f.opts) el.add(new Option(label, val));
      row.appendChild(el);
      return () => el.value;
    }
    el = document.createElement('input');
    el.type = 'number'; el.min = f.num[0]; el.max = f.num[1]; el.value = f.num[2]; el.step = f.step || 1;
    row.appendChild(el);
    return () => f.enc(+el.value);
  });
  const b = document.createElement('button');
  b.textContent = s.fields.length ? 'Set' : 'Query';
  b.onclick = () => send('c3' + s.cmd + getters.map(g => g()).join(''));
  row.appendChild(b);
  $('#settings').appendChild(row);
}

// Remember which sections are open.
document.querySelectorAll('details[data-key]').forEach(d => {
  try { d.open = localStorage.getItem('open-' + d.dataset.key) === '1'; } catch (e) {}
  d.addEventListener('toggle', () => { try { localStorage.setItem('open-' + d.dataset.key, d.open ? '1' : '0'); } catch (e) {} });
});

function render() {
  const s = state;
  $('#model').textContent = (s.model || 'Receiver').toUpperCase();

  if (pendingInput !== null && (s.audio === pendingInput || Date.now() - pendingSince > 4000)) pendingInput = null;
  $('#current').textContent = !s.known ? 'Waiting for receiver…' : (s.power ? nameOf(s.audio) : 'Standby');

  const chips = [];
  if (s.known) chips.push(s.power ? ['On', 'power-on'] : ['Standby', '']);
  if (s.mute) chips.push(['Muted', 'flag']);
  if (s.multi) chips.push(['5.1 input', 'flag']);
  if (s.tapeLoop) chips.push(['Tape monitor', 'flag']);
  const type = TYPES[(s.inputType || '').slice(-2)];
  if (type) chips.push([type, '']);
  if (s.second >= 0) chips.push(['Rec out: ' + nameOf(s.second), '']);
  $('#chips').innerHTML = '';
  for (const [text, cls] of chips) {
    const c = document.createElement('span');
    c.className = 'chip ' + cls; c.textContent = text;
    $('#chips').appendChild(c);
  }

  $('#mute').textContent = s.mute ? 'Unmute' : 'Mute';
  $('#mute').classList.toggle('active', s.mute);
  $('#multi').classList.toggle('active', s.multi);
  $('#pon').classList.toggle('active', s.known && s.power);
  $('#poff').classList.toggle('active', s.known && !s.power);

  const key = JSON.stringify(s.inputs.map(i => [i.code, nameOf(i.code)]));
  if (key !== inputsKey) {
    inputsKey = key;
    $('#inputs').innerHTML = '';
    $('#renamecode').innerHTML = '';
    for (const i of s.inputs) {
      const b = document.createElement('button');
      b.dataset.code = i.code;
      b.textContent = nameOf(i.code);
      b.onclick = () => { pendingInput = i.code; pendingSince = Date.now(); render(); send('c050' + h2(i.code)); };
      $('#inputs').appendChild(b);
      $('#renamecode').add(new Option(nameOf(i.code), i.code));
    }
    $('#second').innerHTML = '';
    for (const code of SECOND_CODES) {
      const b = document.createElement('button');
      b.dataset.code = code;
      b.textContent = nameOf(code);
      b.onclick = () => send('c052' + h2(code) + ' c00e');
      $('#second').appendChild(b);
    }
  }
  document.querySelectorAll('#inputs button').forEach(b => {
    const code = +b.dataset.code;
    b.classList.toggle('active', s.power && code === s.audio && pendingInput === null);
    b.classList.toggle('pending', code === pendingInput);
  });
  document.querySelectorAll('#second button').forEach(b => b.classList.toggle('active', +b.dataset.code === s.second));
  document.querySelectorAll('button[data-type]').forEach(b => b.classList.toggle('active', (s.inputType || '').endsWith(b.dataset.type)));

  $('#tunerstatus').textContent = s.tuner ? 'Last tuner status: ' + s.tuner + (s.tunerName ? ' (' + s.tunerName + ')' : '') : 'No tuner status received yet.';
}

// One polling chain: the next poll is scheduled only after the current one
// finishes, so slow requests can't pile up. pollSoon() (after a button press)
// moves the single pending timer earlier instead of starting another chain; if
// a request is in flight, it just asks for a quick follow-up. The timeout keeps
// one hung request from stopping polling for good.
let pollTimer = 0, polling = false, pollAgain = false;
function pollSoon(ms) {
  if (polling) { pollAgain = true; return; }
  clearTimeout(pollTimer);
  pollTimer = setTimeout(poll, ms);
}
async function poll() {
  polling = true;
  try {
    const r = await fetch('/api/state', {cache: 'no-store', signal: AbortSignal.timeout(5000)});
    state = await r.json();
    $('#dot').classList.add('ok');
    $('#linktext').textContent = state.ageMs >= 0 && state.ageMs < 60000 ? 'live' : 'no recent data';
    render();
    if (state.logSeq !== logSeq) {
      const t = await (await fetch('/api/log?since=' + logSeq, {signal: AbortSignal.timeout(5000)})).text();
      logSeq = state.logSeq;
      const pre = $('#log');
      pre.textContent = (pre.textContent + t).split('\n').slice(-150).join('\n');
      pre.scrollTop = pre.scrollHeight;
    }
  } catch (e) {
    $('#dot').classList.remove('ok');
    $('#linktext').textContent = 'offline';
  } finally {
    polling = false;
    pollSoon(pollAgain ? 400 : 1000);
    pollAgain = false;
  }
}
poll();
</script>
</body>
</html>
)PAGE";
