// =====================================================
//  CALAVERA - Programa de prueba
//  ESP32 clásico (ESP32 Dev Module)
//
//  - Ojos animados en 2 OLED 0.91" (128x32)
//  - Escucha con el micrófono INMP441
//  - Cuando le dices una palabra corta ("hola") responde con voz
//  - Si le hablas largo, se ríe
//  - Si el PIR detecta a alguien: "¿Quién anda ahí?"
//  - El LED RGB brilla en rojo al ritmo de la voz (como boca)
//
//  OJO: sin internet no entiende palabras. Reacciona a que
//  HABLES. Una frase corta (como "hola") = responde hola.
//
//  Librerías: Adafruit SSD1306 + Adafruit GFX
//  Placa: "ESP32 Dev Module"
// =====================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <esp_idf_version.h>
#include "audios.h"

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
// PIR
#define PIN_PIR 34

// ================== AJUSTES ==================
#define VOLUMEN        0.8   // 0.0 a 1.0
#define SENSIBILIDAD   3.0f  // más bajo = más sensible al ruido
#define NIVEL_MINIMO   300   // nivel mínimo para considerar que alguien habla
#define MIC_CANAL      I2S_CHANNEL_FMT_ONLY_LEFT  // si el nivel nunca cambia, prueba ONLY_RIGHT
#define FRECUENCIA     16000

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
volatile bool  personaHablando  = false;
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
  amp.dma_buf_count = 8;
  amp.dma_buf_len = 256;
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

// ================== TAREA DE AUDIO (núcleo 0) ==================
void reproducir(const int16_t* clip, uint32_t len) {
  static int16_t salida[512];
  calaveraHablando = true;
  for (uint32_t i = 0; i < len; i += 256) {
    uint32_t n = min((uint32_t)256, len - i);
    int pico = 0;
    for (uint32_t k = 0; k < n; k++) {
      int s = (int)(clip[i + k] * VOLUMEN);
      salida[2 * k] = salida[2 * k + 1] = s;    // mismo sonido en L y R
      if (abs(s) > pico) pico = abs(s);
    }
    float objetivo = min(1.0f, pico / 12000.0f);
    nivelBoca = nivelBoca * 0.5f + objetivo * 0.5f;
    size_t escritos;
    i2s_write(I2S_NUM_1, salida, n * 4, &escritos, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
  nivelBoca = 0;
  calaveraHablando = false;
}

void tareaAudio(void*) {
  static int32_t muestras[256];
  unsigned long ignorarHasta = millis() + 1500;   // al arrancar, solo mide el ruido
  unsigned long inicioVoz = 0, ultimoSonido = 0;
  bool enFrase = false;

  for (;;) {
    // ---- ¿Hay algo que decir? ----
    if (clipPedido) {
      const int16_t* c = (const int16_t*)clipPedido;
      uint32_t l = clipPedidoLen;
      clipPedido = nullptr;
      reproducir(c, l);
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

    unsigned long ahora = millis();
    if (ahora < ignorarHasta) {
      nivelRuido = nivelRuido * 0.9f + nivel * 0.1f;
      continue;
    }

    float umbral = max((float)NIVEL_MINIMO, nivelRuido * SENSIBILIDAD);
    bool fuerte = nivel > umbral;

    if (!fuerte && !enFrase) nivelRuido = nivelRuido * 0.99f + nivel * 0.01f;  // se adapta al ruido ambiente

    if (fuerte) {
      if (!enFrase) { enFrase = true; inicioVoz = ahora; }
      ultimoSonido = ahora;
      personaHablando = true;
    }

    // Fin de frase: medio segundo de silencio
    if (enFrase && ahora - ultimoSonido > 500) {
      enFrase = false;
      personaHablando = false;
      unsigned long dur = ultimoSonido - inicioVoz;
      Serial.printf("Frase detectada: %lu ms\n", dur);
      if (dur >= 120 && dur <= 1200)      decir(audio_hola, audio_hola_len);   // palabra corta
      else if (dur > 1200)                decir(audio_risa, audio_risa_len);   // habló mucho
    }
  }
}

// ================== MONITOR SERIE ==================
//  n e f s o d = expresiones   a c b = mirar   p = parpadear  m = auto
//  h = decir hola   r = risa   q = quién anda ahí   v = ver nivel del micrófono
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
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Calavera: prueba ===");

  pinMode(PIN_PIR, INPUT);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  ponerLed(0, 0, 0);

  Wire.begin(SDA_IZQ, SCL_IZQ, 400000);
  Wire1.begin(SDA_DER, SCL_DER, 400000);
  if (!pantIzq.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) Serial.println("Falla ojo IZQ (revisa D26/D27)");
  if (!pantDer.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) Serial.println("Falla ojo DER (revisa D21/D22)");
  // pantIzq.setRotation(2);   // descomenta si un ojo queda de cabeza
  // pantDer.setRotation(2);

  configurarI2S();
  randomSeed(esp_random());
  xTaskCreatePinnedToCore(tareaAudio, "audio", 8192, NULL, 2, NULL, 0);

  Serial.println("Listo. Di 'hola' cerca del micrófono. Escribe 'h' para probar la bocina.");
}

// ================== LOOP (ojos, LED, PIR) ==================
unsigned long ultimoPir = 0, ultimoNivel = 0;
bool pirAntes = false;

void loop() {
  unsigned long ahora = millis();
  leerSerial();

  // --- PIR: alguien llegó ---
  bool pir = digitalRead(PIN_PIR);
  if (pir && !pirAntes && ahora - ultimoPir > 15000 && !calaveraHablando) {
    ultimoPir = ahora;
    Serial.println("PIR: alguien llegó");
    ponerExpresion(SORPRENDIDO);
    mirarA(random(2) ? -1 : 1, 0, 1200);
    decir(audio_quien, audio_quien_len);
  }
  pirAntes = pir;

  // --- Reacción de los ojos ---
  static bool hablabaAntes = false;
  if (calaveraHablando) {
    // mientras habla: cambia de expresión con la voz
    ponerExpresion(nivelBoca > 0.55 ? ENOJADO : NORMAL);
  } else if (personaHablando) {
    ponerExpresion(SOSPECHOSO);   // "te está escuchando"
    objetivoX = 0;
  } else if (hablabaAntes) {
    ponerExpresion(NORMAL);
  }
  hablabaAntes = calaveraHablando || personaHablando;

  // --- LED como boca ---
  if (calaveraHablando) ponerLed((int)(nivelBoca * 255), 0, 0);
  else if (personaHablando) ponerLed(0, 0, 40);   // azul tenue = escuchando
  else ponerLed(8, 0, 0);                         // rojo muy tenue en reposo

  // --- Parpadeo ---
  if (expresion != DORMIDO && ahora >= proximoParpadeo) {
    inicioParpadeo = ahora;
    proximoParpadeo = ahora + random(2000, 6000);
    if (random(100) < 20) proximoParpadeo = ahora + 250;
  }
  unsigned long t = ahora - inicioParpadeo;
  apertura = (t < 160) ? fabs((float)t - 80.0) / 80.0 : 1.0;

  // --- Mirada aleatoria ---
  if (modoAuto && !personaHablando && ahora > finControlManual && ahora >= proximaMirada) {
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
