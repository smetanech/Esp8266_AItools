# app.py — Control Panel (веб-панель для управления устройствами + AI-чат)
# ---------------------------------------------------------------
# Что делает:
#   - хранит данные всех устройств (промпты, заметки, сообщения) в JSON-файлах
#   - принимает их от ESP-терминалов и отдаёт обратно при синхронизации
#   - по запросу «/api/ai» отправляет промпт в локальный Ollama и возвращает ответ
#
# Запуск (на ПК с установленным Ollama):
#   pip install flask flask-cors requests
#   python app.py
#
# Доступ из интернета (Tailscale / туннель):
#   tailscale funnel 5000
#
# БЕЗОПАСНОСТЬ: заполни API_TOKEN собственной строкой перед использованием.

from flask import Flask, request, jsonify, render_template
from flask_cors import CORS
import requests, json, time, threading
from pathlib import Path

app = Flask(__name__, static_folder='.', template_folder='templates')
# CORS включён для локального веб-интерфейса; при публикации сузь его.
CORS(app, resources={r"/*": {"origins": "*"}})

DATA_DIR = Path("control_data")
DATA_DIR.mkdir(exist_ok=True)

# Настройки Ollama (локальная LLM). Модель укажи свою.
OLLAMA_URL = "http://127.0.0.1:11434/api/generate"
OLLAMA_MODEL = "deepseek-r1:1.5b"
SYSTEM_PROMPT = "You must answer ONLY in English. Be brief and clear."

# Замени на случайную длинную строку. Тот же токен пропиши в ESP и relay_server.py.
API_TOKEN = "CHANGE_ME_RANDOM_SECRET_TOKEN"

# Синхронизация (для уведомления ESP об изменениях)
esp_subscribers = set()  # WebSocket-подобная штука: просто polling
data_version = {"value": 0}  # Увеличивается при любом изменении
data_lock = threading.Lock()

def sanitize_mac(mac): return mac.replace(":", "_").replace("-", "_").upper()
def device_path(m): return DATA_DIR / f"{sanitize_mac(m)}.json"

def load_dev(mac):
    p = device_path(mac)
    if p.exists():
        try: return json.loads(p.read_text(encoding="utf-8"))
        except: pass
    return {"prompts": [], "notes": [], "messages": [], "notice": "No notice"}

def save_dev(mac, d):
    with data_lock:
        device_path(mac).write_text(json.dumps(d, ensure_ascii=False), encoding="utf-8")
        data_version["value"] += 1

def call_ollama(prompt):
    try:
        r = requests.post(OLLAMA_URL, json={
            "model": OLLAMA_MODEL, 
            "prompt": f"{SYSTEM_PROMPT} User: {prompt}", 
            "stream": False
        }, timeout=180)
        if r.status_code == 200:
            resp = r.json().get("response", "")
            # Ollama (deepseek-r1 и др.) иногда добавляет в начале "…response» —
            # если он залип в ответе, отрезаем до первого вхождения.
            if " response" in resp:
                resp = resp.split(" response", 1)[-1]
            return resp.strip()[:2500]
        return f"Error: Ollama returned {r.status_code}"
    except requests.exceptions.Timeout:
        return "Error: Ollama timeout"
    except Exception as e:
        return f"Error: {e}"


def check_auth(req):
    return req.headers.get("Authorization", "") == f"Bearer {API_TOKEN}"

@app.route("/")
def index(): return render_template("index.html")

@app.route("/api/devices")
def api_devices():
    if not check_auth(request): return jsonify({"err": "unauth"}), 401
    all_data = {}
    for f in DATA_DIR.glob("*.json"):
        did = f.stem
        try: all_data[did] = json.loads(f.read_text(encoding="utf-8"))
        except: pass
    all_data["_version"] = data_version["value"]
    return jsonify(all_data)

@app.route("/api/device/<mac>")
def api_device(mac):
    if not check_auth(request): return jsonify({"err": "unauth"}), 401
    return jsonify(load_dev(mac))

@app.route("/api/version")
def api_version():
    if not check_auth(request): return jsonify({"err": "unauth"}), 401
    return jsonify({"version": data_version["value"]})

@app.route("/api/sync", methods=["POST"])
def api_sync():
    """Принимает push от ESP или relay. СОХРАНЯЕТ ВСЁ (UI-изменения тоже)."""
    if not check_auth(request): return jsonify({"err": "unauth"}), 401
    d = request.json
    if not d or "device_id" not in d: return jsonify({"ok": False, "err": "no device_id"}), 400
    did = d.pop("device_id")
    dev = load_dev(did)
    # Сохраняем ВСЕ поля от любого клиента (ESP, UI, relay)
    for key in ("prompts", "notes", "messages", "notice", "ai_responses"):
        if key in d: dev[key] = d[key]
    dev["last_seen"] = int(time.time() * 1000)
    save_dev(did, dev)
    return jsonify({"ok": True, "version": data_version["value"]})

@app.route("/api/ai", methods=["POST"])
def api_ai():
    if not check_auth(request): return jsonify({"err": "unauth"}), 401
    d = request.json
    if not d or "prompt" not in d: return jsonify({"ok": False, "err": "empty"}), 400
    prompt = d["prompt"].strip()
    if not prompt: return jsonify({"ok": False, "err": "empty"}), 400
    device_id = d.get("device_id", "web")
    print(f"[AI] Device={device_id} Prompt={prompt[:50]}")
    resp = call_ollama(prompt)
    if device_id:
        dev = load_dev(device_id)
        if "ai_responses" not in dev: dev["ai_responses"] = []
        # Только если такого промпта еще нет
        if not any(r.get("prompt") == prompt for r in dev["ai_responses"]):
            dev["ai_responses"].append({"prompt": prompt, "response": resp, "time": int(time.time()*1000)})
            dev["ai_responses"] = dev["ai_responses"][-20:]
            save_dev(device_id, dev)
    return jsonify({"response": resp, "device_id": device_id})

if __name__ == "__main__":
    print("=" * 50)
    print("Control Panel: http://0.0.0.0:5000")
    print("Tailscale: tailscale funnel 5000")
    print("Token:", API_TOKEN)
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)
