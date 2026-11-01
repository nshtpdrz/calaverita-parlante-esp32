# =====================================================
#  CEREBRO DE LA CALAVERA (corre en tu PC, gratis y sin límite)
#
#  La calavera manda por WiFi lo que escuchó  ->  aquí:
#    1) Whisper lo convierte en texto
#    2) Ollama (modelo de chat local) inventa la respuesta
#    3) Piper la convierte en voz (y le ponemos voz grave + eco)
#  ->  la calavera recibe el audio y lo dice.
#
#  Cómo usarlo:  doble clic en iniciar_cerebro.bat
#  (la PC y la calavera deben estar en el MISMO WiFi)
# =====================================================
import difflib
import os
import io
import json
import re
import socket
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np

# ================== AJUSTES ==================
PUERTO = 8000            # la calavera se conecta aquí
PUERTO_AVISO = 4210      # por aquí la PC grita "¡aquí estoy!" para que la calavera la encuentre
PALABRA_CLAVE = "calavera"
MODELO_CHAT = "gemma3:4b"     # más rápido pero menos gracioso: "llama3.2:3b" o "qwen2.5:3b"
MODELO_WHISPER = "base"       # "small" = entiende mejor pero ~4x más lento
VOZ_PIPER = "es_MX-claude-high"
TONO = 0.85              # < 1 = voz más grave y lenta (0.75 muy tenebrosa, 1.0 normal)
ECO = 0.35               # 0 = sin eco
RECORDAR_TURNOS = 4      # cuántas preguntas anteriores recuerda (para platicar)

PERSONALIDAD = (
    "Eres una calavera animatronica de Halloween llamada Calavera. "
    "Eres tenebrosa, sarcastica y graciosa, pero amable con los ninos. "
    "Respondes en espanol de Mexico con maximo 2 frases cortas (menos de 30 palabras), "
    "con humor de ultratumba. Nunca uses emojis, asteriscos, listas ni comillas."
)

CARPETA = Path(__file__).resolve().parent
CARPETA_VOCES = CARPETA / "voces"
OLLAMA = "http://127.0.0.1:11434"


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ================== 1) OÍDO: Whisper ==================
log("Cargando Whisper (la primera vez lo descarga, ~500 MB)...")
from faster_whisper import WhisperModel  # noqa: E402

whisper = WhisperModel(MODELO_WHISPER, device="cpu", compute_type="int8", cpu_threads=os.cpu_count())


def escuchar(wav_bytes):
    with wave.open(io.BytesIO(wav_bytes)) as w:
        frec = w.getframerate()
        audio = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    if frec != 16000:  # Whisper quiere 16 kHz
        n = int(len(audio) * 16000 / frec)
        audio = np.interp(np.linspace(0, len(audio) - 1, n), np.arange(len(audio)), audio).astype(np.float32)
    pico = float(np.max(np.abs(audio))) if len(audio) else 0
    if pico > 0.001:
        audio = audio * min(0.9 / pico, 20)  # sube el volumen si llegó bajito
    audio = np.concatenate([np.zeros(6400, np.float32), audio])  # silencio al inicio: no se come la 1a palabra
    trozos, _ = whisper.transcribe(audio, language="es", beam_size=1, without_timestamps=True,
                                   initial_prompt="Los niños le hablan a Calavera, la calavera de Halloween.")
    return " ".join(t.text for t in trozos).strip()


def sin_acentos(t):
    return "".join(c for c in unicodedata.normalize("NFD", t.lower()) if unicodedata.category(c) != "Mn")


def me_hablan(texto):
    palabras = re.findall(r"[a-z]+", sin_acentos(texto))
    # junta de 1 a 3 palabras seguidas: "a la vera" -> "alavera" ~ "calavera"
    for n in (1, 2, 3):
        for i in range(len(palabras) - n + 1):
            junta = "".join(palabras[i:i + n])
            if difflib.SequenceMatcher(None, junta, PALABRA_CLAVE).ratio() >= 0.75:
                return True
    return False


CARPETA_GRAB = CARPETA / "grabaciones"


def guardar_grabacion(wav_bytes):
    """Guarda las últimas 5 grabaciones para revisar cómo se oye el micrófono."""
    CARPETA_GRAB.mkdir(exist_ok=True)
    for i in range(4, 0, -1):
        viejo = CARPETA_GRAB / f"grabacion_{i}.wav"
        if viejo.exists():
            viejo.replace(CARPETA_GRAB / f"grabacion_{i + 1}.wav")
    (CARPETA_GRAB / "grabacion_1.wav").write_bytes(wav_bytes)
    with wave.open(io.BytesIO(wav_bytes)) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32)
    if len(a):
        recortes = 100 * np.mean(np.abs(a) > 32000)
        return f"volumen pico {int(np.max(np.abs(a)))}/32767, promedio {int(np.sqrt(np.mean(a ** 2)))}, saturado {recortes:.1f}%"
    return "vacía"


# ================== 2) CEREBRO: Ollama ==================
historial = []


def ollama(ruta, datos=None, espera=120):
    req = urllib.request.Request(OLLAMA + ruta, data=json.dumps(datos).encode() if datos is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=espera) as r:
        return json.loads(r.read())


def pensar(pregunta):
    mensajes = [{"role": "system", "content": PERSONALIDAD}] + historial + [{"role": "user", "content": pregunta}]
    r = ollama("/api/chat", {
        "model": MODELO_CHAT, "messages": mensajes, "stream": False, "keep_alive": -1,
        "options": {"num_predict": 90, "temperature": 0.9},
    })
    texto = r["message"]["content"]
    texto = re.sub(r"[*_#\"“”«»]|[\U0001F000-\U0001FFFF☀-➿]", "", texto)
    texto = re.sub(r"\s+", " ", texto).strip()
    historial.extend([{"role": "user", "content": pregunta}, {"role": "assistant", "content": texto}])
    del historial[:-2 * RECORDAR_TURNOS]
    return texto


# ================== 3) VOZ: Piper ==================
def bajar_voz():
    CARPETA_VOCES.mkdir(exist_ok=True)
    idioma, region_nombre = VOZ_PIPER.split("_", 1)[0], VOZ_PIPER.split("-")[0]
    nombre, calidad = VOZ_PIPER.split("-")[1], VOZ_PIPER.split("-")[2]
    base = f"https://huggingface.co/rhasspy/piper-voices/resolve/main/{idioma}/{region_nombre}/{nombre}/{calidad}/{VOZ_PIPER}"
    for ext in (".onnx", ".onnx.json"):
        destino = CARPETA_VOCES / (VOZ_PIPER + ext)
        if not destino.exists():
            log(f"Descargando voz {destino.name}...")
            urllib.request.urlretrieve(base + ext, destino)
    return CARPETA_VOCES / (VOZ_PIPER + ".onnx")


log("Cargando la voz de Piper...")
from piper import PiperVoice  # noqa: E402

voz = PiperVoice.load(str(bajar_voz()))
FREC_VOZ = voz.config.sample_rate


def hablar(texto):
    try:  # piper-tts 1.3 o más nuevo
        from piper import SynthesisConfig
        cfg = SynthesisConfig(length_scale=1.1)
        pcm = b"".join(c.audio_int16_bytes for c in voz.synthesize(texto, syn_config=cfg))
    except ImportError:  # versiones viejas
        pcm = b"".join(voz.synthesize_stream_raw(texto, length_scale=1.1))
    a = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    frec = int(FREC_VOZ * TONO)  # reproducir más lento = voz grave
    if ECO > 0:
        d = int(frec * 0.13)
        a = np.concatenate([a, np.zeros(d * 2, np.float32)])
        a[d:] += ECO * a[:-d]
        a[2 * d:] += ECO * 0.5 * a[:-2 * d]
    a = a * (26000 / max(1.0, float(np.max(np.abs(a)))))
    return a.astype("<i2").tobytes(), frec


# ================== SERVIDOR WEB ==================
class Calavera(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def responder_audio(self, pcm, frec, texto):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(pcm)))
        self.send_header("X-Frecuencia", str(frec))
        self.send_header("X-Texto", urllib.parse.quote(texto[:200]))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(pcm)

    def responder_vacio(self, codigo, texto=""):
        self.send_response(codigo)
        self.send_header("Content-Length", "0")
        self.send_header("X-Texto", urllib.parse.quote(texto[:200]))
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/probar"):
            texto = "Hola mortal, soy Calavera. Bienvenido a mi cripta."
            pcm, frec = hablar(texto)
            return self.responder_audio(pcm, frec, texto)
        cuerpo = b"La calavera esta despierta\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(cuerpo)))
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_POST(self):
        t0 = time.time()
        wav = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            log("Grabación recibida:", guardar_grabacion(wav))
            oido = escuchar(wav)
            t1 = time.time()
            if not oido or not me_hablan(oido):
                log(f"Escuché: '{oido}'  (no dijo '{PALABRA_CLAVE}', la ignoro)  [{t1 - t0:.1f}s]")
                return self.responder_vacio(204, oido)
            log(f"Escuché: '{oido}'  [{t1 - t0:.1f}s]")
            respuesta = pensar(oido)
            t2 = time.time()
            log(f"Respondo: '{respuesta}'  [{t2 - t1:.1f}s]")
            pcm, frec = hablar(respuesta)
            log(f"Voz lista: {len(pcm) / 2 / frec:.1f}s de audio  [{time.time() - t2:.1f}s]  total {time.time() - t0:.1f}s")
            self.responder_audio(pcm, frec, respuesta)
        except Exception as e:
            log("ERROR:", repr(e))
            self.responder_vacio(500, str(e))


# ================== AVISO EN LA RED ==================
def mis_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return [ip for ip in ips if not ip.startswith("127.")]


def avisar():
    """Cada 2 s grita en cada red: la calavera así encuentra la IP de la PC sola."""
    mensaje = f"CALAVERA_PC {PUERTO}".encode()
    while True:
        for ip in mis_ips():
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    s.bind((ip, 0))
                    s.sendto(mensaje, (ip.rsplit(".", 1)[0] + ".255", PUERTO_AVISO))
                    s.sendto(mensaje, ("255.255.255.255", PUERTO_AVISO))
            except OSError:
                pass
        time.sleep(2)


# ================== ARRANQUE ==================
def preparar_ollama():
    try:
        ollama("/api/tags", espera=5)
    except Exception:
        log("!! Ollama no responde. Ábrelo (busca 'Ollama' en el menú Inicio) y vuelve a correr esto.")
        sys.exit(1)
    modelos = [m["name"] for m in ollama("/api/tags")["models"]]
    if not any(m == MODELO_CHAT or m.split(":")[0] == MODELO_CHAT for m in modelos):
        log(f"Descargando el modelo {MODELO_CHAT} (solo la primera vez, unos GB)...")
        req = urllib.request.Request(OLLAMA + "/api/pull", data=json.dumps({"model": MODELO_CHAT}).encode())
        with urllib.request.urlopen(req, timeout=3600) as r:
            ultimo = ""
            for linea in r:
                estado = json.loads(linea).get("status", "")
                if estado != ultimo and not estado.startswith("pulling "):
                    log("  ", estado)
                ultimo = estado
    log("Despertando el modelo de chat...")
    ollama("/api/chat", {"model": MODELO_CHAT, "messages": [{"role": "user", "content": "hola"}],
                         "stream": False, "keep_alive": -1, "options": {"num_predict": 1}}, espera=300)


if __name__ == "__main__":
    preparar_ollama()
    hablar("listo")  # calienta la voz
    threading.Thread(target=avisar, daemon=True).start()
    log(f"Cerebro listo en {', '.join(mis_ips())} puerto {PUERTO}. Prende la calavera.")
    log("(deja esta ventana abierta; ciérrala para apagar el cerebro)")
    ThreadingHTTPServer(("0.0.0.0", PUERTO), Calavera).serve_forever()
