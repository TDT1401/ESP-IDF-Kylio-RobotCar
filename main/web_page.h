#pragma once

inline constexpr char kWebPage[] = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <meta name="theme-color" content="#eef4ff" id="themeColor">
  <title>Kylio Robot Car</title>
  <style>
    :root{--bg:#eef4ff;--card:#fffffff0;--text:#172033;--muted:#68758d;--line:#dce5f2;--blue:#356df3;--blue2:#2458d5;--green:#16a56a;--red:#ef4e5b;--shadow:0 18px 50px #25438221}
    body.dark{--bg:#0c1220;--card:#141d30f2;--text:#edf3ff;--muted:#9eabc1;--line:#273550;--blue:#5d88ff;--blue2:#4773ea;--green:#28bd7d;--red:#ff6370;--shadow:0 20px 55px #0005}
    *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}body{margin:0;min-height:100vh;background:radial-gradient(circle at 15% 5%,#356df324,transparent 34%),var(--bg);color:var(--text);font-family:Arial,sans-serif;transition:.2s}button{font:inherit}.app{width:min(100%,700px);margin:auto;padding:22px 18px 40px}.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px}.brand{display:flex;align-items:center;gap:12px}.logo{display:grid;place-items:center;width:48px;height:48px;border-radius:15px;background:linear-gradient(135deg,var(--blue),#7d55ef);font-size:25px}.top h1{font-size:26px;margin:0}.sub{color:var(--muted);font-size:13px;margin:4px 0 0}.theme{width:45px;height:45px;border:1px solid var(--line);border-radius:14px;background:var(--card);cursor:pointer}.telemetry{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:18px}.chip{padding:8px 11px;border:1px solid var(--line);border-radius:999px;background:var(--card);font-size:12px}.panel{min-height:430px;padding:32px;border:1px solid var(--line);border-radius:26px;background:var(--card);box-shadow:var(--shadow)}.panel-head{display:flex;justify-content:space-between;gap:12px;margin-bottom:25px}.panel h2{margin:0 0 7px}.badge{height:27px;padding:6px 10px;border-radius:99px;background:#16a56a1c;color:var(--green);font-size:11px;font-weight:800}.drive-pad{display:grid;grid-template-columns:repeat(3,78px);grid-template-rows:repeat(3,68px);justify-content:center;gap:10px}.drive{border:0;border-radius:18px;background:linear-gradient(145deg,var(--blue),var(--blue2));color:white;font-size:27px;font-weight:bold;cursor:pointer;touch-action:none;user-select:none;box-shadow:0 8px 18px #356df33b}.drive:active{transform:scale(.95)}.stop{background:linear-gradient(145deg,var(--red),#d63846)}.error{display:none;margin-bottom:14px;padding:11px 14px;border-radius:12px;background:#ef4e5b1c;color:var(--red);font-size:13px}.error.show{display:block}
    @media(max-width:700px){.app{padding:16px 13px 40px}.panel{padding:24px 17px;min-height:440px}.drive-pad{grid-template-columns:repeat(3,72px)}}
  </style>
</head>
<body>
<main class="app">
  <header class="top"><div class="brand"><div class="logo">🚗</div><div><h1>Kylio Robot Car</h1><p class="sub">ESP-IDF Control Dashboard</p></div></div><button class="theme" id="themeBtn" onclick="toggleTheme()">🌙</button></header>
  <div class="telemetry"><span class="chip" id="ipChip">IP: --</span><span class="chip" id="motionChip">Motion: stopped</span><span class="chip" id="wifiChip">Wi-Fi: --</span></div>
  <div class="error" id="errorBox"></div>
  <section class="panel">
      <div class="panel-head"><div><h2>Manual Control</h2><p class="sub">Hold to drive; left and right rotate in place. Release or lost connection stops the car.</p></div><span class="badge">READY</span></div>
        <div class="drive-pad"><span></span><button class="drive robot-control" data-dir="f" aria-label="Forward">▲</button><span></span><button class="drive robot-control" data-dir="l" aria-label="Rotate left">◀</button><button class="drive stop" onclick="emergencyStop()" aria-label="Stop">■</button><button class="drive robot-control" data-dir="r" aria-label="Rotate right">▶</button><span></span><button class="drive robot-control" data-dir="b" aria-label="Backward">▼</button><span></span></div>
  </section>
</main>
<script>
const themeBtn=document.getElementById('themeBtn'),themeColor=document.getElementById('themeColor'),errorBox=document.getElementById('errorBox'),ipChip=document.getElementById('ipChip'),motionChip=document.getElementById('motionChip'),wifiChip=document.getElementById('wifiChip');
let holdTimer=null;let heldDir=null;let heldHoldId=0;let heartbeatInFlight=false;let commandSequence=Date.now();
function toggleTheme(){const dark=document.body.classList.toggle('dark');themeBtn.textContent=dark?'☀️':'🌙';themeColor.content=dark?'#0c1220':'#eef4ff';localStorage.setItem('kylio-theme',dark?'dark':'light')}
function showError(message=''){errorBox.textContent=message;errorBox.classList.toggle('show',!!message)}
async function request(url,keepalive=false){try{const response=await fetch(url,{cache:'no-store',keepalive});if(!response.ok)throw new Error((await response.text())||`HTTP ${response.status}`);showError();return response}catch(error){showError(error.message);throw error}}
function command(dir,on,keepalive=!on,holdId=0){const sequence=++commandSequence;return request(`/cmd?dir=${encodeURIComponent(dir)}&state=${on?1:0}&seq=${sequence}${holdId?`&hold=${holdId}`:''}`,keepalive)}
function sendHeartbeat(){if(!heldDir||heartbeatInFlight)return;const direction=heldDir,holdId=heldHoldId;heartbeatInFlight=true;command(direction,true,false,holdId).catch(error=>{if(error.message==='Press was stopped'&&heldHoldId===holdId)endHold()}).finally(()=>{heartbeatInFlight=false})}
function beginHold(event){event.preventDefault();endHold();heldDir=event.currentTarget.dataset.dir;heldHoldId=++commandSequence;if(event.currentTarget.setPointerCapture)event.currentTarget.setPointerCapture(event.pointerId);sendHeartbeat();holdTimer=setInterval(sendHeartbeat,100)}
function endHold(){if(holdTimer){clearInterval(holdTimer);holdTimer=null}if(heldDir){const direction=heldDir,holdId=heldHoldId;heldDir=null;heldHoldId=0;command(direction,false,true,holdId).catch(()=>{})}}
function emergencyStop(){endHold();command('s',true,true).then(refresh).catch(()=>{})}
async function refresh(){try{const response=await request('/api/state');const state=await response.json();ipChip.textContent=`IP: ${state.ip}`;motionChip.textContent=`Motion: ${state.motion}`;wifiChip.textContent=state.wifiConnected?`Wi-Fi: ${state.rssi} dBm`:'Wi-Fi: disconnected'}catch(_){}}
document.querySelectorAll('[data-dir]').forEach(button=>{button.addEventListener('pointerdown',beginHold);button.addEventListener('pointerup',endHold);button.addEventListener('pointercancel',endHold);button.addEventListener('lostpointercapture',endHold)});window.addEventListener('blur',endHold);document.addEventListener('visibilitychange',()=>{if(document.hidden)endHold()});if(localStorage.getItem('kylio-theme')==='dark')toggleTheme();refresh();setInterval(refresh,1000);
</script>
</body>
</html>
)HTML";
