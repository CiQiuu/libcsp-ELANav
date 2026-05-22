#!/usr/bin/env python3
"""
web_gs.py — ELANav Ground Station Web Interface

http://localhost:5000

Uso: python3 web_gs.py --device /dev/ttyACM0
"""

import subprocess
import threading
import argparse
import os
import re
import base64
import time
from flask import Flask, render_template_string, send_file
from flask_socketio import SocketIO, emit

# ── Configuration ────────────────────────────────────────────────────────────────
BASE_DIR    = os.path.dirname(os.path.abspath(__file__))
GS_BINARY   = os.path.join(BASE_DIR, "build/examples/csp_sfp_gs")
# El binario hace fopen() con ruta RELATIVA, por eso lanzamos el proceso con
# cwd=BASE_DIR y leemos los archivos desde ahí: así coinciden siempre.
IMAGE_OUT   = os.path.join(BASE_DIR, "gs_received_image.jpg")
VIDEO_OUT   = os.path.join(BASE_DIR, "gs_received_video.mp4")
DEFAULT_DEV = "/dev/ttyACM0"

app = Flask(__name__)
app.config["SECRET_KEY"] = "elanav2026"
sio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

state = {
    "connected": False,
    "gs_proc":   None,
    "device":    DEFAULT_DEV,
    "log":       [],
    "tele_buf":  None,   # acumulador del bloque TELEMETRIA OLYMPUS
}

def strip_ansi(s):
    return re.sub(r'\x1b\[[0-9;]*m', '', s)

def emit_log(msg, level="info"):
    entry = {"msg": msg, "level": level, "ts": time.time()}
    state["log"].append(entry)
    sio.emit("log", entry)

def parse_line(line: str):
    line = strip_ansi(line.strip())
    if not line:
        return

    # ── Captura del bloque de telemetría completa (comando 'd') ──────────
    if state["tele_buf"] is not None:
        if "fin telemetria" in line.lower():
            raw = "\n".join(state["tele_buf"])
            sio.emit("telemetry", {"raw": raw, "ts": time.time()})
            emit_log(f"Telemetría completa recibida ({len(raw)} chars).", level="ok")
            state["tele_buf"] = None
        else:
            state["tele_buf"].append(line)
            emit_log(line, level="info")
        return
    if "TELEMETRIA OLYMPUS" in line:
        state["tele_buf"] = []
        emit_log(line, level="ok")
        return

    level = "rdp" if ("RDP" in line or "Send CMP" in line or "Received in" in line) else "info"
    emit_log(line, level=level)

    if "CONNECTED" in line:
        state["connected"] = True
        sio.emit("status", {"connected": True})
    elif "DISCONNECTED" in line or "finalizado" in line.lower():
        state["connected"] = False
        sio.emit("status", {"connected": False})
    elif line.startswith("RESP:"):
        sio.emit("sensor", {"raw": line[5:].strip(), "ts": time.time()})
    elif "Guardada en" in line and "image" in line:
        _send_image()
    elif "Guardado en" in line and "video" in line:
        _send_video()
    elif "ERROR SFP" in line:
        emit_log(line, level="error")

def _send_image():
    # Pequeña espera + reintento: el print "Guardada en" puede salir antes
    # de que el flush a disco termine en algunas plataformas.
    for _ in range(5):
        if os.path.exists(IMAGE_OUT) and os.path.getsize(IMAGE_OUT) > 0:
            break
        time.sleep(0.2)
    if os.path.exists(IMAGE_OUT) and os.path.getsize(IMAGE_OUT) > 0:
        with open(IMAGE_OUT, "rb") as f:
            b64 = base64.b64encode(f.read()).decode()
        sio.emit("image", {"data": b64, "ts": time.time()})
        emit_log(f"Imagen recibida y enviada al browser ({os.path.getsize(IMAGE_OUT)} B).", level="ok")
    else:
        emit_log(f"Imagen no encontrada en {IMAGE_OUT}", level="error")

def _send_video():
    for _ in range(10):
        if os.path.exists(VIDEO_OUT) and os.path.getsize(VIDEO_OUT) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(VIDEO_OUT) and os.path.getsize(VIDEO_OUT) > 0:
        size = os.path.getsize(VIDEO_OUT)
        sio.emit("video", {"url": "/video", "size": size, "ts": time.time()})
        emit_log(f"Video recibido ({size} B). Disponible en /video", level="ok")
    else:
        emit_log(f"Video no encontrado en {VIDEO_OUT}", level="error")

def _reader_thread(proc):
    for raw in iter(proc.stdout.readline, b""):
        try:
            parse_line(raw.decode("utf-8", errors="replace"))
        except Exception as e:
            emit_log(f"[parser error] {e}", level="error")
    state["connected"] = False
    state["gs_proc"]   = None
    sio.emit("status", {"connected": False})
    emit_log("Proceso GS terminado.", level="warn")

@sio.on("connect")
def on_connect():
    emit("status", {"connected": state["connected"]})
    for entry in state["log"][-50:]:
        emit("log", entry)

@sio.on("start_gs")
def on_start(data):

    if state["gs_proc"] and state["gs_proc"].poll() is None:
        emit_log("GS ya está corriendo.", level="warn")
        return
    device = data.get("device", state["device"])
    state["device"] = device
    
    if not os.path.exists(GS_BINARY):
        emit_log(f"Binario no encontrado: {GS_BINARY}", level="error")
        return
    emit_log(f"Iniciando csp_sfp_gs en {device}…")
    
    try:
        proc = subprocess.Popen(
            [GS_BINARY, "-k", device],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
            cwd=BASE_DIR,
        )
        state["gs_proc"] = proc
        t = threading.Thread(target=_reader_thread, args=(proc,), daemon=True)
        t.start()
        emit_log("Proceso GS iniciado.", level="ok")
    except Exception as e:
        emit_log(f"Error al iniciar GS: {e}", level="error")

@sio.on("stop_gs")
def on_stop():
    proc = state["gs_proc"]
    if proc and proc.poll() is None:
        proc.terminate()
        emit_log("GS detenido.", level="warn")
    else:
        emit_log("GS no estaba corriendo.", level="warn")

@sio.on("cmd")
def on_cmd(data):
    proc = state["gs_proc"]
    
    if not proc or proc.poll() is not None:
        emit_log("GS no está corriendo.", level="error")
        return
    
    if proc.stdin is None:
        emit_log("stdin no disponible — reinicia el GS.", level="error")
        return
    cmd = data.get("cmd", "").strip()
    
    if not cmd:
        return
    try:
        proc.stdin.write((cmd + "\n").encode())
        proc.stdin.flush()
        emit_log(f"→ {cmd}", level="sent")
    except Exception as e:
        emit_log(f"Error enviando comando: {e}", level="error")

@sio.on("get_image")
def on_get_image():
    _send_image()

@app.route("/")
def index():
    html_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web_gs.html")
    return render_template_string(open(html_path).read())

@app.route("/image")
def image():
    if os.path.exists(IMAGE_OUT):
        return send_file(IMAGE_OUT, mimetype="image/jpeg")
    return "No image", 404

@app.route("/video")
def video():
    if os.path.exists(VIDEO_OUT):
        return send_file(VIDEO_OUT, mimetype="video/mp4")
    return "No video", 404

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ELANav GS Web Interface")
    parser.add_argument("--device", default=DEFAULT_DEV)
    parser.add_argument("--port", type=int, default=5000)
    args = parser.parse_args()
    state["device"] = args.device

    print(f"ELANav GS Web — http://localhost:{args.port}")
    print(f"Dispositivo:    {args.device}")
    print(f"Binario GS:     {GS_BINARY}")

    url = f"http://localhost:{args.port}"

    def _open_browser():
        time.sleep(1.2)
        try:
            subprocess.Popen(["firefox", "--new-window", url],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            try:
                subprocess.Popen(["xdg-open", url],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception:
                print(f"Abre manualmente: {url}")

    threading.Thread(target=_open_browser, daemon=True).start()
    sio.run(app, host="0.0.0.0", port=args.port, debug=False)
