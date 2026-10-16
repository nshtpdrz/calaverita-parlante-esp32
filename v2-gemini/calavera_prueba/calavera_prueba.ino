// =====================================================
//  CALAVERA PARLANTE "tipo Alexa" para Halloween
//  ESP32 clásico (ESP32 Dev Module)
//
//  - Ojos animados en 2 OLED 0.91" (128x32)
//  - Escucha con el micrófono INMP441
//  - Le dices: "Calavera, ¿cómo te llamas?"  ->  graba tu frase,
//    la manda por WiFi a Google Gemini (GRATIS), que la entiende,
//    inventa una respuesta tenebrosa y la convierte en voz.
//  - Si la frase NO empieza/menciona "Calavera", la ignora.
//  - Mientras piensa: ojos mirando arriba y LED morado.
//  - El LED RGB brilla en rojo al ritmo de la voz (como boca)
//  - Sin WiFi: funciona como antes (hola / risa).
//
//  QUÉ NECESITAS:
//   1) Clave gratis de Gemini: https://aistudio.google.com/apikey
//   2) Poner tu WiFi y la clave abajo en "WIFI + GEMINI"
//
//  Librerías: Adafruit SSD1306 + Adafruit GFX
//  Placa: "ESP32 Dev Module"
// =====================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <esp_idf_version.h>
#include "audios.h"
#include "secrets.h"   // copia secrets.example.h como secrets.h

// ================== WIFI + GEMINI ==================
#define MODELO_CEREBRO "gemini-flash-latest"           // entiende tu voz y piensa la respuesta
#define MODELO_VOZ     "gemini-3.1-flash-tts-preview"  // si da error 404, prueba "gemini-2.5-flash-preview-tts"
#define VOZ            "Algenib"                       // voces graves: Algenib, Charon, Fenrir, Orus
#define PALABRA_CLAVE  "Calavera"

// Personalidad de la calavera (no uses comillas dobles aquí)
#define PERSONALIDAD \
  "Eres una calavera animatronica de Halloween llamada " PALABRA_CLAVE ". " \
  "Eres tenebrosa, sarcastica y graciosa, pero amable con los ninos. " \
  "Vas a escuchar una grabacion de audio de alguien cerca de ti. " \
  "Si en la grabacion NO te hablan a ti usando la palabra " PALABRA_CLAVE " (o algo que suene muy parecido), " \
  "responde exactamente: NADA. " \
  "Si si te hablan, responde en espanol con maximo 2 frases cortas (menos de 30 palabras), " \
  "con humor de ultratumba. No uses emojis, asteriscos, ni comillas."
#define ESTILO_VOZ "Dilo en espanol con voz grave, lenta, ronca y tenebrosa, como una calavera de Halloween"

// ================== PINES ==================
// Ojos
#define SDA_IZQ 26
#define SCL_IZQ 27
#define SDA_DER 21
#define SCL_DER 22
// Amplificador MAX98357A
#define AMP_BCLK 32
#define AMP_LRC  25
#define AMP_DIN  33
// Micrófono INMP441
#define MIC_SCK 14
#define MIC_WS  15
#define MIC_SD  35
// LED RGB (si los colores salen cambiados, intercambia estos números)
#define LED_R 4
#define LED_G 12
#define LED_B 13
#define LED_ANODO_COMUN false   // pon true si tu LED se enciende al revés

// ================== AJUSTES ==================
#define VOLUMEN        0.8   // 0.0 a 1.0
#define SENSIBILIDAD   3.0f  // más bajo = más sensible al ruido
#define NIVEL_MINIMO   300   // nivel mínimo para considerar que alguien habla
#define MIC_CANAL      I2S_CHANNEL_FMT_ONLY_LEFT  // si el nivel nunca cambia, prueba ONLY_RIGHT
#define FRECUENCIA     16000
#define GANANCIA_MIC   2     // sube a 3 o 4 si Gemini no te entiende (voz muy bajita)
#define FREC_GRAB      8000  // la grabación que se manda a Gemini
#define SEG_GRABACION  5     // máximo de segundos por pregunta
#define FREC_VOZ_GEMINI 24000

// ================== OJOS ==================
Adafruit_SSD1306 pantIzq(128, 32, &Wire,  -1);
Adafruit_SSD1306 pantDer(128, 32, &Wire1, -1);

enum Expresion { NORMAL, ENOJADO, FELIZ, SOSPECHOSO, SORPRENDIDO, DORMIDO };
struct Ojo { Adafruit_SSD1306* d; bool esIzquierdo; };
Ojo ojoIzq = { &pantIzq, true  };
Ojo ojoDer = { &pantDer, false };

Expresion expresion = NORMAL;
float posX = 0, objetivoX = 0, posY = 0, objetivoY = 0;
float apertura = 1.0;
bool  modoAuto = true;
unsigned long inicioParpadeo = 0, proximoParpadeo = 2000, proximaMirada = 1000, finControlManual = 0;

// ================== ESTADO COMPARTIDO (audio <-> ojos) ==================
const int16_t* volatile clipPedido = nullptr;
volatile uint32_t clipPedidoLen = 0;
volatile bool  calaveraHablando = false;
volatile bool  calaveraPensando = false;
volatile bool  personaHablando  = false;
volatile bool  pruebaGemini     = false;
volatile float nivelBoca = 0;           // 0..1, para el LED
volatile float nivelMic = 0, nivelRuido = 200;
volatile bool  mostrarNivel = false;

void decir(const int16_t* clip, uint32_t len) {
  if (calaveraHablando) return;
  clipPedidoLen = len;
  clipPedido = clip;
}

// ================== FUNCIONES DE OJOS ==================
void ponerExpresion(Expresion e) { expresion = e; }
void mirarA(float x, float y = 0, unsigned long ms = 3000) {
  objetivoX = constrain(x, -1.0, 1.0);
  objetivoY = constrain(y, -1.0, 1.0);
  finControlManual = millis() + ms;
}
void parpadear() { inicioParpadeo = millis(); }

void dibujarOjo(Ojo& o) {
  Adafruit_SSD1306& d = *o.d;
  d.clearDisplay();
  int ancho = 44, alto = 26;
  if (expresion == SORPRENDIDO) { ancho = 34; alto = 30; }
  if (expresion == SOSPECHOSO)  { alto = 12; }
  int h = (int)(alto * apertura);
  if (expresion == DORMIDO) h = 2;
  if (h < 2) h = 2;
  int cx = 64 + (int)(posX * 38);
  int cy = 16 + (int)(posY * 2);
  int x = cx - ancho / 2, y = cy - h / 2;
  int r = min(8, h / 2);
  d.fillRoundRect(x, y, ancho, h, r, SSD1306_WHITE);
  if (expresion == ENOJADO) {
    int interior = o.esIzquierdo ? x + ancho : x;
    int exterior = o.esIzquierdo ? x : x + ancho;
    d.fillTriangle(exterior, y - 1, interior, y - 1, interior, y + (int)(h * 0.6), SSD1306_BLACK);
  }
  if (expresion == FELIZ) d.fillRoundRect(x - 2, y + h / 2, ancho + 4, h, r, SSD1306_BLACK);
  d.display();
}

// ================== LED ==================
void ponerLed(int r, int g, int b) {
  if (LED_ANODO_COMUN) { r = 255 - r; g = 255 - g; b = 255 - b; }
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}

// ================== I2S ==================
void configurarI2S() {
  // ---- Micrófono: puerto 0, recepción ----
  i2s_config_t mic = {};
  mic.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  mic.sample_rate = FRECUENCIA;
  mic.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  mic.channel_format = MIC_CANAL;
  mic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  mic.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  mic.dma_buf_count = 4;
  mic.dma_buf_len = 256;
  i2s_driver_install(I2S_NUM_0, &mic, 0, NULL);

  i2s_pin_config_t pm = {};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pm.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pm.bck_io_num = MIC_SCK;
  pm.ws_io_num = MIC_WS;
  pm.data_out_num = I2S_PIN_NO_CHANGE;
  pm.data_in_num = MIC_SD;
  i2s_set_pin(I2S_NUM_0, &pm);

  // ---- Bocina: puerto 1, transmisión ----
  i2s_config_t amp = {};
  amp.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  amp.sample_rate = FRECUENCIA;
  amp.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  amp.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  amp.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  amp.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  amp.dma_buf_count = 16;          // colchón grande para que la voz de internet no se corte
  amp.dma_buf_len = 512;
  amp.tx_desc_auto_clear = true;   // silencio cuando no hay audio (evita zumbido)
  i2s_driver_install(I2S_NUM_1, &amp, 0, NULL);

  i2s_pin_config_t pa = {};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pa.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pa.bck_io_num = AMP_BCLK;
  pa.ws_io_num = AMP_LRC;
  pa.data_out_num = AMP_DIN;
  pa.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &pa);
  i2s_zero_dma_buffer(I2S_NUM_1);
}

// ================== REPRODUCIR ==================
// Manda un pedazo de audio mono a la bocina y mueve la "boca"
void sonarBloque(const int16_t* bloque, uint32_t n) {
  static int16_t salida[512];
  int pico = 0;
  for (uint32_t k = 0; k < n; k++) {
    int s = (int)(bloque[k] * VOLUMEN);
    salida[2 * k] = salida[2 * k + 1] = s;    // mismo sonido en L y R
    if (abs(s) > pico) pico = abs(s);
  }
  float objetivo = min(1.0f, pico / 12000.0f);
  nivelBoca = nivelBoca * 0.5f + objetivo * 0.5f;
  size_t escritos;
  i2s_write(I2S_NUM_1, salida, n * 4, &escritos, portMAX_DELAY);
}

void reproducir(const int16_t* clip, uint32_t len) {
  calaveraHablando = true;
  for (uint32_t i = 0; i < len; i += 256) sonarBloque(clip + i, min((uint32_t)256, len - i));
  i2s_zero_dma_buffer(I2S_NUM_1);
  nivelBoca = 0;
  calaveraHablando = false;
}

// ================== GRABACIÓN ==================
#define MAX_GRAB (FREC_GRAB * SEG_GRABACION)
#define PRE_GRAB (FREC_GRAB * 4 / 10)   // 0.4 s antes de que se detecte la voz
int16_t* grab = nullptr;                // se reserva en setup()
uint32_t grabLen = 0;
int16_t  anillo[PRE_GRAB];              // guarda siempre lo último que se oyó
uint32_t anilloPos = 0;

void guardarEnAnillo(const int16_t* b, int n) {
  for (int i = 0; i < n; i++) { anillo[anilloPos] = b[i]; anilloPos = (anilloPos + 1) % PRE_GRAB; }
}
void empezarGrabacion() {
  grabLen = 0;
  for (uint32_t i = 0; i < PRE_GRAB; i++) grab[grabLen++] = anillo[(anilloPos + i) % PRE_GRAB];
}
void agregarAGrabacion(const int16_t* b, int n) {
  for (int i = 0; i < n && grabLen < MAX_GRAB; i++) grab[grabLen++] = b[i];
}

// ================== INTERNET (Gemini) ==================
#define SERVIDOR "generativelanguage.googleapis.com"
WiFiClientSecure cliente;
uint8_t  bufRed[1024];
int      bufPos = 0, bufLen = 0;
long     trozoRestante = 0;
bool     esChunked = false, cuerpoFin = false;

bool hayWifi() { return WiFi.status() == WL_CONNECTED; }
bool wifiConfigurado() { return strcmp(WIFI_NOMBRE, "TU_WIFI") != 0 && strcmp(GEMINI_CLAVE, "TU_CLAVE_DE_GEMINI") != 0; }

// Lee un byte crudo de la conexión (-1 = se cortó / tardó mucho)
int leerCrudo() {
  if (bufPos < bufLen) return bufRed[bufPos++];
  unsigned long t0 = millis();
  for (;;) {
    int n = cliente.available();
    if (n > 0) {
      bufLen = cliente.read(bufRed, min(n, (int)sizeof(bufRed)));
      bufPos = 0;
      if (bufLen > 0) return bufRed[bufPos++];
    } else if (!cliente.connected() || millis() - t0 > 25000) {
      return -1;
    } else {
      delay(2);
    }
  }
}

String leerLinea() {
  String l;
  for (;;) {
    int c = leerCrudo();
    if (c < 0 || c == '\n') break;
    if (c != '\r' && l.length() < 300) l += (char)c;
  }
  return l;
}

// Lee un byte del cuerpo de la respuesta (entiende "chunked")
int leerCuerpo() {
  if (cuerpoFin) return -1;
  if (esChunked && trozoRestante == 0) {
    String linea = leerLinea();
    if (linea.length() == 0) linea = leerLinea();   // fin del trozo anterior
    trozoRestante = strtol(linea.c_str(), NULL, 16);
    if (trozoRestante <= 0) { cuerpoFin = true; return -1; }
  }
  int b = leerCrudo();
  if (b < 0) { cuerpoFin = true; return -1; }
  if (esChunked) trozoRestante--;
  return b;
}

bool abrirPeticion(const char* modelo, size_t largo) {
  cliente.stop();
  bufPos = bufLen = 0; trozoRestante = 0; esChunked = false; cuerpoFin = false;
  if (!hayWifi()) { Serial.println("No pude conectar con Gemini: WiFi desconectado"); return false; }

  // 1) Buscar la dirección del servidor (DNS)
  IPAddress ip;
  if (!WiFi.hostByName(SERVIDOR, ip) || ip == IPAddress(0, 0, 0, 0)) {
    Serial.println("DNS del router falló, cambiando a DNS de Google (8.8.8.8)...");
    WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    delay(200);
    if (!WiFi.hostByName(SERVIDOR, ip) || ip == IPAddress(0, 0, 0, 0)) {
      Serial.println("No pude conectar con Gemini: no encuentro el servidor (DNS)");
      return false;
    }
  }

  // 2) Conexión segura (HTTPS)
  if (!cliente.connect(SERVIDOR, 443)) {
    char err[100];
    int codigo = cliente.lastError(err, sizeof(err));
    Serial.printf("No pude conectar con Gemini (%s): error %d %s | memoria libre %u, bloque mayor %u\n",
                  ip.toString().c_str(), codigo, err, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  cliente.printf("POST /v1beta/models/%s:generateContent HTTP/1.1\r\n", modelo);
  cliente.print("Host: " SERVIDOR "\r\n");
  cliente.print("x-goog-api-key: " GEMINI_CLAVE "\r\n");
  cliente.print("Content-Type: application/json\r\n");
  cliente.printf("Content-Length: %u\r\n", (unsigned)largo);
  cliente.print("Connection: close\r\n\r\n");
  return true;
}

int leerEstado() {
  String l = leerLinea();                 // "HTTP/1.1 200 OK"
  int estado = l.length() > 12 ? l.substring(9, 12).toInt() : -1;
  for (;;) {
    l = leerLinea();
    if (l.length() == 0) break;
    l.toLowerCase();
    if (l.startsWith("transfer-encoding:") && l.indexOf("chunked") >= 0) esChunked = true;
  }
  return estado;
}

void mostrarError(int estado) {
  Serial.printf("Gemini respondió error %d:\n", estado);
  for (int i = 0; i < 600; i++) { int c = leerCuerpo(); if (c < 0) break; Serial.write(c); }
  Serial.println();
  cliente.stop();
}

// --- Base64 ---
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int valorB64(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// La grabación como archivo WAV, byte por byte
uint8_t cabeceraWav[44];
void armarCabeceraWav() {
  uint32_t datos = grabLen * 2;
  uint32_t v[] = { 0x46464952, 36 + datos, 0x45564157, 0x20746d66, 16,
                   0x00010001, FREC_GRAB, FREC_GRAB * 2, 0x00100002, 0x61746164, datos };
  memcpy(cabeceraWav, v, 44);   // "RIFF....WAVEfmt " ... "data"
}
uint8_t byteWav(uint32_t i) {
  if (i < 44) return cabeceraWav[i];
  i -= 44;
  int16_t s = grab[i / 2];
  return (i & 1) ? (uint8_t)(s >> 8) : (uint8_t)s;
}

// Manda la grabación a Gemini. Devuelve el texto de la respuesta
// (tal cual viene en el JSON) o "" si hubo error.
String preguntarAGemini() {
  armarCabeceraWav();
  uint32_t totalWav = 44 + grabLen * 2;
  uint32_t largoB64 = (totalWav + 2) / 3 * 4;
  String inicio = "{\"systemInstruction\":{\"parts\":[{\"text\":\"" PERSONALIDAD "\"}]},"
                  "\"contents\":[{\"role\":\"user\",\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  String fin = "\"}}]}]}";
  if (!abrirPeticion(MODELO_CEREBRO, inicio.length() + largoB64 + fin.length())) return "";

  cliente.print(inicio);
  char trozo[1024];
  int p = 0;
  for (uint32_t i = 0; i < totalWav; i += 3) {
    uint32_t a = byteWav(i);
    uint32_t b = i + 1 < totalWav ? byteWav(i + 1) : 0;
    uint32_t c = i + 2 < totalWav ? byteWav(i + 2) : 0;
    uint32_t x = (a << 16) | (b << 8) | c;
    trozo[p++] = B64[(x >> 18) & 63];
    trozo[p++] = B64[(x >> 12) & 63];
    trozo[p++] = i + 1 < totalWav ? B64[(x >> 6) & 63] : '=';
    trozo[p++] = i + 2 < totalWav ? B64[x & 63] : '=';
    if (p >= (int)sizeof(trozo) - 4) { cliente.write((uint8_t*)trozo, p); p = 0; }
  }
  if (p) cliente.write((uint8_t*)trozo, p);
  cliente.print(fin);

  int estado = leerEstado();
  if (estado != 200) { mostrarError(estado); return ""; }

  String cuerpo;
  for (;;) { int c = leerCuerpo(); if (c < 0 || cuerpo.length() > 8000) break; cuerpo += (char)c; }
  cliente.stop();

  int k = cuerpo.indexOf("\"text\"");
  if (k < 0) { Serial.println("Gemini no mandó texto:"); Serial.println(cuerpo.substring(0, 500)); return ""; }
  k = cuerpo.indexOf('"', cuerpo.indexOf(':', k)) + 1;
  String texto;
  for (int i = k; i < (int)cuerpo.length(); i++) {
    char c = cuerpo[i];
    if (c == '\\' && i + 1 < (int)cuerpo.length()) { texto += c; texto += cuerpo[++i]; continue; }
    if (c == '"') break;
    texto += c;
  }
  texto.trim();
  return texto;
}

// Convierte el texto en voz con Gemini y lo reproduce mientras llega
bool hablarConGemini(const String& texto) {
  String peticion = "{\"contents\":[{\"parts\":[{\"text\":\"" ESTILO_VOZ ": " + texto + "\"}]}],"
                    "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
                    "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"" VOZ "\"}}}}}";
  if (!abrirPeticion(MODELO_VOZ, peticion.length())) return false;
  cliente.print(peticion);

  int estado = leerEstado();
  if (estado != 200) { mostrarError(estado); return false; }

  // Busca  "data": "   dentro del JSON
  const char* clave = "\"data\":";
  int coincide = 0;
  for (;;) {
    int c = leerCuerpo();
    if (c < 0) { Serial.println("Gemini no mandó audio"); cliente.stop(); return false; }
    coincide = (c == clave[coincide]) ? coincide + 1 : (c == clave[0] ? 1 : 0);
    if (clave[coincide] == 0) break;
  }
  int c;
  do { c = leerCuerpo(); } while (c == ' ');
  if (c != '"') { cliente.stop(); return false; }

  // Decodifica base64 -> audio PCM 24 kHz y lo manda a la bocina
  i2s_set_sample_rates(I2S_NUM_1, FREC_VOZ_GEMINI);
  calaveraHablando = true;
  static int16_t bloque[256];
  int nBloque = 0;
  uint32_t acum = 0; int bits = 0;
  bool hayByteBajo = false; uint8_t byteBajo = 0;
  uint32_t muestras = 0;
  for (;;) {
    c = leerCuerpo();
    if (c < 0 || c == '"') break;
    int v = valorB64(c);
    if (v < 0) continue;              // '=', '\' u otros
    acum = (acum << 6) | v; bits += 6;
    if (bits >= 8) {
      bits -= 8;
      uint8_t by = (acum >> bits) & 0xFF;
      if (!hayByteBajo) { byteBajo = by; hayByteBajo = true; continue; }
      hayByteBajo = false;
      bloque[nBloque++] = (int16_t)(byteBajo | (by << 8));
      if (nBloque == 256) { sonarBloque(bloque, nBloque); nBloque = 0; muestras += 256; }
    }
  }
  if (nBloque) sonarBloque(bloque, nBloque);
  cliente.stop();

  // Espera a que termine de sonar lo que quedó en el colchón
  delay(16 * 512 * 1000 / FREC_VOZ_GEMINI + 50);
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_set_sample_rates(I2S_NUM_1, FRECUENCIA);
  nivelBoca = 0;
  calaveraHablando = false;
  Serial.printf("Voz reproducida: %.1f s\n", muestras / (float)FREC_VOZ_GEMINI);
  return muestras > 0;
}

// Todo el proceso: grabación -> Gemini -> voz
void responderConGemini() {
  calaveraPensando = true;
  unsigned long t0 = millis();
  String texto = preguntarAGemini();
  Serial.printf("Gemini (%lu ms): %s\n", millis() - t0, texto.c_str());

  if (texto.length() == 0) {                 // error de internet o de la clave
    calaveraPensando = false;
    reproducir(audio_risa, audio_risa_len);
    return;
  }
  String t = texto; t.toUpperCase();
  if (t.startsWith("NADA")) {                // no le hablaron a ella
    calaveraPensando = false;
    return;
  }
  bool ok = hablarConGemini(texto);
  calaveraPensando = false;
  if (!ok) reproducir(audio_risa, audio_risa_len);
}

// ================== TAREA DE AUDIO (núcleo 0) ==================
void tareaAudio(void*) {
  static int32_t muestras[256];
  static int16_t bloque[128];
  unsigned long ignorarHasta = millis() + 1500;   // al arrancar, solo mide el ruido
  unsigned long inicioVoz = 0, ultimoSonido = 0;
  bool enFrase = false;

  for (;;) {
    // ---- ¿Hay algo que decir? ----
    bool despuesDeHablar = false;
    if (clipPedido) {
      const int16_t* c = (const int16_t*)clipPedido;
      uint32_t l = clipPedidoLen;
      clipPedido = nullptr;
      reproducir(c, l);
      despuesDeHablar = true;
    } else if (pruebaGemini) {
      pruebaGemini = false;
      if (hayWifi()) {
        calaveraPensando = true;
        if (!hablarConGemini("Hola mortal, soy " PALABRA_CLAVE ". Bienvenido a mi cripta.")) reproducir(audio_risa, audio_risa_len);
        calaveraPensando = false;
      } else Serial.println("Sin WiFi");
      despuesDeHablar = true;
    }
    if (despuesDeHablar) {
      ignorarHasta = millis() + 700;              // que no se escuche a sí misma
      enFrase = false;
      personaHablando = false;
      continue;
    }

    // ---- Escuchar ----
    size_t leidos = 0;
    i2s_read(I2S_NUM_0, muestras, sizeof(muestras), &leidos, portMAX_DELAY);
    int n = leidos / 4;
    if (n == 0) continue;

    long suma = 0;
    for (int i = 0; i < n; i++) suma += (muestras[i] >> 14);
    long media = suma / n;
    long dev = 0;
    for (int i = 0; i < n; i++) dev += labs((muestras[i] >> 14) - media);
    float nivel = (float)dev / n;
    nivelMic = nivel;

    // Audio para grabar: sin DC, con ganancia, a 8 kHz
    int nb = 0;
    for (int i = 0; i + 1 < n; i += 2) {
      long v = ((muestras[i] >> 14) + (muestras[i + 1] >> 14) - 2 * media) * GANANCIA_MIC / 2;
      bloque[nb++] = (int16_t)constrain(v, -32767L, 32767L);
    }

    unsigned long ahora = millis();
    if (ahora < ignorarHasta) {
      nivelRuido = nivelRuido * 0.9f + nivel * 0.1f;
      guardarEnAnillo(bloque, nb);
      continue;
    }

    float umbral = max((float)NIVEL_MINIMO, nivelRuido * SENSIBILIDAD);
    bool fuerte = nivel > umbral;

    if (!fuerte && !enFrase) nivelRuido = nivelRuido * 0.99f + nivel * 0.01f;  // se adapta al ruido ambiente

    if (fuerte) {
      if (!enFrase) { enFrase = true; inicioVoz = ahora; if (grab) empezarGrabacion(); }
      ultimoSonido = ahora;
      personaHablando = true;
    }
    if (enFrase && grab) agregarAGrabacion(bloque, nb);
    else guardarEnAnillo(bloque, nb);

    // Fin de frase: 0.7 s de silencio, o se llenó la grabación
    bool lleno = grab && grabLen >= MAX_GRAB;
    if (enFrase && (ahora - ultimoSonido > 700 || lleno)) {
      enFrase = false;
      personaHablando = false;
      unsigned long dur = ultimoSonido - inicioVoz;
      Serial.printf("Frase detectada: %lu ms\n", dur);

      if (grab && hayWifi()) {
        if (dur >= 400) responderConGemini();          // "Calavera, ..."
      } else {
        // Sin internet: como antes
        if (dur >= 120 && dur <= 1200)      reproducir(audio_hola, audio_hola_len);
        else if (dur > 1200)                reproducir(audio_risa, audio_risa_len);
      }
      ignorarHasta = millis() + 700;
    }
  }
}

// ================== MONITOR SERIE ==================
//  n e f s o d = expresiones   a c b = mirar   p = parpadear  m = auto
//  h = decir hola   r = risa   q = quién anda ahí   v = ver nivel del micrófono
//  g = probar la voz de Gemini   w = estado del WiFi
void leerSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'n': ponerExpresion(NORMAL); break;
    case 'e': ponerExpresion(ENOJADO); break;
    case 'f': ponerExpresion(FELIZ); break;
    case 's': ponerExpresion(SOSPECHOSO); break;
    case 'o': ponerExpresion(SORPRENDIDO); break;
    case 'd': ponerExpresion(DORMIDO); break;
    case 'a': mirarA(-1); break;
    case 'c': mirarA(0); break;
    case 'b': mirarA(1); break;
    case 'p': parpadear(); break;
    case 'm': modoAuto = !modoAuto; break;
    case 'h': decir(audio_hola, audio_hola_len); break;
    case 'r': decir(audio_risa, audio_risa_len); break;
    case 'q': decir(audio_quien, audio_quien_len); break;
    case 'v': mostrarNivel = !mostrarNivel; break;
    case 'g': pruebaGemini = true; break;
    case 'w':
      if (hayWifi()) Serial.printf("WiFi OK, IP %s, señal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      else Serial.println("WiFi desconectado");
      Serial.printf("Memoria libre: %u bytes\n", ESP.getFreeHeap());
      break;
  }
}

// ================== WIFI ==================
void conectarWifi() {
  if (!wifiConfigurado()) {
    Serial.println("WiFi/Gemini sin configurar: modo sin internet (hola / risa).");
    return;
  }
  Serial.printf("Conectando a WiFi '%s'", WIFI_NOMBRE);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // responde más rápido
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  for (int i = 0; i < 30 && !hayWifi(); i++) {
    // ojos dormidos mientras se conecta
    ponerExpresion(DORMIDO); dibujarOjo(ojoIzq); dibujarOjo(ojoDer);
    delay(500); Serial.print(".");
  }
  ponerExpresion(NORMAL);
  if (hayWifi()) Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nNo se pudo conectar al WiFi (seguirá intentando solo).");
  cliente.setInsecure();         // no revisa el certificado (más simple)
  cliente.setTimeout(25);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Calavera parlante ===");

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  ponerLed(0, 0, 0);

  Wire.begin(SDA_IZQ, SCL_IZQ, 400000);
  Wire1.begin(SDA_DER, SCL_DER, 400000);
  if (!pantIzq.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) Serial.println("Falla ojo IZQ (revisa D26/D27)");
  if (!pantDer.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) Serial.println("Falla ojo DER (revisa D21/D22)");
  // pantIzq.setRotation(2);   // descomenta si un ojo queda de cabeza
  // pantDer.setRotation(2);

  if (wifiConfigurado()) {
    grab = (int16_t*)malloc(MAX_GRAB * sizeof(int16_t));
    if (!grab) Serial.println("Sin memoria para grabar: baja SEG_GRABACION");
  }
  conectarWifi();

  configurarI2S();
  randomSeed(esp_random());
  xTaskCreatePinnedToCore(tareaAudio, "audio", 16384, NULL, 2, NULL, 0);

  Serial.printf("Memoria libre: %u bytes\n", ESP.getFreeHeap());
  if (grab && hayWifi()) Serial.println("Listo. Di: 'Calavera, ¿quién eres?'  (escribe 'g' para probar la voz)");
  else Serial.println("Listo. Di 'hola' cerca del micrófono. Escribe 'h' para probar la bocina.");
}

// ================== LOOP (ojos y LED) ==================
unsigned long ultimoNivel = 0;

void loop() {
  unsigned long ahora = millis();
  leerSerial();

  // --- Reacción de los ojos ---
  static bool ocupadoAntes = false;
  if (calaveraHablando) {
    // mientras habla: cambia de expresión con la voz
    ponerExpresion(nivelBoca > 0.55 ? ENOJADO : NORMAL);
  } else if (calaveraPensando) {
    ponerExpresion(SOSPECHOSO);   // "pensando": mira hacia arriba de lado a lado
    objetivoX = sin(ahora / 400.0) * 0.6;
    objetivoY = -1;
  } else if (personaHablando) {
    ponerExpresion(SOSPECHOSO);   // "te está escuchando"
    objetivoX = 0;
  } else if (ocupadoAntes) {
    ponerExpresion(NORMAL);
  }
  ocupadoAntes = calaveraHablando || calaveraPensando || personaHablando;

  // --- LED como boca ---
  if (calaveraHablando) ponerLed((int)(nivelBoca * 255), 0, 0);
  else if (calaveraPensando) {                      // morado latiendo = pensando
    int brillo = 20 + (int)(60 * (0.5 + 0.5 * sin(ahora / 150.0)));
    ponerLed(brillo, 0, brillo);
  }
  else if (personaHablando) ponerLed(0, 0, 40);     // azul tenue = escuchando
  else ponerLed(8, 0, 0);                           // rojo muy tenue en reposo

  // --- Parpadeo ---
  if (expresion != DORMIDO && ahora >= proximoParpadeo) {
    inicioParpadeo = ahora;
    proximoParpadeo = ahora + random(2000, 6000);
    if (random(100) < 20) proximoParpadeo = ahora + 250;
  }
  unsigned long t = ahora - inicioParpadeo;
  apertura = (t < 160) ? fabs((float)t - 80.0) / 80.0 : 1.0;

  // --- Mirada aleatoria ---
  if (modoAuto && !personaHablando && !calaveraPensando && ahora > finControlManual && ahora >= proximaMirada) {
    objetivoX = (random(100) < 40) ? 0 : random(-100, 101) / 100.0;
    objetivoY = random(-100, 101) / 100.0;
    proximaMirada = ahora + random(800, 3500);
  }
  posX += (objetivoX - posX) * 0.25;
  posY += (objetivoY - posY) * 0.25;

  dibujarOjo(ojoIzq);
  dibujarOjo(ojoDer);

  // --- Nivel del micrófono para calibrar ---
  if (mostrarNivel && ahora - ultimoNivel > 200) {
    ultimoNivel = ahora;
    Serial.printf("mic: %.0f   ruido: %.0f   umbral: %.0f\n",
                  nivelMic, nivelRuido, max((float)NIVEL_MINIMO, nivelRuido * SENSIBILIDAD));
  }
  delay(10);
}
