#!/usr/bin/env python3
"""
stress_video.py — Prueba de estrés de transferencia de video sobre RF (ELANav)

Lanza su propio csp_sfp_gs, envía el comando 'v' en ciclos:
  enviar 'v' → esperar fin de transferencia → esperar N minutos → repetir

Cada ciclo se considera:
  ÉXITO  si aparece  "Guardado en gs_received_video.mp4"
  FALLO  si aparece  "ERROR SFP"  o se agota el watchdog del ciclo

Genera:
  - Log en vivo por consola con contadores ✓ / ✗
  - CSV con una fila por ciclo
  - Log completo de stdout del GS

Uso:
  python3 stress_video.py --device /dev/ttyACM0 --cycles 10 --rest 600

  --device   Dispositivo KISS del NinoTNC      (default /dev/ttyACM0)
  --cycles   Número de envíos de video         (default 10)
  --rest     Segundos de espera entre ciclos   (default 600 = 10 min)
  --timeout  Watchdog por ciclo en segundos    (default 21600 = 6 h)
  -t         packet_timeout_ms para csp_sfp_gs (default 5000 = C1)
  --md5      MD5 esperado del video original    (opcional; activa verificación)
"""

import subprocess
import threading
import argparse
import signal
import queue
import time
import csv
import os
import sys
import hashlib
from datetime import datetime

# ── Rutas ────────────────────────────────────────────────────────────────────
BASE_DIR  = os.path.dirname(os.path.abspath(__file__))
GS_BINARY = os.path.join(BASE_DIR, "build/examples/csp_sfp_gs")

# ── Marcadores que imprime el binario, por comando ────────────────────────────
# OJO: imagen imprime "GuardaDA" (jpg), video imprime "GuardaDO" (mp4).
CMD_PROFILES = {
    "v": {
        "mark_ok":   "Guardado en gs_received_video.mp4",
        "mark_size": "Video recibido:",      # "Video recibido: 2684090 bytes"
        "out_file":  "gs_received_video.mp4",
    },
    "i": {
        "mark_ok":   "Guardada en gs_received_image.jpg",
        "mark_size": "Imagen recibida:",     # "Imagen recibida: 2048 bytes"
        "out_file":  "gs_received_image.jpg",
    },
}
MARK_FAIL = "ERROR SFP"
MARK_TIME = "Tiempo de transferencia"    # "Tiempo de transferencia SFP: 13650.21 s"
MARK_GOOD = "Goodput:"                    # "Goodput: 196.67 B/s"

# ── Estado global ──────────────────────────────────────────────────────────────
stop_flag = threading.Event()
line_q    = queue.Queue()


def ts_now():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def reader_thread(proc, raw_log):
    """Lee stdout del GS línea por línea, lo encola y lo guarda al log crudo."""
    with open(raw_log, "a", buffering=1) as lf:
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            lf.write(f"{ts_now()}  {line}\n")
            line_q.put(line)
    line_q.put(None)  # señal de EOF (proceso terminó)


def drain_until(markers_ok, markers_fail, deadline, mark_size):
    """
    Consume líneas hasta encontrar un marcador de éxito o fallo, o hasta deadline.
    Devuelve dict con resultado y datos extraídos.
    """
    info = {"result": "TIMEOUT", "size": None, "elapsed": None, "goodput": None}
    while time.time() < deadline and not stop_flag.is_set():
        try:
            line = line_q.get(timeout=1.0)
        except queue.Empty:
            continue
        if line is None:
            info["result"] = "GS_DIED"
            return info
        # Extraer métricas si pasan
        if mark_size in line:
            try:
                info["size"] = int(line.split(mark_size)[1].split("bytes")[0].strip())
            except Exception:
                pass
        if MARK_TIME in line:
            try:
                seg = line.split(":")[1].strip()
                info["elapsed"] = float(seg.split("s")[0].strip())
            except Exception:
                pass
        if MARK_GOOD in line:
            try:
                info["goodput"] = float(line.split(MARK_GOOD)[1].split("B/s")[0].strip())
            except Exception:
                pass
        # Marcadores de fin
        if any(m in line for m in markers_ok):
            info["result"] = "OK"
            return info
        if any(m in line for m in markers_fail):
            info["result"] = "FAIL"
            return info
    return info


def main():
    ap = argparse.ArgumentParser(description="Prueba de estrés de video RF — ELANav")
    ap.add_argument("--device",  default="/dev/ttyACM0")
    ap.add_argument("--cycles",  type=int, default=10)
    ap.add_argument("--rest",    type=int, default=600,    help="segundos entre ciclos")
    ap.add_argument("--timeout", type=int, default=21600,  help="watchdog por ciclo (s)")
    ap.add_argument("-t",        type=int, default=5000, dest="ptmo",
                    help="packet_timeout_ms para csp_sfp_gs")
    ap.add_argument("--cmd",     default="i", choices=["v", "i"],
                    help="comando a enviar en cada ciclo: v=video, i=imagen")
    ap.add_argument("--md5",     default=None, help="MD5 esperado del archivo original")
    args = ap.parse_args()

    prof      = CMD_PROFILES[args.cmd]
    MARK_OK   = prof["mark_ok"]
    MARK_SIZE = prof["mark_size"]
    OUT_FILE  = os.path.join(BASE_DIR, prof["out_file"])

    if not os.path.exists(GS_BINARY):
        print(f"ERROR: binario no encontrado: {GS_BINARY}")
        sys.exit(1)

    stamp    = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_dir  = os.path.join(BASE_DIR, "logs", f"stress_video_{stamp}")
    os.makedirs(log_dir, exist_ok=True)
    csv_path = os.path.join(log_dir, "ciclos.csv")
    raw_log  = os.path.join(log_dir, "gs_stdout.log")

    # ── Lanzar el GS ────────────────────────────────────────────────────────
    nombre = {"v": "VIDEO", "i": "IMAGEN"}[args.cmd]
    print("=" * 64)
    print(f"  PRUEBA DE ESTRÉS DE {nombre} RF — ELANav")
    print(f"  Inicio:    {ts_now()}")
    print(f"  Comando:   '{args.cmd}'  → archivo {prof['out_file']}")
    print(f"  Device:    {args.device}   packet_timeout_ms={args.ptmo}")
    print(f"  Ciclos:    {args.cycles}    descanso={args.rest}s ({args.rest/60:.0f} min)")
    print(f"  Watchdog:  {args.timeout}s ({args.timeout/3600:.1f} h) por ciclo")
    print(f"  Logs:      {log_dir}")
    if args.md5:
        print(f"  MD5 ref:   {args.md5}")
    print("=" * 64)

    proc = subprocess.Popen(
        [GS_BINARY, "-k", args.device, "-t", str(args.ptmo)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        cwd=BASE_DIR,
    )

    t = threading.Thread(target=reader_thread, args=(proc, raw_log), daemon=True)
    t.start()

    # Manejo de Ctrl+C → cierre limpio
    def handle_sigint(signum, frame):
        print("\n[!] Señal recibida, cerrando limpio…")
        stop_flag.set()
    signal.signal(signal.SIGINT,  handle_sigint)
    signal.signal(signal.SIGTERM, handle_sigint)

    # Dar tiempo al GS a inicializar (banner + router)
    time.sleep(3)

    ok_count   = 0
    fail_count = 0

    with open(csv_path, "w", newline="") as cf:
        w = csv.writer(cf)
        w.writerow(["ciclo", "inicio", "fin", "duracion_s",
                    "resultado", "bytes", "elapsed_sfp_s", "goodput_Bps",
                    "md5_match", "exitosos_acum", "fallidos_acum"])

        for cyc in range(1, args.cycles + 1):
            if stop_flag.is_set():
                break

            t_ini   = time.time()
            ini_str = ts_now()
            print(f"\n── Ciclo {cyc}/{args.cycles} ─ inicio {ini_str} "
                  f"─ ✓{ok_count} ✗{fail_count}")

            # Borrar archivo anterior para no contar un MD5 viejo como éxito
            try:
                if os.path.exists(OUT_FILE):
                    os.remove(OUT_FILE)
            except OSError:
                pass

            # Enviar comando ('v' o 'i')
            try:
                proc.stdin.write((args.cmd + "\n").encode())
                proc.stdin.flush()
            except Exception as e:
                print(f"   [ERROR] no se pudo escribir al GS: {e}")
                fail_count += 1
                w.writerow([cyc, ini_str, ts_now(), 0, "WRITE_ERROR",
                            "", "", "", "", ok_count, fail_count])
                cf.flush()
                break

            print(f"   → comando '{args.cmd}' enviado, esperando transferencia…")

            # Esperar resultado
            deadline = t_ini + args.timeout
            info = drain_until([MARK_OK], [MARK_FAIL], deadline, MARK_SIZE)

            t_fin    = time.time()
            fin_str  = ts_now()
            dur      = t_fin - t_ini

            # Verificación MD5 (si aplica y hubo éxito)
            md5_match = ""
            if info["result"] == "OK":
                if args.md5 and os.path.exists(OUT_FILE):
                    got = md5_of(OUT_FILE)
                    md5_match = "SI" if got == args.md5 else f"NO({got[:8]})"
                    if md5_match != "SI":
                        print(f"   [!] MD5 NO coincide: {got}")
                        info["result"] = "OK_MD5_FAIL"
                ok_count += 1
                print(f"   ✓ ÉXITO en {dur/60:.1f} min  "
                      f"({info['size']} B, {info['goodput']} B/s)"
                      f"{'  MD5✓' if md5_match=='SI' else ''}")
            else:
                fail_count += 1
                print(f"   ✗ FALLO [{info['result']}] tras {dur/60:.1f} min")

            w.writerow([cyc, ini_str, fin_str, f"{dur:.1f}",
                        info["result"], info["size"] or "",
                        info["elapsed"] or "", info["goodput"] or "",
                        md5_match, ok_count, fail_count])
            cf.flush()

            # Descanso entre ciclos (salvo en el último)
            if cyc < args.cycles and not stop_flag.is_set():
                print(f"   … descanso {args.rest/60:.0f} min "
                      f"(reanuda ~{datetime.fromtimestamp(time.time()+args.rest).strftime('%H:%M:%S')})")
                slept = 0
                while slept < args.rest and not stop_flag.is_set():
                    time.sleep(min(5, args.rest - slept))
                    slept += 5

    # ── Cierre ──────────────────────────────────────────────────────────────
    print("\n" + "=" * 64)
    print(f"  RESUMEN — {ts_now()}")
    print(f"  Ciclos completados: {ok_count + fail_count}/{args.cycles}")
    print(f"  ✓ Exitosos: {ok_count}")
    print(f"  ✗ Fallidos: {fail_count}")
    if ok_count + fail_count > 0:
        tasa = 100.0 * ok_count / (ok_count + fail_count)
        print(f"  Tasa de éxito: {tasa:.1f}%")
    print(f"  CSV: {csv_path}")
    print("=" * 64)

    # Salir del GS limpio
    try:
        proc.stdin.write(b"q\n")
        proc.stdin.flush()
        proc.wait(timeout=10)
    except Exception:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
