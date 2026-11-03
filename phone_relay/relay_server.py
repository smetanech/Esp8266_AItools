#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# relay_server.py — ретранслятор ESP <-> Control Panel (запускается на телефоне в Termux).
# ---------------------------------------------------------------
# Зачем это нужно:
#  - ESP8266 не выходит в интернет напрямую, он подключается к точке доступа телефона.
#  - Этот сервер живёт на телефоне, принимает данные от ESP по локальной сети (порт 8000)
#    и пересылает их в публичный Control Panel (CP) или забирает обновления оттуда.
#  - Плюс отдаёт простой локальный веб-интерфейс для правки промптов/заметок прямо с телефона.
#
# Запуск в Termux:
#   pip install flask flask-cors requests
#   python relay_server.py
# ---------------------------------------------------------------
# КЛЮЧЕВОЕ: данные форвардятся в CP при КАЖДОМ POST, никакой фильтрации.

from flask import Flask, request, jsonify, render_template_string
from flask_cors import CORS
import requests, json, threading, time, socket
from pathlib import Path
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
from requests.adapters import HTTPAdapter
from urllib3.poolmanager import PoolManager
import ssl

class TLSAdapter(HTTPAdapter):
    def init_poolmanager(self, connections, maxsize, block=False):
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.poolmanager = PoolManager(num_pools=connections, maxsize=maxsize, block=block, ssl_context=ctx)

app = Flask(__name__)
CORS(app, resources={r"/*": {"origins": "*"}})

# ========== КОНФИГ ==========
# Публичный адрес Control Panel. Укажи свой (Tailscale funnel / туннель / VPS).
# Пример: "https://your-panel.example.com"
PUBLIC_CONTROL_PANEL = "https://YOUR_PUBLIC_CONTROL_PANEL_URL"
# Общий токен: должен совпадать с API_TOKEN в control_panel/app.py.
API_TOKEN = "CHANGE_ME_RANDOM_SECRET_TOKEN"
RELAY_PORT = 8000
SYNC_INTERVAL = 5  # Чаще опрашиваем CP (5 сек)
DATA_DIR = Path("relay_data")
DATA_DIR.mkdir(exist_ok=True)

# Авто-определение IP точки доступа Android
def detect_ap_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except: pass
    try:
        import subprocess
        result = subprocess.run(['ip', 'route'], capture_output=True, text=True, timeout=2)
        if 'src' in result.stdout:
            for line in result.stdout.split('\n'):
                if 'src' in line:
                    return line.split('src')[1].strip().split()[0]
    except: pass
    return "192.168.43.1"

PHONE_IP = detect_ap_ip()
# ================================

# Локальное состояние = последнее полностью синхронизированное
local_state = {
    "prompts": [], "notes": [], "messages": [], "notice": "No notice",
    "_last_synced": 0  # версия, которую последний раз отдали ESP
}
state_lock = threading.Lock()
sync_session = requests.Session()
sync_session.mount('https://', TLSAdapter())

# HTML — локальный веб-интерфейс
LOCAL_WEB_UI = """
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP Control</title><style>
*{box-sizing:border-box}body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:1rem;background:#111;color:#eee}
h1{color:#4fc3f7;font-size:1.5rem;margin:0 0 1rem}
h2{color:#81d4fa;font-size:1.1rem;margin:1rem 0 0.5rem;border-bottom:1px solid #333;padding-bottom:0.3rem}
.card{background:#222;padding:1rem;border-radius:8px;margin:0.7rem 0}
input,textarea{width:100%;background:#333;border:1px solid #444;color:#fff;padding:0.5rem;border-radius:4px;margin:0.3rem 0;box-sizing:border-box}
button{background:#4fc3f7;color:#000;border:none;padding:0.5rem 1rem;border-radius:4px;cursor:pointer;font-weight:bold;margin:0.2rem}
button:hover{background:#29b6f6}button.d{background:#ef5350;color:#fff}
.row{display:flex;gap:0.3rem;flex-wrap:wrap;align-items:center;margin:0.3rem 0}
.item{background:#333;padding:0.5rem;border-radius:4px;margin:0.3rem 0;display:flex;gap:0.3rem;align-items:center}
.item input{flex:1;padding:0.3rem;background:#222;border:1px solid #444;color:#fff;border-radius:3px;margin:0}
.del{background:#ef5350;color:#fff;border:none;padding:0.3rem 0.5rem;border-radius:3px;cursor:pointer}
.badge{display:inline-block;padding:0.2rem 0.5rem;background:#444;border-radius:3px;font-size:0.8rem;margin-right:0.3rem}
.status{padding:0.5rem;border-radius:4px;margin:0.5rem 0;font-size:0.9rem}
.ok-bg{background:#1b5e20;color:#a5d6a7}
.warn-bg{background:#e65100;color:#ffcc80}
</style></head><body>
<h1>🎮 ESP Local Control</h1>
<div id="status" class="status">Loading...</div>
<div class="card">
<h2>📝 Prompts <span class="badge" id="pc">0</span></h2>
<div id="prompts"></div>
<div class="row"><input id="newP" placeholder="New prompt (max 60)"><button onclick="addP()">+ Add</button></div>
</div>
<div class="card">
<h2>📌 Notes <span class="badge" id="nc">0</span></h2>
<div id="notes"></div>
<div class="row"><input id="newN" placeholder="New note"><button onclick="addN()">+ Add</button><button class="d" onclick="clrN()">Clear All</button></div>
</div>
<div class="card">
<h2>💬 Messages <span class="badge" id="mc">0</span></h2>
<div id="msgs"></div>
<div class="row"><input id="newM" placeholder="New message (max 180)"><button onclick="addM()">+ Add</button><button class="d" onclick="clrM()">Clear All</button></div>
</div>
<div class="card">
<h2>📢 Notice</h2>
<div class="row"><input id="noticeInp" placeholder="Notice text"><button onclick="setN()">Set</button></div>
</div>
<div class="card">
<h2>🤖 AI Question</h2>
<textarea id="aiP" rows="3" placeholder="Ask AI..."></textarea>
<div class="row"><button onclick="askAI()">Send to AI</button></div>
<div id="aiRes" class="card" style="margin-top:1rem;background:#1a1a1a"></div>
</div>
<div class="card">
<h2>ℹ️ System Info</h2>
<div>Local URL: <b>http://""" + PHONE_IP + """:""" + str(RELAY_PORT) + """</b></div>
<div>Public Server: <b>""" + PUBLIC_CONTROL_PANEL + """</b></div>
<div>Last CP sync: <span id="lastCp">never</span></div>
<div>Last ESP sync: <span id="lastEsp">never</span></div>
</div>
<script>
let data={prompts:[],notes:[],messages:[],notice:""};

async function load(){
  try{
    const r=await(await fetch('/api/state')).json();
    data={prompts:r.prompts||[],notes:r.notes||[],messages:r.messages||[],notice:r.notice||""};
    render();
    document.getElementById('status').className='status ok-bg';
    document.getElementById('status').textContent='✅ Connected. ESP items: '+(r.prompts||[]).length+'P '+(r.notes||[]).length+'N '+(r.messages||[]).length+'M';
  }catch(e){
    document.getElementById('status').className='status warn-bg';
    document.getElementById('status').textContent='⚠️ Cannot reach relay. ESP not connected?';
  }
}

function render(){
  document.getElementById('pc').textContent=data.prompts.length;
  document.getElementById('nc').textContent=data.notes.length;
  document.getElementById('mc').textContent=data.messages.length;
  document.getElementById('noticeInp').value=data.notice;

  let ph='';data.prompts.forEach((p,i)=>{
    ph+=`<div class="item"><span style="min-width:20px">${i+1}.</span><input id="p${i}" value="${(p||'').replace(/"/g,'&quot;')}" onchange="updP(${i}, this.value)"><button class="del" onclick="delP(${i})">×</button></div>`;
  });
  document.getElementById('prompts').innerHTML=ph||'<i style="color:#888">empty</i>';

  let nh='';data.notes.forEach((n,i)=>{
    nh+=`<div class="item"><span style="min-width:20px">${i+1}.</span><input id="n${i}" value="${(n||'').replace(/"/g,'&quot;')}" onchange="updN(${i}, this.value)"><button class="del" onclick="delN(${i})">×</button></div>`;
  });
  document.getElementById('notes').innerHTML=nh||'<i style="color:#888">empty</i>';

  let mh='';data.messages.forEach((m,i)=>{
    const t=m.text||m;
    mh+=`<div class="item"><span style="min-width:30px">[${m.fromESP?'Y':'W'}]</span><input id="m${i}" value="${(t||'').replace(/"/g,'&quot;')}" onchange="updM(${i}, this.value)"><button class="del" onclick="delM(${i})">×</button></div>`;
  });
  document.getElementById('msgs').innerHTML=mh||'<i style="color:#888">empty</i>';
}

// Отправляет на relay, тот форвардит в Control Panel, и при следующем pull CP отдаст обновления
async function push(){
  const resp=await fetch('/api/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  if(resp.ok){
    console.log('Pushed to Control Panel');
    // Сразу опрашиваем чтобы UI обновился
    setTimeout(load, 500);
  } else {
    alert('Save failed!');
  }
}

async function updP(i,v){data.prompts[i]=v; await push();}
async function delP(i){data.prompts.splice(i,1); render(); await push();}
async function addP(){const v=document.getElementById('newP').value.trim();if(!v)return;data.prompts.push(v);document.getElementById('newP').value='';render();await push();}
async function updN(i,v){data.notes[i]=v;await push();}
async function delN(i){data.notes.splice(i,1);render();await push();}
async function addN(){const v=document.getElementById('newN').value.trim();if(!v)return;data.notes.push(v);document.getElementById('newN').value='';render();await push();}
async function clrN(){if(!confirm('Clear all notes?'))return;data.notes=[];render();await push();}
async function updM(i,v){data.messages[i].text=v;await push();}
async function delM(i){data.messages.splice(i,1);render();await push();}
async function addM(){const v=document.getElementById('newM').value.trim();if(!v)return;data.messages.push({text:v,fromESP:false,time:Date.now()});document.getElementById('newM').value='';render();await push();}
async function clrM(){if(!confirm('Clear all messages?'))return;data.messages=[];render();await push();}
async function setN(){data.notice=document.getElementById('noticeInp').value;await push();document.getElementById('status').className='status ok-bg';document.getElementById('status').textContent='✅ Notice saved';}

async function askAI(){
  const p=document.getElementById('aiP').value.trim();if(!p)return;
  document.getElementById('aiRes').textContent='Thinking... (may take 10-60 sec)';
  const r=await(await fetch('/api/ai',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:p})})).json();
  document.getElementById('aiRes').textContent=(r.response||r.err||'No response');
}

load();
setInterval(load, 2000);
</script></body></html>
"""

def forward_to_cp(payload):
    """Отправить данные в Control Panel. Возвращает True если успешно."""
    try:
        r = sync_session.post(
            f"{PUBLIC_CONTROL_PANEL}/api/sync",
            json=payload,
            timeout=10,
            headers={"Authorization": f"Bearer {API_TOKEN}"}
        )
        if r.status_code == 200:
            print(f"[Forward] OK to CP")
            return True
        print(f"[Forward] CP {r.status_code}: {r.text[:100]}")
    except Exception as e:
        print(f"[Forward] Error: {e}")
    return False

# ========== ENDPOINTS ==========

@app.route("/")
def root(): return LOCAL_WEB_UI

@app.route("/api/ping")
def ping(): return jsonify({"pong": True, "ap_ip": PHONE_IP})

@app.route("/api/state", methods=["GET"])
def get_state():
    """ESP получает своё состояние."""
    with state_lock:
        return jsonify({
            "prompts": local_state.get("prompts", []),
            "notes": local_state.get("notes", []),
            "messages": local_state.get("messages", []),
            "notice": local_state.get("notice", "No notice"),
            "ai_responses": local_state.get("ai_responses", []),
            "version": local_state.get("_version", 0)
        })

@app.route("/api/state", methods=["POST"])
def set_state():
    """
    Принимает от ESP или UI.
    ОБЯЗАТЕЛЬНО форвардит в Control Panel.
    БЕЗ ФИЛЬТРОВ - всё что пришло сохраняется.
    """
    d = request.json or {}
    # Всегда форвардим в Control Panel
    if "device_id" in d:
        forward_to_cp(d)
    
    # Также сохраняем локально
    with state_lock:
        local_state.update(d)
        local_state["_version"] = local_state.get("_version", 0) + 1
        try:
            (DATA_DIR / "esp_state.json").write_text(
                json.dumps(local_state, ensure_ascii=False), encoding="utf-8"
            )
        except Exception as e:
            print(f"[Save] {e}")
    return jsonify({"ok": True, "version": local_state.get("_version", 0)})

@app.route("/api/ai", methods=["POST"])
def ai():
    d = request.json or {}
    prompt = d.get("prompt", "")
    if not prompt: return jsonify({"err": "empty"}), 400
    try:
        r = sync_session.post(f"{PUBLIC_CONTROL_PANEL}/api/ai", json=d, timeout=180,
                              headers={"Authorization": f"Bearer {API_TOKEN}"})
        if r.status_code == 200: return jsonify(r.json())
        return jsonify({"err": f"CP {r.status_code}"}), 500
    except Exception as e:
        return jsonify({"err": str(e)}), 500

# ========== SYNC LOOP: тянем с Control Panel каждые 5 сек ==========
# Локальное состояние — это «последняя известная вселенная». Мы держим его,
# чтобы отвечать ESP мгновенно даже без интернета, а в фоне догоняем CP.
def sync_loop():
    global local_state
    while True:
        time.sleep(SYNC_INTERVAL)
        try:
            r = sync_session.get(f"{PUBLIC_CONTROL_PANEL}/api/devices", timeout=15,
                                  headers={"Authorization": f"Bearer {API_TOKEN}"})
            if r.status_code != 200:
                print(f"[Sync] CP returned {r.status_code}")
                continue
            data = r.json()
            if not data: continue
            
            # Берём данные нашего устройства
            target_id = next((k for k in data.keys() if not k.startswith("_")), None)
            if not target_id: continue
            
            cp_data = data[target_id]
            cp_version = data.get("_version", 0)
            
            with state_lock:
                local_version = local_state.get("_version", 0)
                # Принимаем CP только если версия больше нашей
                # (UI-изменения имеют свою версию, поэтому не затираются)
                if cp_version > local_version:
                    # Проверяем что это действительно наши данные (сравниваем MAC)
                    if "device_id" not in cp_data or cp_data.get("device_id") == target_id:
                        local_state.update(cp_data)
                        local_state["_version"] = cp_version
                        (DATA_DIR / "esp_state.json").write_text(
                            json.dumps(local_state, ensure_ascii=False), encoding="utf-8"
                        )
                        print(f"[Sync] Got from CP v{cp_version}: {len(cp_data.get('prompts',[]))}P {len(cp_data.get('notes',[]))}N {len(cp_data.get('messages',[]))}M")
                else:
                    print(f"[Sync] Local v{local_version} >= CP v{cp_version}, skip")
        except Exception as e:
            print(f"[Sync] Error: {e}")

# ========== MAIN ==========

if __name__ == "__main__":
    state_file = DATA_DIR / "esp_state.json"
    if state_file.exists():
        try:
            saved = json.loads(state_file.read_text(encoding="utf-8"))
            with state_lock:
                local_state.update(saved)
            print(f"[Init] Loaded: {len(saved.get('prompts',[]))}P {len(saved.get('notes',[]))}N")
        except: pass
    
    with state_lock:
        for key in ("prompts", "notes", "messages", "ai_responses"):
            if key not in local_state: local_state[key] = []
        if "notice" not in local_state: local_state["notice"] = "No notice"
    
    threading.Thread(target=sync_loop, daemon=True).start()
    
    print("=" * 50)
    print(f"Phone Relay v3.0 (instant sync, no filters)")
    print(f"Local: http://{PHONE_IP}:{RELAY_PORT}/")
    print(f"Public: {PUBLIC_CONTROL_PANEL}")
    print("=" * 50)
    
    app.run(host="0.0.0.0", port=RELAY_PORT, threaded=True, debug=False)
