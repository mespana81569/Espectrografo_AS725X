#pragma once
#include <pgmspace.h>

static const char HTML_CONTENT[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Spectrograph AS7265X</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  :root{--bg:#0f172a;--card:#1e293b;--accent:#38bdf8;--accent2:#a78bfa;--accent3:#34d399;--text:#e2e8f0;--muted:#94a3b8;--danger:#f87171;--ok:#4ade80}
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;padding:1rem}
  h1{text-align:center;color:var(--accent);font-size:1.4rem;margin-bottom:1rem;letter-spacing:.05em}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:1rem;max-width:900px;margin:0 auto}
  @media(max-width:600px){.grid{grid-template-columns:1fr}}
  .card{background:var(--card);border-radius:.75rem;padding:1rem;border:1px solid #334155}
  .card h2{font-size:.85rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:.75rem}
  label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:.15rem;margin-top:.5rem}
  select,input[type=number]{width:100%;padding:.4rem .6rem;background:#0f172a;border:1px solid #334155;border-radius:.4rem;color:var(--text);font-size:.9rem}
  button{width:100%;padding:.55rem;margin-top:.5rem;border:none;border-radius:.4rem;cursor:pointer;font-size:.85rem;font-weight:600;transition:opacity .15s}
  button:hover{opacity:.85} button:disabled{opacity:.4;cursor:not-allowed}
  .btn-primary{background:var(--accent);color:#0f172a}
  .btn-success{background:var(--ok);color:#0f172a}
  .btn-danger{background:var(--danger);color:#0f172a}
  .btn-purple{background:var(--accent2);color:#0f172a}
  .status-badge{display:inline-block;padding:.2rem .6rem;border-radius:999px;font-size:.75rem;font-weight:700;background:#334155;color:var(--accent);letter-spacing:.05em}
  #stateDisplay{font-size:1.1rem;font-weight:700;color:var(--accent);text-align:center;margin:.5rem 0}
  .row{display:flex;gap:.5rem;align-items:center;margin-bottom:.3rem}
  .dot{width:.6rem;height:.6rem;border-radius:50%;background:#334155;flex-shrink:0}
  .dot.ok{background:var(--ok)} .dot.err{background:var(--danger)}
  #chartWrapper{grid-column:1/-1}
  canvas{max-height:320px}
  #log{font-size:.72rem;color:var(--muted);height:80px;overflow-y:auto;background:#0f172a;border-radius:.4rem;padding:.4rem;border:1px solid #334155;margin-top:.5rem}
  .progress{height:.4rem;background:#334155;border-radius:.2rem;overflow:hidden;margin-top:.3rem}
  .progress-bar{height:100%;background:var(--accent);width:0%;transition:width .3s}
</style>
</head>
<body>
<h1>Portable Spectrograph &mdash; AS7265X</h1>
<div class="grid">

  <!-- Status Card -->
  <div class="card">
    <h2>System Status</h2>
    <div id="stateDisplay">IDLE</div>
    <div class="row"><span class="dot" id="dotSensor"></span><span id="lblSensor">Sensor: checking...</span></div>
    <div class="row"><span class="dot" id="dotSD"></span><span id="lblSD">SD Card: checking...</span></div>
    <div class="row"><span class="dot" id="dotCal"></span><span id="lblCal">Calibration: not done</span></div>
    <div class="progress"><div class="progress-bar" id="measProgress"></div></div>
    <div id="measProgressLbl" style="font-size:.75rem;color:var(--muted);margin-top:.2rem">0 / 0</div>
    <div id="log"></div>
  </div>

  <!-- Config Card -->
  <div class="card">
    <h2>Configuration</h2>
    <label>Experiment ID</label>
    <input type="text" id="expId" value="EXP_001" style="width:100%;padding:.4rem .6rem;background:#0f172a;border:1px solid #334155;border-radius:.4rem;color:var(--text);font-size:.9rem"/>
    <label>Gain</label>
    <select id="gain">
      <option value="0">1x</option>
      <option value="1">4x</option>
      <option value="2" selected>16x</option>
      <option value="3">64x</option>
    </select>
    <label>Integration Cycles (1 cycle = 2.8 ms)</label>
    <input type="number" id="intCycles" value="50" min="1" max="255"/>
    <label>Measurement Count (N, max 20)</label>
    <input type="number" id="measN" value="5" min="1" max="20"/>
    <label>LED Current (mA)</label>
    <select id="ledCurrent">
      <option value="12" selected>12.5 mA</option>
      <option value="25">25 mA</option>
      <option value="50">50 mA</option>
      <option value="100">100 mA</option>
    </select>
    <button class="btn-primary" onclick="applyConfig()">Apply Config</button>
  </div>

  <!-- Control Card -->
  <div class="card">
    <h2>Control</h2>
    <button class="btn-purple" id="btnCal" onclick="startCalibration()">1. Start Calibration (blank reference)</button>
    <button class="btn-primary" id="btnConfirm" onclick="confirmSample()" disabled>2. Insert Sample &amp; Confirm</button>
    <button class="btn-success" id="btnMeasure" onclick="startMeasure()" disabled>3. Start Measurement</button>
    <button class="btn-primary" id="btnAccept" onclick="acceptValidation()" disabled>4. Accept &amp; Proceed to Save</button>
    <button class="btn-success" id="btnSave" onclick="saveData()" disabled>5. Save to SD Card</button>
    <button class="btn-danger" id="btnDiscard" onclick="discardData()" disabled>Discard</button>
  </div>

  <!-- Chart Card -->
  <div class="card" id="chartWrapper">
    <h2>Spectral Overlay (last 3 measurements)</h2>
    <canvas id="spectraChart"></canvas>
  </div>

</div>

<script>
// ─── State & Chart ────────────────────────────────────────────────────────────
const COLORS = ['#38bdf8','#a78bfa','#34d399','#fb923c','#f472b6'];
let chart = null;
let currentState = 'IDLE';

function initChart() {
  const ctx = document.getElementById('spectraChart').getContext('2d');
  chart = new Chart(ctx, {
    type: 'line',
    data: { labels: [], datasets: [] },
    options: {
      responsive: true,
      animation: false,
      plugins: {
        legend: { labels: { color: '#e2e8f0', font:{size:11} } }
      },
      scales: {
        x: { ticks:{color:'#94a3b8'}, grid:{color:'#1e293b'}, title:{display:true,text:'Wavelength (nm)',color:'#94a3b8'} },
        y: { ticks:{color:'#94a3b8'}, grid:{color:'#1e293b'}, title:{display:true,text:'Calibrated Value',color:'#94a3b8'}, beginAtZero:true }
      }
    }
  });
}

function updateChart(data) {
  if (!chart || !data.spectra || data.spectra.length === 0) return;
  chart.data.labels = data.wavelengths;
  // Show last 3
  const spectra = data.spectra.slice(-3);
  chart.data.datasets = spectra.map((sp, i) => ({
    label: `Meas ${data.count - spectra.length + i + 1}`,
    data: sp,
    borderColor: COLORS[i % COLORS.length],
    backgroundColor: 'transparent',
    borderWidth: 2,
    pointRadius: 3,
    tension: 0.3
  }));
  chart.update('none');
}

// ─── Logging ──────────────────────────────────────────────────────────────────
function log(msg) {
  const el = document.getElementById('log');
  const t = new Date().toLocaleTimeString();
  el.innerHTML += `<div>[${t}] ${msg}</div>`;
  el.scrollTop = el.scrollHeight;
}

// ─── API Calls ────────────────────────────────────────────────────────────────
async function api(path, method='GET', body=null) {
  try {
    const opts = { method };
    if (body) { opts.headers={'Content-Type':'application/json'}; opts.body=JSON.stringify(body); }
    const r = await fetch(path, opts);
    return await r.json();
  } catch(e) { log('Network error: '+e.message); return null; }
}

async function applyConfig() {
  const cfg = {
    gain: parseInt(document.getElementById('gain').value),
    integrationCycles: parseInt(document.getElementById('intCycles').value),
    mode: 3,
    ledCurrent: parseInt(document.getElementById('ledCurrent').value),
    ledEnabled: true,
    N: parseInt(document.getElementById('measN').value),
    expId: document.getElementById('expId').value.trim() || 'EXP_001'
  };
  const r = await api('/api/config', 'POST', cfg);
  if (r) log('Config applied: '+JSON.stringify(r));
}

async function startCalibration() {
  const r = await api('/api/calibrate', 'POST');
  if (r) log('Calibration: '+JSON.stringify(r));
}

async function confirmSample() {
  const r = await api('/api/confirm', 'POST');
  if (r) log('Confirmed: '+JSON.stringify(r));
}

async function startMeasure() {
  const r = await api('/api/measure', 'POST');
  if (r) log('Measure: '+JSON.stringify(r));
}

async function acceptValidation() {
  const r = await api('/api/accept', 'POST');
  if (r) log('Accepted: '+JSON.stringify(r));
}

async function saveData() {
  const r = await api('/api/save', 'POST');
  if (r) log('Save: '+JSON.stringify(r));
}

async function discardData() {
  const r = await api('/api/discard', 'POST');
  if (r) log('Discard: '+JSON.stringify(r));
}

// ─── Polling ──────────────────────────────────────────────────────────────────
function updateButtons(state) {
  const btns = {
    btnCal:     state === 'IDLE',
    btnConfirm: state === 'WAIT_CONFIRMATION',
    btnMeasure: state === 'WAIT_CONFIRMATION' || state === 'IDLE',
    btnAccept:  state === 'VALIDATION',
    btnSave:    state === 'SAVE_DECISION',
    btnDiscard: state === 'SAVE_DECISION'
  };
  for (const [id, en] of Object.entries(btns)) {
    document.getElementById(id).disabled = !en;
  }
}

async function pollStatus() {
  const s = await api('/api/status');
  if (!s) return;

  document.getElementById('stateDisplay').textContent = s.state;
  currentState = s.state;

  const dot = (id, ok) => { document.getElementById(id).className = 'dot ' + (ok?'ok':'err'); };
  dot('dotSensor', s.sensorReady);
  dot('dotSD', s.sdReady);
  dot('dotCal', s.calValid);
  document.getElementById('lblSensor').textContent = 'Sensor: ' + (s.sensorReady?'ready':'not found');
  document.getElementById('lblSD').textContent = 'SD Card: ' + (s.sdReady?'ready':'not present');
  document.getElementById('lblCal').textContent = 'Calibration: ' + (s.calValid?'valid':'not done');

  const pct = s.measTarget > 0 ? (s.measCount / s.measTarget * 100) : 0;
  document.getElementById('measProgress').style.width = pct + '%';
  document.getElementById('measProgressLbl').textContent = s.measCount + ' / ' + s.measTarget;

  updateButtons(s.state);
}

async function pollSpectra() {
  const d = await api('/api/spectra');
  if (d && d.count > 0) updateChart(d);
}

// ─── Init ─────────────────────────────────────────────────────────────────────
window.addEventListener('load', () => {
  initChart();
  log('UI ready');
  setInterval(pollStatus,  1500);
  setInterval(pollSpectra, 3000);
  pollStatus();
});
</script>
</body>
</html>
)rawhtml";
