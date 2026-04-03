#pragma once

// Web UI as embedded HTML string.
// Served directly by WebServer — no SD card required.
const char WEBUI[] = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Bulletproof Wakeup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 50%,#0f3460 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#fff;border-radius:20px;box-shadow:0 25px 60px rgba(0,0,0,.4);padding:32px 24px;max-width:380px;width:100%}
h1{text-align:center;color:#1a1a2e;font-size:24px;margin-bottom:6px}
.sub{text-align:center;color:#aaa;font-size:12px;margin-bottom:20px}
.ct{text-align:center;font-size:56px;font-weight:700;color:#0f3460;font-family:'Courier New',monospace;letter-spacing:4px;margin-bottom:4px}
.ct-label{text-align:center;color:#bbb;font-size:11px;text-transform:uppercase;letter-spacing:2px;margin-bottom:14px}
.status-row{text-align:center;margin-bottom:20px}
.badge{display:inline-block;padding:5px 16px;border-radius:20px;font-size:13px;font-weight:700;letter-spacing:.5px}
.badge-armed{background:#d4edda;color:#155724;border:1px solid #c3e6cb}
.badge-disabled{background:#e9ecef;color:#6c757d;border:1px solid #ced4da}
.alarm-banner{display:none;margin-bottom:14px;padding:14px;background:#fff3cd;border:1px solid #ffc107;border-radius:10px;text-align:center;color:#856404;font-weight:600;font-size:14px}
.alarm-banner.active{display:block}
.confirm-box{display:none;border:2px solid #ffc107;background:#fffdf0;border-radius:14px;padding:20px;margin-bottom:16px;text-align:center}
.confirm-box.show{display:block}
.confirm-title{font-size:15px;font-weight:700;color:#856404;margin-bottom:8px}
.confirm-desc{font-size:13px;color:#555;margin-bottom:12px;line-height:1.6}
.confirm-timer{font-size:32px;font-weight:700;color:#0f3460;font-family:'Courier New',monospace;margin-bottom:12px}
.btn-cancel{padding:9px 22px;border:2px solid #ccc;border-radius:8px;background:#fff;color:#666;font-size:14px;font-weight:600;cursor:pointer;transition:background .15s}
.btn-cancel:hover{background:#f0f0f0}
.section-label{text-align:center;color:#999;font-size:11px;text-transform:uppercase;letter-spacing:2px;margin-bottom:14px}
.picker{display:flex;align-items:center;justify-content:center;gap:14px;margin-bottom:24px}
.pcol{display:flex;flex-direction:column;align-items:center;gap:8px}
.plabel{font-size:11px;color:#aaa;text-transform:uppercase;letter-spacing:1px}
.sbtn{width:46px;height:46px;border:2px solid #dde;background:#f8f9ff;border-radius:10px;font-size:24px;font-weight:bold;color:#0f3460;cursor:pointer;display:flex;align-items:center;justify-content:center;user-select:none;-webkit-tap-highlight-color:transparent;transition:background .15s,border-color .15s}
.sbtn:active{background:#e0e8ff;border-color:#0f3460}
.sbtn:disabled{opacity:.35;cursor:not-allowed}
.tval{width:84px;height:76px;font-size:42px;font-weight:700;text-align:center;border:2px solid #dde;border-radius:12px;color:#1a1a2e;font-family:'Courier New',monospace;background:#fafafa}
.tval:focus{outline:none;border-color:#0f3460;background:#fff}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none}
input[type=number]{-moz-appearance:textfield}
.colon{font-size:50px;font-weight:700;color:#ccc;line-height:76px;margin-top:22px}
.btnrow{display:flex;gap:12px;margin-bottom:10px}
.btn{flex:1;padding:16px;border:none;border-radius:12px;font-size:16px;font-weight:700;cursor:pointer;letter-spacing:.5px;transition:transform .1s,opacity .1s;-webkit-tap-highlight-color:transparent}
.btn:active{transform:scale(.97);opacity:.9}
.btn:disabled{opacity:.4;cursor:not-allowed;transform:none}
.btn-set{background:linear-gradient(135deg,#0f3460 0%,#16213e 100%);color:#fff;box-shadow:0 4px 16px rgba(15,52,96,.4)}
.btn-test{background:#f0f4ff;color:#0f3460;border:2px solid #cdd8f0}
.btn-disable{display:none;width:100%;padding:12px;border:2px solid #ddd;border-radius:12px;font-size:14px;font-weight:600;cursor:pointer;color:#888;background:#f9f9f9;margin-bottom:14px;transition:background .15s,border-color .15s}
.btn-disable:hover{background:#f0f0f0;border-color:#bbb}
.btn-disable:disabled{opacity:.4;cursor:not-allowed}
.toast{padding:14px;border-radius:10px;text-align:center;font-size:14px;font-weight:600;overflow:hidden;max-height:0;transition:max-height .3s,padding .3s,margin .3s}
.toast.ok{max-height:60px;background:#d4edda;color:#155724;border:1px solid #c3e6cb}
.toast.err{max-height:60px;background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}
.info{text-align:center;color:#ccc;font-size:12px;margin-top:18px;line-height:1.7}
.lock-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(26,26,46,.92);z-index:1000;align-items:center;justify-content:center;flex-direction:column;padding:20px}
.lock-overlay.active{display:flex}
.lock-icon{font-size:72px;margin-bottom:16px}
.lock-title{color:#ff6b6b;font-size:22px;font-weight:700;margin-bottom:10px;text-align:center}
.lock-msg{color:#ddd;font-size:15px;text-align:center;line-height:1.6;max-width:320px}
.lock-progress{margin-top:20px;color:#ffc107;font-size:18px;font-weight:700;font-family:'Courier New',monospace}
</style>
</head>
<body>
<div class="lock-overlay" id="lock-overlay">
  <div class="lock-icon">&#128274;</div>
  <div class="lock-title">Web Control Locked</div>
  <div class="lock-msg">Snooze cycles still running.<br>Dismiss all alarms with BtnA on the kitchen device.</div>
  <div class="lock-progress" id="lock-progress">Snooze 1/2</div>
</div>
<div class="card">
  <h1>Bulletproof Wakeup</h1>
  <p class="sub">Alarm rings in the bedroom</p>

  <div class="ct" id="ct">--:--</div>
  <div class="ct-label">Current Time</div>

  <div class="status-row">
    <span class="badge badge-armed" id="status-badge">ACTIVE --:--</span>
  </div>

  <div class="alarm-banner" id="banner">&#128226; ALARM RINGING! &#8594; Press BtnA to stop</div>

  <div class="confirm-box" id="confirm-box">
    <div class="confirm-title">&#9888; Confirmation Required</div>
    <div class="confirm-desc" id="confirm-desc">Please press BtnA on the kitchen device</div>
    <div class="confirm-timer" id="confirm-timer">30s</div>
    <button class="btn-cancel" onclick="cancelConfirm()">Cancel</button>
  </div>

  <div class="section-label">New Alarm Time</div>
  <div class="picker">
    <div class="pcol">
      <span class="plabel">Hour</span>
      <button class="sbtn" id="adj-h-up" onclick="adj('h',1)">+</button>
      <input class="tval" type="text" id="ih" value="07" readonly>
      <button class="sbtn" id="adj-h-dn" onclick="adj('h',-1)">&#8722;</button>
    </div>
    <div class="colon">:</div>
    <div class="pcol">
      <span class="plabel">Minute</span>
      <button class="sbtn" id="adj-m-up" onclick="adj('m',1)">+</button>
      <input class="tval" type="text" id="im" value="00" readonly>
      <button class="sbtn" id="adj-m-dn" onclick="adj('m',-1)">&#8722;</button>
    </div>
  </div>

  <div class="btnrow">
    <button class="btn btn-set" id="btn-set" onclick="requestSetAlarm()">Set Alarm</button>
    <button class="btn btn-test" id="btn-test" onclick="testAlarm()">Test</button>
  </div>
  <button class="btn btn-disable" id="btn-disable" onclick="requestDisableAlarm()">Disable Alarm</button>

  <div class="toast" id="toast"></div>
  <p class="info">BtnA confirmation only required if alarm is due in &le;5h</p>
</div>

<script>
function pad(n){return String(n).padStart(2,'0')}

function adj(f,d){
  const el=document.getElementById(f==='h'?'ih':'im');
  const max=f==='h'?23:59;
  el.value=pad(((parseInt(el.value)||0)+d+max+1)%(max+1));
}

let toastTimer=null;
function showToast(msg,ok){
  const t=document.getElementById('toast');
  t.textContent=msg;
  t.className='toast '+(ok?'ok':'err');
  if(toastTimer)clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>{t.className='toast'},4000);
}

let confirmPolling=null;
let initialLoad=true;

function setButtonsDisabled(dis){
  ['btn-set','btn-test','btn-disable'].forEach(id=>{
    const el=document.getElementById(id);
    if(el)el.disabled=dis;
  });
  ['adj-h-up','adj-h-dn','adj-m-up','adj-m-dn'].forEach(id=>{
    const el=document.getElementById(id);
    if(el)el.disabled=dis;
  });
  const ih=document.getElementById('ih');
  const im=document.getElementById('im');
  if(ih)ih.readOnly=dis;
  if(im)im.readOnly=dis;
}

function showConfirmBox(action,h,m,remaining){
  const desc=document.getElementById('confirm-desc');
  if(action==='set'){
    desc.textContent='Please press BtnA on the kitchen device to set alarm '+pad(h)+':'+pad(m)+'.';
  } else {
    desc.textContent='Please press BtnA on the kitchen device to disable the alarm.';
  }
  document.getElementById('confirm-timer').textContent=remaining+'s';
  document.getElementById('confirm-box').className='confirm-box show';
  setButtonsDisabled(true);
}

function hideConfirmBox(){
  document.getElementById('confirm-box').className='confirm-box';
  setButtonsDisabled(false);
  if(confirmPolling){clearInterval(confirmPolling);confirmPolling=null;}
}

function startConfirmPolling(){
  if(confirmPolling)clearInterval(confirmPolling);
  confirmPolling=setInterval(pollConfirmStatus,1000);
}

async function pollConfirmStatus(){
  try{
    const r=await fetch('/confirm-status');
    if(!r.ok)return;
    const d=await r.json();
    if(d.state==='confirmed'){
      hideConfirmBox();
      if(d.action==='set'){
        showToast('\u2713 Alarm set: '+pad(d.hour)+':'+pad(d.minute),true);
      } else {
        showToast('\u2713 Alarm disabled',true);
      }
      applyStatus(await fetchStatusData());
    } else if(d.state==='waiting'){
      document.getElementById('confirm-timer').textContent=d.remaining+'s';
    } else if(d.state==='timeout'){
      hideConfirmBox();
      showToast('Timeout \u2014 BtnA was not pressed',false);
    } else {
      hideConfirmBox();
    }
  }catch(e){}
}

function applyStatus(d){
  if(!d)return;
  if(d.time)document.getElementById('ct').textContent=d.time;
  const badge=document.getElementById('status-badge');
  if(d.enabled){
    badge.textContent='ACTIVE '+pad(d.hour)+':'+pad(d.minute);
    badge.className='badge badge-armed';
  } else {
    badge.textContent='DISABLED';
    badge.className='badge badge-disabled';
  }
  document.getElementById('banner').className='alarm-banner'+(d.active?' active':'');
  // Show disable button only when alarm is active
  document.getElementById('btn-disable').style.display=d.enabled?'block':'none';
  // Lock overlay: web locked during snooze phase
  const overlay=document.getElementById('lock-overlay');
  if(d.webLocked){
    overlay.className='lock-overlay active';
    const prog=document.getElementById('lock-progress');
    prog.textContent='Snooze '+d.snoozeCount+'/'+d.snoozeMax+(d.snooze?' \u2014 Next alarm in '+d.snoozeRemaining+'s':'');
  } else {
    overlay.className='lock-overlay';
  }
}

async function fetchStatusData(){
  const r=await fetch('/status');
  if(!r.ok)return null;
  return r.json();
}

async function fetchStatus(){
  try{
    const d=await fetchStatusData();
    if(!d)return;
    if(initialLoad){
      // Initialize time fields on first load
      document.getElementById('ih').value=pad(d.hour);
      document.getElementById('im').value=pad(d.minute);
      initialLoad=false;
    }
    applyStatus(d);
  }catch(e){}
}

async function requestSetAlarm(){
  const h=parseInt(document.getElementById('ih').value,10)||0;
  const m=parseInt(document.getElementById('im').value,10)||0;
  try{
    const r=await fetch('/request-confirm',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'action=set&hour='+h+'&minute='+m});
    if(r.ok){
      const t=await r.text();
      if(t==='OK:direct'){
        showToast('\u2713 Alarm set: '+pad(h)+':'+pad(m),true);
        applyStatus(await fetchStatusData());
      } else {
        showConfirmBox('set',h,m,30);
        startConfirmPolling();
      }
    } else if(r.status===423){
      showToast('Locked \u2014 dismiss snooze cycles first!',false);
    } else if(r.status===409){
      const t=await r.text();
      showToast(t,false);
    } else {
      showToast('Error ('+r.status+')',false);
    }
  }catch(e){showToast('Connection error',false)}
}

async function requestDisableAlarm(){
  try{
    const r=await fetch('/request-confirm',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'action=disable'});
    if(r.ok){
      const t=await r.text();
      if(t==='OK:direct'){
        showToast('\u2713 Alarm disabled',true);
        applyStatus(await fetchStatusData());
      } else {
        showConfirmBox('disable',0,0,30);
        startConfirmPolling();
      }
    } else if(r.status===423){
      showToast('Locked \u2014 dismiss snooze cycles first!',false);
    } else if(r.status===409){
      const t=await r.text();
      showToast(t,false);
    } else {
      showToast('Error ('+r.status+')',false);
    }
  }catch(e){showToast('Connection error',false)}
}

async function cancelConfirm(){
  hideConfirmBox();
  try{await fetch('/cancel-confirm',{method:'POST'});}catch(e){}
}

async function testAlarm(){
  try{
    const r=await fetch('/test-alarm',{method:'POST'});
    if(r.ok){showToast('Test alarm triggered!',true)}
    else if(r.status===423){showToast('Locked \u2014 dismiss snooze cycles first!',false)}
    else{showToast('Error ('+r.status+')',false)}
  }catch(e){showToast('Connection error',false)}
}

// On load: fetch status, then check if confirmation is pending
(async()=>{
  await fetchStatus();
  try{
    const r=await fetch('/confirm-status');
    if(!r.ok)return;
    const d=await r.json();
    if(d.state==='waiting'){
      showConfirmBox(d.action,d.hour,d.minute,d.remaining);
      startConfirmPolling();
    }
  }catch(e){}
})();

// Update time + status every 5s
setInterval(async()=>{
  try{
    const d=await fetchStatusData();
    if(!d)return;
    applyStatus(d);
  }catch(e){}
},5000);
</script>
</body>
</html>)rawliteral";
