#pragma once
#include <pgmspace.h>

static const char HTML_CONTENT[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Spectrograph AS7265X</title>
<style>
  :root{--bg:#0f172a;--card:#1e293b;--bdr:#334155;--accent:#38bdf8;--accent2:#a78bfa;
    --ok:#4ade80;--danger:#f87171;--warn:#fbbf24;--text:#e2e8f0;--muted:#94a3b8}
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);
    min-height:100vh;padding:.75rem}
  h1{text-align:center;color:var(--accent);font-size:1.3rem;margin-bottom:.6rem;letter-spacing:.05em}

  /* Pipeline */
  .pipe{display:flex;align-items:center;justify-content:center;gap:0;margin-bottom:.75rem;flex-wrap:wrap}
  .stp{padding:.3rem .55rem;font-size:.65rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em;
    background:var(--bdr);color:var(--muted);border-radius:.3rem;white-space:nowrap;transition:all .3s}
  .stp.on{background:var(--accent);color:var(--bg);animation:pulse 1.5s ease-in-out infinite}
  .stp.ok{background:var(--ok);color:var(--bg)}
  .arr{color:var(--bdr);font-size:.85rem;padding:0 .15rem}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.65}}

  /* Grid */
  .g{display:grid;grid-template-columns:1fr 1fr;gap:.7rem;max-width:900px;margin:0 auto}
  @media(max-width:600px){.g{grid-template-columns:1fr}}
  .c{background:var(--card);border-radius:.7rem;padding:.8rem;border:1px solid var(--bdr)}
  .c h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:.45rem}
  .full{grid-column:1/-1}

  /* Form */
  label{display:block;font-size:.72rem;color:var(--muted);margin:.35rem 0 .1rem}
  select,input{width:100%;padding:.32rem .45rem;background:var(--bg);border:1px solid var(--bdr);
    border-radius:.3rem;color:var(--text);font-size:.82rem}
  button{width:100%;padding:.45rem;margin-top:.35rem;border:none;border-radius:.3rem;
    cursor:pointer;font-size:.78rem;font-weight:600;transition:all .15s}
  button:hover:not(:disabled){opacity:.85}
  button:disabled{opacity:.3;cursor:not-allowed}
  .bA{background:var(--accent);color:var(--bg)}
  .bG{background:var(--ok);color:var(--bg)}
  .bR{background:var(--danger);color:var(--bg)}
  .bP{background:var(--accent2);color:var(--bg)}

  /* Status */
  .row{display:flex;gap:.35rem;align-items:center;margin-bottom:.2rem;font-size:.8rem}
  .d{width:.5rem;height:.5rem;border-radius:50%;background:var(--bdr);flex-shrink:0}
  .d.ok{background:var(--ok)}.d.er{background:var(--danger)}.d.wa{background:var(--warn)}
  .bar{height:.3rem;background:var(--bdr);border-radius:.15rem;overflow:hidden;margin-top:.25rem}
  .bar>div{height:100%;background:var(--accent);width:0%;transition:width .3s}

  /* Log */
  #log{font-size:.65rem;color:var(--muted);height:65px;overflow-y:auto;background:var(--bg);
    border-radius:.3rem;padding:.3rem;border:1px solid var(--bdr);margin-top:.35rem;font-family:monospace}

  /* Canvas */
  .cv{width:100%;height:240px;display:block}
  .hidden{display:none!important}
</style>
</head>
<body>
<h1>Portable Spectrograph &mdash; AS7265X</h1>

<div class="pipe" id="pipe">
  <div class="stp on" data-s="IDLE">1. Idle</div><div class="arr">&rsaquo;</div>
  <div class="stp" data-s="CALIBRATION">2. Calibrating</div><div class="arr">&rsaquo;</div>
  <div class="stp" data-s="WAIT_CONFIRMATION">3. Confirm</div><div class="arr">&rsaquo;</div>
  <div class="stp" data-s="MEASUREMENT">4. Measuring</div><div class="arr">&rsaquo;</div>
  <div class="stp" data-s="VALIDATION">5. Validate</div><div class="arr">&rsaquo;</div>
  <div class="stp" data-s="SAVE_DECISION">6. Save</div>
</div>

<div class="g">
  <!-- Status + Control -->
  <div class="c">
    <h2>Status</h2>
    <div class="row"><span class="d" id="dS"></span><span id="lS">Sensor: --</span></div>
    <div class="row"><span class="d" id="dD"></span><span id="lD">SD: --</span></div>
    <div class="row"><span class="d" id="dC"></span><span id="lC">Cal: --</span></div>
    <div class="bar"><div id="prog"></div></div>
    <div id="progL" style="font-size:.68rem;color:var(--muted);margin-top:.12rem">0 / 0</div>

    <h2 style="margin-top:.5rem">Control</h2>
    <button class="bP" id="b1" onclick="act('calibrate')">1. Calibrate (blank reference)</button>
    <button class="bA" id="b2" onclick="act('confirm')" disabled>2. Sample Inserted &mdash; Confirm</button>
    <button class="bG" id="b3" onclick="act('measure')" disabled>3. Start Measurement</button>
    <button class="bA" id="b4" onclick="act('accept')" disabled>4. Accept Results</button>
    <button class="bG" id="b5" onclick="act('save')" disabled>5. Save to SD</button>
    <button class="bR" id="b6" onclick="act('discard')" disabled>Discard</button>

    <h2 style="margin-top:.5rem">Diagnostics</h2>
    <button class="bA" id="bMon" onclick="monStart()">Live Monitor</button>
    <button class="bR" id="bMonStop" onclick="monStop()" disabled>Stop Monitor</button>
    <div id="log"></div>
  </div>

  <!-- Config + WiFi -->
  <div class="c">
    <h2>Configuration</h2>
    <label>Experiment ID</label>
    <input type="text" id="expId" value="EXP_001"/>
    <label>Gain</label>
    <select id="gain"><option value="0">1x</option><option value="1">4x</option>
      <option value="2" selected>16x</option><option value="3">64x</option></select>
    <label>Integration (cycles, 1 = 2.8 ms)</label>
    <input type="number" id="intCycles" value="50" min="1" max="255"/>
    <label>Measurements (N, max 20)</label>
    <input type="number" id="measN" value="5" min="1" max="20"/>
    <h2 style="margin-top:.5rem">LED Bulbs</h2>
    <div class="row"><input type="checkbox" id="ledW" style="width:auto"><label style="margin:0;display:inline">White (VIS)</label>
      <select id="ledWmA" style="width:5rem;margin-left:auto"><option value="12" selected>12.5</option><option value="25">25</option><option value="50">50</option><option value="100">100</option></select><span style="font-size:.7rem;color:var(--muted)">mA</span></div>
    <div class="row"><input type="checkbox" id="ledI" style="width:auto"><label style="margin:0;display:inline">IR (NIR)</label>
      <select id="ledImA" style="width:5rem;margin-left:auto"><option value="12" selected>12.5</option><option value="25">25</option><option value="50">50</option><option value="100">100</option></select><span style="font-size:.7rem;color:var(--muted)">mA</span></div>
    <div class="row"><input type="checkbox" id="ledU" style="width:auto"><label style="margin:0;display:inline">UV</label>
      <select id="ledUmA" style="width:5rem;margin-left:auto"><option value="12" selected>12.5</option><option value="25">25</option><option value="50">50</option><option value="100">100</option></select><span style="font-size:.7rem;color:var(--muted)">mA</span></div>
    <button class="bA" onclick="applyCfg()">Apply Config</button>

    <h2 style="margin-top:.7rem">WiFi</h2>
    <div class="row"><span class="d" id="dW"></span><span id="lW">--</span></div>
    <label>Network</label>
    <div class="row" style="gap:.3rem">
      <select id="wSSID" style="flex:1"><option value="">-- Scan first --</option></select>
      <button class="bA" style="width:auto;padding:.32rem .6rem;margin:0" onclick="scanWifi()" id="bScan">Scan</button>
    </div>
    <label>Password</label>
    <input type="password" id="wPASS" placeholder="Password"/>
    <button class="bA" onclick="connectWifi()">Connect</button>
  </div>

  <!-- Spectra chart -->
  <div class="c full" id="chartCard">
    <h2>Spectra (last 3 measurements)</h2>
    <canvas id="chart" class="cv" height="240"></canvas>
  </div>

  <!-- Live Monitor chart (separate) -->
  <div class="c full hidden" id="liveCard">
    <h2>Live Monitor (real-time raw data)</h2>
    <canvas id="liveChart" class="cv" height="240"></canvas>
  </div>
</div>

<script>
/* ─── Zero-dependency JS ────────────────────────────────────────────────── */
var ST=['IDLE','CALIBRATION','WAIT_CONFIRMATION','MEASUREMENT','VALIDATION','SAVE_DECISION'];
var cur='IDLE';

function lg(m){
  var el=document.getElementById('log');
  if(!el)return;
  var t=new Date().toLocaleTimeString();
  el.innerHTML+='<div>['+t+'] '+m+'</div>';
  el.scrollTop=el.scrollHeight;
}

function api(path,method,body){
  return new Promise(function(ok){
    try{
      var o={method:method||'GET'};
      if(body){o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(body);}
      fetch(path,o).then(function(r){return r.json();}).then(ok).catch(function(e){
        lg('ERR: '+e.message);ok(null);
      });
    }catch(e){lg('ERR: '+e.message);ok(null);}
  });
}

/* ─── Pipeline ──────────────────────────────────────────────────────────── */
function updPipe(state){
  var steps=document.querySelectorAll('.stp');
  var idx=-1;
  for(var i=0;i<ST.length;i++){if(ST[i]===state)idx=i;}
  for(var i=0;i<steps.length;i++){
    steps[i].classList.remove('on','ok');
    if(i<idx)steps[i].classList.add('ok');
    else if(i===idx)steps[i].classList.add('on');
  }
}

/* ─── Buttons ───────────────────────────────────────────────────────────── */
function updBtns(s){
  var idle=s==='IDLE';
  var live=s==='LIVE_MONITOR';
  document.getElementById('b1').disabled      = !idle;
  document.getElementById('b2').disabled      = s!=='WAIT_CONFIRMATION';
  document.getElementById('b3').disabled      = s!=='WAIT_CONFIRMATION'&&!idle;
  document.getElementById('b4').disabled      = s!=='VALIDATION';
  document.getElementById('b5').disabled      = s!=='SAVE_DECISION';
  document.getElementById('b6').disabled      = s!=='SAVE_DECISION';
  document.getElementById('bMon').disabled    = !idle;
  document.getElementById('bMonStop').disabled= !live;
  // Toggle chart sections
  document.getElementById('chartCard').classList.toggle('hidden',live);
  document.getElementById('liveCard').classList.toggle('hidden',!live);
}

/* ─── Actions ───────────────────────────────────────────────────────────── */
function act(a){
  api('/api/'+a,'POST').then(function(r){
    if(r)lg(a+': '+(r.status||r.error||'ok'));
    // After a successful save, auto-increment experiment ID in the UI
    if(a==='save' && r && r.status==='saved'){
      var el=document.getElementById('expId');
      var v=el.value.trim();
      // Try to increment trailing number: EXP_001 → EXP_002, Test3 → Test4
      var m=v.match(/^(.*?)(\d+)$/);
      if(m){
        var num=parseInt(m[2])+1;
        var pad=m[2].length;
        var ns=String(num);
        while(ns.length<pad)ns='0'+ns;
        el.value=m[1]+ns;
      } else {
        el.value=v+'_2';
      }
      lg('Next experiment: '+el.value);
    }
    poll();
  });
}

function applyCfg(){
  var cfg={
    gain:parseInt(document.getElementById('gain').value),
    integrationCycles:parseInt(document.getElementById('intCycles').value),
    mode:3,
    ledWhiteCurrent:parseInt(document.getElementById('ledWmA').value),
    ledIrCurrent:parseInt(document.getElementById('ledImA').value),
    ledUvCurrent:parseInt(document.getElementById('ledUmA').value),
    ledWhiteEnabled:document.getElementById('ledW').checked,
    ledIrEnabled:document.getElementById('ledI').checked,
    ledUvEnabled:document.getElementById('ledU').checked,
    N:parseInt(document.getElementById('measN').value),
    expId:document.getElementById('expId').value.trim()||'EXP_001'
  };
  api('/api/config','POST',cfg).then(function(r){
    if(r)lg('Config: '+(r.status||r.error));
  });
}

function scanWifi(){
  var btn=document.getElementById('bScan');
  btn.disabled=true;btn.textContent='...';
  lg('WiFi: scan starting (AP will drop briefly)...');
  // POST triggers the scan — AP will go down, phone will disconnect
  api('/api/wifi/scan','POST').then(function(){});
  // AP is going down — we'll lose connection for ~10 seconds
  lg('WiFi: AP offline for ~10s while scanning...');
  // Poll for results after AP comes back (3s settle + 4s scan + 2s AP restore)
  var attempts=0;
  setTimeout(function pollScan(){
    attempts++;
    api('/api/wifi/scan').then(function(r){
      if(!r||r.scanning){
        // AP not back yet or still scanning — retry (up to 25 attempts = ~75s)
        if(attempts<25){setTimeout(pollScan,3000);return;}
        lg('WiFi: scan timeout — try again');
        btn.disabled=false;btn.textContent='Scan';
        return;
      }
      populateNetworks(r.networks);
      btn.disabled=false;btn.textContent='Scan';
    });
  },10000);  // first poll after 10s
}

function populateNetworks(nets){
  var sel=document.getElementById('wSSID');
  sel.innerHTML='';
  if(!nets||nets.length===0){
    sel.innerHTML='<option value="">No networks found</option>';
    lg('WiFi: no networks found');
    return;
  }
  // Sort by signal strength (already sorted by ESP32, but ensure)
  nets.sort(function(a,b){return b.rssi-a.rssi;});
  for(var i=0;i<nets.length;i++){
    var o=document.createElement('option');
    o.value=nets[i].ssid;
    o.textContent=nets[i].ssid+' ('+nets[i].rssi+' dBm)'+(nets[i].enc?'':' [open]');
    sel.appendChild(o);
  }
  lg('WiFi: found '+nets.length+' networks');
}

function connectWifi(){
  var ssid=document.getElementById('wSSID').value;
  var pass=document.getElementById('wPASS').value;
  if(!ssid){lg('SSID required — press Scan first');return;}
  lg('WiFi: connecting to '+ssid+'...');
  api('/api/wifi','POST',{ssid:ssid,password:pass}).then(function(r){
    if(r)lg('WiFi: '+(r.status||r.error));
  });
}

/* ─── Status Poll ───────────────────────────────────────────────────────── */
function poll(){
  api('/api/status').then(function(s){
    if(!s)return;
    cur=s.state;
    updPipe(s.state);
    updBtns(s.state);

    var sd=function(id,ok){document.getElementById(id).className='d '+(ok?'ok':'er');};
    sd('dS',s.sensorReady);sd('dD',s.sdReady);sd('dC',s.calValid);
    document.getElementById('lS').textContent='Sensor: '+(s.sensorReady?'OK':'--');
    document.getElementById('lD').textContent='SD: '+(s.sdReady?'OK':'--');
    document.getElementById('lC').textContent='Cal: '+(s.calValid?'Valid':'--');

    var pct=s.measTarget>0?(s.measCount/s.measTarget*100):0;
    document.getElementById('prog').style.width=pct+'%';
    document.getElementById('progL').textContent=s.measCount+' / '+s.measTarget;
  });
}

/* ─── Canvas Chart (no library) ─────────────────────────────────────────── */
function drawChart(data){
  var cv=document.getElementById('chart');
  if(!cv)return;
  var ctx=cv.getContext('2d');
  var rect=cv.getBoundingClientRect();
  var dpr=window.devicePixelRatio||1;
  cv.width=rect.width*dpr;cv.height=rect.height*dpr;
  ctx.scale(dpr,dpr);
  var W=rect.width,H=rect.height;
  ctx.clearRect(0,0,W,H);

  var sp=data.spectra.slice(-3);
  var wl=data.wavelengths;
  if(!sp.length||!wl.length)return;

  var mx=0;
  for(var i=0;i<sp.length;i++)for(var j=0;j<sp[i].length;j++)if(sp[i][j]>mx)mx=sp[i][j];
  if(mx===0)mx=1;

  var L=48,R=12,T=18,B=26;
  var pW=W-L-R,pH=H-T-B;

  /* grid */
  ctx.strokeStyle='#334155';ctx.lineWidth=0.5;
  for(var i=0;i<=4;i++){
    var y=T+pH*(1-i/4);
    ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(W-R,y);ctx.stroke();
  }

  /* axis labels */
  ctx.fillStyle='#94a3b8';ctx.font='9px sans-serif';ctx.textAlign='center';
  for(var i=0;i<wl.length;i+=3){
    var x=L+(i/(wl.length-1))*pW;
    ctx.fillText(wl[i],x,H-4);
  }
  ctx.textAlign='right';
  for(var i=0;i<=4;i++){
    var y=T+pH*(1-i/4);
    ctx.fillText((mx*i/4).toFixed(1),L-4,y+3);
  }

  /* lines + dots */
  var clr=['#38bdf8','#a78bfa','#34d399'];
  for(var s=0;s<sp.length;s++){
    ctx.strokeStyle=clr[s%3];ctx.lineWidth=2;ctx.beginPath();
    for(var i=0;i<sp[s].length;i++){
      var x=L+(i/(sp[s].length-1))*pW;
      var y=T+pH*(1-sp[s][i]/mx);
      if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
    }
    ctx.stroke();
    ctx.fillStyle=clr[s%3];
    for(var i=0;i<sp[s].length;i++){
      var x=L+(i/(sp[s].length-1))*pW;
      var y=T+pH*(1-sp[s][i]/mx);
      ctx.beginPath();ctx.arc(x,y,2,0,6.28);ctx.fill();
    }
  }

  /* legend */
  ctx.font='10px sans-serif';ctx.textAlign='left';
  for(var s=0;s<sp.length;s++){
    var lx=L+4+s*80;
    ctx.fillStyle=clr[s%3];
    ctx.fillRect(lx,T+1,10,3);
    ctx.fillText('Meas '+(data.count-sp.length+s+1),lx+14,T+7);
  }
}

/* ─── Live Monitor ─────────────────────────────────────────────────────── */
function monStart(){
  api('/api/monitor/start','POST').then(function(r){
    if(r)lg('Monitor: '+(r.status||r.error));
    poll();
  });
}
function monStop(){
  api('/api/monitor/stop','POST').then(function(r){
    if(r)lg('Monitor: '+(r.status||r.error));
    poll();
  });
}

/* wavelength→colour for bar chart */
function wlClr(nm){
  if(nm<450)return'#8b5cf6';if(nm<500)return'#3b82f6';if(nm<560)return'#22c55e';
  if(nm<600)return'#eab308';if(nm<650)return'#f97316';if(nm<750)return'#ef4444';
  return'#991b1b';
}

function drawLive(data){
  var cv=document.getElementById('liveChart');
  if(!cv)return;
  var ctx=cv.getContext('2d');
  var rect=cv.getBoundingClientRect();
  var dpr=window.devicePixelRatio||1;
  cv.width=rect.width*dpr;cv.height=rect.height*dpr;
  ctx.scale(dpr,dpr);
  var W=rect.width,H=rect.height;
  ctx.clearRect(0,0,W,H);

  var ch=data.ch, wl=data.wl;
  if(!ch||!wl||ch.length!==18)return;

  var mx=0;
  for(var i=0;i<ch.length;i++)if(ch[i]>mx)mx=ch[i];
  if(mx===0)mx=1;

  var L=48,R=12,T=18,B=30;
  var pW=W-L-R,pH=H-T-B;
  var bw=pW/ch.length*0.7;
  var gap=pW/ch.length;

  /* grid */
  ctx.strokeStyle='#334155';ctx.lineWidth=0.5;
  for(var i=0;i<=4;i++){
    var y=T+pH*(1-i/4);
    ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(W-R,y);ctx.stroke();
  }

  /* Y labels */
  ctx.fillStyle='#94a3b8';ctx.font='9px sans-serif';ctx.textAlign='right';
  for(var i=0;i<=4;i++){
    var y=T+pH*(1-i/4);
    ctx.fillText((mx*i/4).toFixed(1),L-4,y+3);
  }

  /* bars + X labels */
  var names='ABCDEFGHIJKLRSTUVW';
  ctx.textAlign='center';
  for(var i=0;i<ch.length;i++){
    var x=L+gap*i+gap/2-bw/2;
    var h=ch[i]/mx*pH;
    ctx.fillStyle=wlClr(wl[i]);
    ctx.fillRect(x,T+pH-h,bw,h);
    /* label */
    ctx.fillStyle='#94a3b8';ctx.font='8px sans-serif';
    ctx.fillText(wl[i],L+gap*i+gap/2,H-14);
    ctx.fillText(names[i],L+gap*i+gap/2,H-4);
  }
}

function monitorLoop(){
  if(cur!=='LIVE_MONITOR'){setTimeout(monitorLoop,500);return;}
  api('/api/monitor').then(function(d){
    if(d&&d.ready)drawLive(d);
  });
  setTimeout(monitorLoop,600);
}

/* ─── Self-scheduling loops (never overlap) ─────────────────────────────── */
function statusLoop(){
  poll();
  var fast=(cur==='CALIBRATION'||cur==='MEASUREMENT'||cur==='LIVE_MONITOR');
  setTimeout(statusLoop,fast?800:1500);
}

function spectraLoop(){
  api('/api/spectra').then(function(d){
    if(d&&d.count>0)drawChart(d);
  });
  setTimeout(spectraLoop,3000);
}

var lastWifiSt='';
function wifiLoop(){
  api('/api/wifi').then(function(r){
    if(!r)return;
    var st=r.status||'';
    var ok=st.indexOf('connected:')===0;
    var cls=ok?'ok':(st==='connecting'?'wa':'er');
    if(st==='not_configured')cls='';
    document.getElementById('dW').className='d '+cls;
    document.getElementById('lW').textContent=ok?st.replace('connected:','IP: '):st;
    if(st!==lastWifiSt){
      if(ok)lg('WiFi connected: '+st.replace('connected:',''));
      else if(st==='failed')lg('WiFi connection failed');
      else if(st==='ssid_not_found')lg('WiFi SSID not found');
      else if(st==='connecting')lg('WiFi connecting...');
      lastWifiSt=st;
    }
  });
  setTimeout(wifiLoop,3000);
}

/* ─── Boot ──────────────────────────────────────────────────────────────── */
lg('UI ready (standalone, no CDN)');
statusLoop();
setTimeout(spectraLoop,400);
setTimeout(wifiLoop,900);
setTimeout(monitorLoop,600);
</script>
</body>
</html>
)rawhtml";
