#include <SPI.h>
#include <Adafruit_GFX.h>
#include <TFT_ILI9163C.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <WiFi.h>
#include <WebServer.h>

// ======================================================
// CONFIGURACIÓN DE COLORES TFT
// ======================================================
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define WHITE   0xFFFF
#define YELLOW  0xFFE0

// Pines Pantalla
#define __CS 2
#define __DC 4
#define __RST 5
TFT_ILI9163C tft = TFT_ILI9163C(__CS, __DC, __RST);

// Pines Sensores I2C y LM35
#define INA_SDA 21
#define INA_SCL 22
Adafruit_INA219 ina219;
#define LM35_PIN 34

// Pines Driver de Motor (Puente H)
#define PWMA 25
#define AIN1 26
#define AIN2 14
#define STBY 13

// Pines Botones
#define BTN_MOTOR 32
#define BTN_START 33
#define BTN_STOP  12

// Pines LEDs de Estado
#define LED_AZUL     16
#define LED_VERDE    17
#define LED_AMARILLO 27
#define LED_ROJO     19

#define BOTON_PRESIONADO LOW
const unsigned long rebote = 40;        // Tiempo de estabilidad requerido (ms)
const unsigned long TIME_COOLDOWN = 250; // Bloqueo de seguridad entre clics de menús (ms)

// Configuración Watchdog
#define WDT_TIMEOUT_S 5
#define WDT_TIMEOUT_MS 5000

// ======================================================
// ESTRUCTURA DE CONTROL PARA BOTONES (ANTI-REBOTES INTEGRAL)
// ======================================================
struct Boton {
  int pin;
  bool ultimoEstado;
  unsigned long ultimoTiempoDebounce;
  bool liberado;
};

Boton btnStart = {BTN_START, HIGH, 0, true};
Boton btnMotor = {BTN_MOTOR, HIGH, 0, true};
Boton btnStop  = {BTN_STOP,  HIGH, 0, true};

// ======================================================
// WIFI ACCESS POINT & WEB SERVER
// ======================================================
const char* ssid     = "BancoDC";
const char* password = "12345678";
WebServer server(80);

// ======================================================
// FILTRO PROMEDIO MÓVIL (MOVING AVERAGE)
// ======================================================
const int N = 5;
float bufferVoltaje[N];
float bufferCorriente[N];
float bufferTemperatura[N];
int indice = 0;
int muestrasValidas = 0;

// Variables Globales de Telemetría (Filtradas)
float voltaje      = 0;
float corriente    = 0;
float potencia     = 0;
float temperatura = 0;

// Estados del Sistema
bool motorOn = false;
bool paroActivo = false;
bool pantallaInicio = true;
bool seleccionMotor = false;
bool errorSensor = false;
bool errorTermico = false;

int pantalla = 5; // Inicia en pantalla de resumen
int ultimaPantalla = -1;

int dutyPercent = 70;
int pwmValue = 0;

float energiaEntradaJ = 0;
float cargaReal = 0;
float cargaSuavizada = 0;

unsigned long lastUpdate = 0;
unsigned long lastEnergyMillis = 0;
unsigned long lastAccionTime = 0; // Para el cooldown global de menús

const unsigned long tiempoRefresco = 500;
bool reiniciarBarra = true;
volatile bool eventoStop = false;

int erroresSensor = 0;
const int MAX_ERRORES_SENSOR = 5;

// Estructura de Motores
struct MotorConfig {
  String nombre;
  float iNormal;
  float iLeve;
  float iCritica;
  float tAdvertencia;
  float tCritica;
};

MotorConfig motores[] = {
  {"DC130",   0.200, 0.400, 0.400, 45.0, 55.0},
  {"MOTOR 2", 0.200, 0.400, 0.400, 45.0, 55.0},
  {"MOTOR 3", 0.200, 0.400, 0.400, 45.0, 55.0}
};

const int totalMotores = 3;
int motorSeleccionado = 0;

enum EstadoSistema {
  EST_NORMAL,
  EST_LEVE,
  EST_CRITICO,
  EST_PARO
};

EstadoSistema estadoActual = EST_NORMAL;
EstadoSistema ultimoEstado = EST_NORMAL;

// ======================================================
// FUNCIONES AUXILIARES Y FILTRADO
// ======================================================
void IRAM_ATTR isrStop() {
  eventoStop = true;
}

void iniciarWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#endif
}

float promedio(float arreglo[]) {
    float suma = 0;
    for(int i = 0; i < muestrasValidas; i++) {
        suma += arreglo[i];
    }
    return muestrasValidas > 0 ? (suma / muestrasValidas) : 0;
}

String ajustarTexto(String texto, int largo) {
  while (texto.length() < largo) texto += " ";
  if (texto.length() > largo) texto = texto.substring(0, largo);
  return texto;
}

String centrarTextoCampo(String texto, int largo) {
  if (texto.length() >= largo) return texto.substring(0, largo);
  int espaciosTotales = largo - texto.length();
  int espaciosIzq = espaciosTotales / 2;
  int espaciosDer = espaciosTotales - espaciosIzq;
  String salida = "";
  for (int i = 0; i < espaciosIzq; i++) salida += " ";
  salida += texto;
  for (int i = 0; i < espaciosDer; i++) salida += " ";
  return salida;
}

void printFijo(int x, int y, String texto, int largo, uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(color, BLACK);
  tft.setCursor(x, y);
  tft.print(ajustarTexto(texto, largo));
}

void printFijoGrandeCentrado(int y, String texto, int largo, uint16_t color) {
  String campo = centrarTextoCampo(texto, largo);
  int ancho = largo * 6 * 2;
  int x = (128 - ancho) / 2;
  if (x < 0) x = 0;
  tft.setTextSize(2);
  tft.setTextColor(color, BLACK);
  tft.setCursor(x, y);
  tft.print(campo);
}

void printLinea(int x, int y, String texto, uint16_t color) {
  int anchoDisponible = 122 - x;
  if (anchoDisponible < 0) anchoDisponible = 0;
  tft.fillRect(x, y, anchoDisponible, 10, BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, BLACK);
  tft.setCursor(x, y);
  tft.print(texto);
}

bool leerBotonEstable(Boton &b) {
  bool lectura = digitalRead(b.pin);
  bool presionado = false;

  if (lectura != b.ultimoEstado) {
    b.ultimoTiempoDebounce = millis();
  }

  if ((millis() - b.ultimoTiempoDebounce) > rebote) {
    if (lectura == BOTON_PRESIONADO) {
      if (b.liberado) {
        presionado = true;
        b.liberado = false;
      }
    } else {
      b.liberado = true;
    }
  }
  b.ultimoEstado = lectura;
  return presionado;
}

float leerLM35Simple() {
  int lectura = analogRead(LM35_PIN);
  float voltajeLM35 = lectura * (3.3 / 4095.0);
  return voltajeLM35 * 100.0;
}

void apagarMotorSeguro() {
  ledcWrite(PWMA, 0);
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  digitalWrite(STBY, LOW);
  motorOn = false;
}

// ======================================================
// RUTAS DEL SERVIDOR WEB (JSON Y HTML)
// ======================================================
void enviarDatos() {
    String json = "[";
    json += String(voltaje, 2);
    json += ",";
    json += String(corriente, 3);
    json += ",";
    json += String(potencia, 2);
    json += ",";
    json += String(temperatura, 1);
    json += "]";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET");
    server.sendHeader("Access-Control-Allow-Headers", "*");
    server.send(200, "application/json", json);
}

void paginaPrincipal() {
    String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
        <title>ESP32 BancoDC</title>
        <style>
            body { background: #0a0f1c; color: white; font-family: Arial; text-align: center; margin-top: 80px; }
            h1 { color: #38bdf8; }
        </style>
    </head>
    <body>
        <h1>ESP32 BANCO DC</h1>
        <h2>Servidor funcionando</h2>
        <p>Ruta de datos de telemetría:</p>
        <h3>/datos</h3>
    </body>
    </html>
    )rawliteral";
    server.send(200, "text/html", html);
}

// ======================================================
// CONTROL DE PANTALLA TFT
// ======================================================
void actualizarEstadoMotorHeader(bool forzar = false) {
  static String ultimoEstadoHeader = "";
  String estadoActualHeader = "";
  
  if (paroActivo) estadoActualHeader = "STOP";
  else if (motorOn) estadoActualHeader = "ON";
  else estadoActualHeader = "OFF";

  if (forzar || estadoActualHeader != ultimoEstadoHeader) {
    ultimoEstadoHeader = estadoActualHeader;
    tft.fillRect(88, 7, 32, 11, BLUE);
    tft.setTextSize(1);
    if (paroActivo) {
      tft.setTextColor(WHITE, BLUE);
      tft.setCursor(92, 9);
      tft.print("STOP");
    } 
    else if (motorOn) {
      tft.setTextColor(GREEN, BLUE);
      tft.setCursor(92, 9);
      tft.print("ON");
    } 
    else {
      tft.setTextColor(WHITE, BLUE);
      tft.setCursor(92, 9);
      tft.print("OFF");
    }
  }
}

void actualizarMotor() {
  pwmValue = map(dutyPercent, 0, 100, 0, 255);
  if (motorOn && !paroActivo && !errorSensor && !errorTermico) {
    digitalWrite(STBY, HIGH);
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    ledcWrite(PWMA, pwmValue);
  } else {
    apagarMotorSeguro();
  }
}

bool validarLectura(float v, float i, float p, float t) {
  if (isnan(v) || isnan(i) || isnan(p) || isnan(t)) return false;
  if (v < 0 || v > 30) return false;
  if (i < 0 || i > 5) return false;
  if (p < 0 || p > 100) return false;
  if (t < -10 || t > 120) return false;
  return true;
}

void leerSensores() {

  float nuevoVoltaje      = ina219.getBusVoltage_V() - 0.78;
  float nuevaCorriente    = (ina219.getCurrent_mA() / 1000.0) - 0.08;
  float nuevaTemperatura  = leerLM35Simple();
  
  if (nuevoVoltaje < 0) {
    nuevoVoltaje = 0;
  }
  if (nuevaCorriente < 0) {
    nuevaCorriente = 0;
  }
  if (nuevaCorriente < 0) nuevaCorriente = 0;

  float nuevaPotencia = nuevoVoltaje * nuevaCorriente;

  if (!validarLectura(nuevoVoltaje, nuevaCorriente,
                      nuevaPotencia,
                      nuevaTemperatura)) {
    erroresSensor++;

    if (erroresSensor >= MAX_ERRORES_SENSOR) {
      errorSensor = true;
      paroActivo = true;
      apagarMotorSeguro();
    }
    return;
  }

  erroresSensor = 0;
  errorSensor = false;

  static bool motorAntes = false;
  static unsigned long tiempoCambioMotor = 0;

  if (motorOn != motorAntes) {
    motorAntes = motorOn;
    tiempoCambioMotor = millis();

    voltaje = nuevoVoltaje;
    corriente = nuevaCorriente;
    temperatura = nuevaTemperatura;
    potencia = voltaje * corriente;
    return;
  }

  if (millis() - tiempoCambioMotor < 1200) {
    voltaje = nuevoVoltaje;
    corriente = nuevaCorriente;
    temperatura = nuevaTemperatura;
    potencia = voltaje * corriente;
    return;
  }

  const float alpha = 0.14;

  const float maxSubidaI = 0.020;
  const float maxBajadaI = 0.04;

  const float maxSubidaV = 0.14;
  const float maxBajadaV = 0.3;

  if (muestrasValidas == 0) {
    voltaje = nuevoVoltaje;
    corriente = nuevaCorriente;
    temperatura = nuevaTemperatura;
    muestrasValidas = 1;
  } else {
    float objetivoI = corriente + alpha * (nuevaCorriente - corriente);
    float objetivoV = voltaje + alpha * (nuevoVoltaje - voltaje);

    if (objetivoI > corriente + maxSubidaI) objetivoI = corriente + maxSubidaI;
    if (objetivoI < corriente - maxBajadaI) objetivoI = corriente - maxBajadaI;

    if (objetivoV > voltaje + maxSubidaV) objetivoV = voltaje + maxSubidaV;
    if (objetivoV < voltaje - maxBajadaV) objetivoV = voltaje - maxBajadaV;

    corriente = objetivoI;
    voltaje = objetivoV;
    temperatura = temperatura + alpha * (nuevaTemperatura - temperatura);
  }

  potencia = voltaje * corriente;
}

void actualizarEnergia() {
  unsigned long ahora = millis();
  if (lastEnergyMillis == 0) {
    lastEnergyMillis = ahora;
    return;
  }
  float deltaT = (ahora - lastEnergyMillis) / 1000.0;
  lastEnergyMillis = ahora;
  if (motorOn && !paroActivo && !errorSensor && !errorTermico) {
    energiaEntradaJ += potencia * deltaT;
  }
}

void calcularCarga() {
  if (!motorOn || paroActivo || errorSensor || errorTermico) {
    cargaReal = 0;
    cargaSuavizada = cargaSuavizada + ((0 - cargaSuavizada) * 0.90);
    if (cargaSuavizada < 1) cargaSuavizada = 0;
    return;
  }

  MotorConfig m = motores[motorSeleccionado];
  if (corriente <= m.iNormal) {
    cargaReal = 0;
  } else {
    cargaReal = ((corriente - m.iNormal) / (m.iCritica - m.iNormal)) * 100.0;
  }

  if (cargaReal < 0) cargaReal = 0; 
  if (cargaReal > 100) cargaReal = 100;

  if (cargaReal > cargaSuavizada) {
    cargaSuavizada = cargaSuavizada + ((cargaReal - cargaSuavizada) * 0.35);
  } else {
    cargaSuavizada = cargaSuavizada + ((cargaReal - cargaSuavizada) * 0.90);
  }
}

void evaluarEstado() {
  if (paroActivo) {
    estadoActual = EST_PARO;
    return;
  }

  MotorConfig m = motores[motorSeleccionado];
  if (temperatura >= m.tCritica) {
    errorTermico = true;
    paroActivo = true;
    apagarMotorSeguro();
    estadoActual = EST_CRITICO;
    return;
  }

  if (corriente > m.iCritica) {
    estadoActual = EST_CRITICO;
  } 
  else if (corriente > m.iNormal || temperatura >= m.tAdvertencia) {
    estadoActual = EST_LEVE;
  } 
  else {
    estadoActual = EST_NORMAL;
  }
}

String textoEstado() {
  if (errorSensor) return "ERROR SENSOR";
  if (errorTermico) return "TEMP ALTA";
  if (estadoActual == EST_NORMAL) return "NORMAL";
  if (estadoActual == EST_LEVE) return "LEVE";
  if (estadoActual == EST_CRITICO) return "CRITICO";
  if (estadoActual == EST_PARO) return "PARO";
  return "NORMAL";
}

uint16_t colorEstado() {
  if (estadoActual == EST_NORMAL) return GREEN;
  if (estadoActual == EST_LEVE) return YELLOW;
  if (estadoActual == EST_CRITICO) return RED;
  if (estadoActual == EST_PARO) return RED;
  return WHITE;
}

void actualizarLedsEstado() {
  digitalWrite(LED_AZUL, HIGH);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_ROJO, LOW);

  if (errorSensor || errorTermico) {
    digitalWrite(LED_ROJO, HIGH);
    return;
  }

  if (estadoActual == EST_NORMAL) {
    digitalWrite(LED_VERDE, HIGH);
  } 
  else if (estadoActual == EST_LEVE) {
    digitalWrite(LED_AMARILLO, HIGH);
  } 
  else if (estadoActual == EST_CRITICO || estadoActual == EST_PARO) {
    digitalWrite(LED_ROJO, HIGH);
  }
}

void dibujarInicio() {
  tft.fillScreen(BLACK);
  tft.drawRect(5, 5, 118, 118, CYAN);
  printLinea(40, 28, "PROYECTO", CYAN);
  printLinea(52, 45, "ABET", WHITE);
  printLinea(17, 70, "ING. MECATRONICA", GREEN);
  printLinea(14, 95, "START para seguir", YELLOW);
}

void dibujarBase() {
  tft.fillScreen(BLACK);
  tft.drawRect(5, 5, 118, 118, CYAN);
  tft.fillRect(5, 5, 118, 15, BLUE);
  tft.setTextSize(1);
  tft.setTextColor(WHITE, BLUE);
  tft.setCursor(10, 9);
  tft.print("BANCO DC");
  actualizarEstadoMotorHeader(true);
}

void dibujarSeleccionMotor() {
  tft.fillScreen(BLACK);
  tft.drawRect(5, 5, 118, 118, CYAN);
  printLinea(18, 24, "SELECCION MOTOR", CYAN);
  tft.setTextSize(2);
  tft.setTextColor(WHITE, BLACK);
  printFijoGrandeCentrado(50, motores[motorSeleccionado].nombre, 9, WHITE);
  printLinea(14, 82, "START: cambiar", YELLOW);
  printLinea(14, 96, "MOTOR: aceptar", GREEN);
  printLinea(14, 110, "STOP: paro", RED);
}

// MODIFICACIÓN DE LA PANTALLA DE PARO (MÁS LIMPIA Y SIN SUPERPOSICIONES)
void dibujarPantalla() {
  reiniciarBarra = true;
  if (pantallaInicio) {
    dibujarInicio();
    return;
  }
  if (seleccionMotor) {
    dibujarSeleccionMotor();
    return;
  }

  // Si el sistema está en PARO, generamos una interfaz de alerta dedicada aislada de la telemetría
  if (paroActivo) {
    tft.fillScreen(BLACK);
    tft.drawRect(5, 5, 118, 118, RED); 
    tft.fillRect(5, 5, 118, 15, RED);   
    
    tft.setTextSize(1);
    tft.setTextColor(WHITE, RED);
    tft.setCursor(44, 9);
    tft.print("ALERTA");

    printFijoGrandeCentrado(40, "PARO", 4, RED);

    String motivo = "EMERGENCIA";
    if (errorSensor) motivo = "ERR. SENSOR";
    else if (errorTermico) motivo = "TEMP ALTA";
    
    printLinea(14, 72, centrarTextoCampo(motivo, 16), WHITE);
    printLinea(14, 96, centrarTextoCampo("START: SALIR", 16), YELLOW);
    return;
  }

  dibujarBase();

  if (pantalla == 1) printLinea(38, 30, "VOLTAJE", CYAN);
  if (pantalla == 2) printLinea(30, 30, "CORRIENTE", CYAN);
  if (pantalla == 3) printLinea(36, 30, "POTENCIA", CYAN);
  if (pantalla == 4) printLinea(47, 30, "TEMP", CYAN);

  if (pantalla == 5) {
    printLinea(38, 24, "RESUMEN", CYAN);
    tft.setTextSize(1);
    tft.setTextColor(WHITE, BLACK);
    tft.setCursor(12, 40);  tft.print("V:");
    tft.setCursor(12, 52);  tft.print("I:");
    tft.setCursor(12, 64);  tft.print("P:");
    tft.setCursor(12, 76);  tft.print("T:");
    tft.setCursor(12, 88);  tft.print("E:");
    tft.setCursor(12, 100); tft.print("Carga:");
    tft.drawRect(12, 112, 100, 8, WHITE);
  }
}

void dibujarBarraCarga() {
  static int anchoAnterior = -1;
  static uint16_t colorAnterior = BLACK;
  int x = 13; int y = 113; int anchoMax = 98; int alto = 6;
  int ancho = map((int)cargaSuavizada, 0, 100, 0, anchoMax);

  if (ancho < 0) ancho = 0;
  if (ancho > anchoMax) ancho = anchoMax;

  uint16_t color = GREEN;
  if (estadoActual == EST_LEVE) color = YELLOW;
  if (estadoActual == EST_CRITICO || estadoActual == EST_PARO) color = RED;

  if (reiniciarBarra) {
    anchoAnterior = -1;
    colorAnterior = BLACK;
    reiniciarBarra = false;
  }

  if (anchoAnterior < 0 || color != colorAnterior) {
    tft.fillRect(x, y, anchoMax, alto, BLACK);
    tft.fillRect(x, y, ancho, alto, color);
  } 
  else if (ancho > anchoAnterior) {
    tft.fillRect(x + anchoAnterior, y, ancho - anchoAnterior, alto, color);
  } 
  else if (ancho < anchoAnterior) {
    tft.fillRect(x + ancho, y, anchoAnterior - ancho, alto, BLACK);
  }
  anchoAnterior = ancho;
  colorAnterior = color;
}

// SE AÑADIÓ GATILLO DE SEGURIDAD PARA DETENER EL REDIBUJADO DURANTE EL PARO
void actualizarValoresPantalla() {
  if (pantallaInicio) return;
  if (seleccionMotor) return;
  if (paroActivo) return; // Si estamos en paro, salimos inmediatamente para no sobreescribir la pantalla

  static int pantallaAnteriorValores = -1;
  bool forzar = (pantalla != pantallaAnteriorValores);
  pantallaAnteriorValores = pantalla;

  actualizarEstadoMotorHeader(forzar);

  static String lastTextoG = "";
  static String lastTextoEst = "";

  if (pantalla >= 1 && pantalla <= 4) {
    String texto = "";
    if (pantalla == 1) texto = String(voltaje, 2) + " V";
    if (pantalla == 2) texto = String(corriente, 3) + " A";
    if (pantalla == 3) texto = String(potencia, 2) + " W";
    if (pantalla == 4) texto = String(temperatura, 1) + " C";

    if (forzar || texto != lastTextoG) {
      lastTextoG = texto;
      printFijoGrandeCentrado(60, texto, 9, WHITE);
    }
    
    String tEst = textoEstado();
    if (forzar || tEst != lastTextoEst) {
      lastTextoEst = tEst;
      printFijo(34, 96, tEst, 14, colorEstado());
    }
  }

  static String lastEncabezado = "";
  static String lastV = "";
  static String lastI = "";
  static String lastP = "";
  static String lastT = "";
  static String lastE = "";
  static String lastC = "";

  if (pantalla == 5) {
    String encabezado = motores[motorSeleccionado].nombre + " | " + textoEstado();
    String strV = String(voltaje, 2) + " V";
    String strI = String(corriente, 3) + " A";
    String strP = String(potencia, 2) + " W";
    String strT = String(temperatura, 1) + " C";
    String strE = String(energiaEntradaJ, 1) + " J";
    String strC = String((int)cargaSuavizada) + " %";

    if (forzar || encabezado != lastEncabezado) {
      lastEncabezado = encabezado;
      printFijo(12, 32, encabezado, 18, colorEstado());
    }
    if (forzar || strV != lastV) {
      lastV = strV;
      printFijo(28, 40, strV, 12, WHITE);
    }
    if (forzar || strI != lastI) {
      lastI = strI;
      printFijo(28, 52, strI, 12, WHITE);
    }
    if (forzar || strP != lastP) {
      lastP = strP;
      printFijo(28, 64, strP, 12, WHITE);
    }
    if (forzar || strT != lastT) {
      lastT = strT;
      printFijo(28, 76, strT, 12, WHITE);
    }
    if (forzar || strE != lastE) {
      lastE = strE;
      printFijo(28, 88, strE, 12, WHITE);
    }
    if (forzar || strC != lastC) {
      lastC = strC;
      printFijo(58, 100, strC, 8, WHITE);
    }
    dibujarBarraCarga();
  }
}

// ======================================================
// LÓGICA DE BOTONES Y EVENTOS
// ======================================================
void activarParoEmergencia() {
  paroActivo = true;
  apagarMotorSeguro();
  evaluarEstado();
  calcularCarga();
  actualizarLedsEstado();
  ultimaPantalla = -1;
}

void manejarMotor() {
  if (seleccionMotor && !paroActivo) {
    seleccionMotor = false;
    pantalla = 5;
    ultimaPantalla = -1;
    return;
  }

  if (!pantallaInicio && !seleccionMotor && !paroActivo && !errorSensor && !errorTermico) {
    motorOn = !motorOn;
    if (motorOn) {
      energiaEntradaJ = 0;
      lastEnergyMillis = millis();
      cargaReal = 0;
      cargaSuavizada = 0;
    } else {
      cargaReal = 0;
      cargaSuavizada = 0;
    }
    actualizarMotor();
    evaluarEstado();
    calcularCarga();
    actualizarLedsEstado();
    actualizarValoresPantalla();
  }
}

void manejarBotones() {
  unsigned long ahora = millis();

  // 1. Botón de START
  if (leerBotonEstable(btnStart)) {
    if (ahora - lastAccionTime < TIME_COOLDOWN) return;
    lastAccionTime = ahora;

    if (digitalRead(BTN_STOP) == BOTON_PRESIONADO) return; 

    if (pantallaInicio) {
      pantallaInicio = false;
      seleccionMotor = true;
      ultimaPantalla = -1;
      return;
    }

    if (paroActivo) {
      paroActivo = false;
      errorSensor = false;
      errorTermico = false;
      motorOn = false;
      pantalla = 5;
      actualizarMotor();
      evaluarEstado();
      actualizarLedsEstado();
      ultimaPantalla = -1;
      return;
    }

    if (seleccionMotor) {
      motorSeleccionado++;
      if (motorSeleccionado >= totalMotores) motorSeleccionado = 0;
      
      tft.fillRect(6, 46, 116, 24, BLACK); 
      printFijoGrandeCentrado(50, motores[motorSeleccionado].nombre, 9, WHITE);
      return; 
    }

    if (pantalla == 5) {
      pantalla = 1;
    } else {
      pantalla++;
      if (pantalla > 4) pantalla = 5;
    }
    ultimaPantalla = -1;
    return;
  }

  // 2. Botón de MOTOR
  if (leerBotonEstable(btnMotor)) {
    if (ahora - lastAccionTime < TIME_COOLDOWN) return;
    lastAccionTime = ahora;

    if (digitalRead(BTN_STOP) == BOTON_PRESIONADO || digitalRead(BTN_START) == BOTON_PRESIONADO) return;

    if (seleccionMotor) {
      seleccionMotor = false;
      pantalla = 5;
      ultimaPantalla = -1;
      return;
    }

    manejarMotor();
    return;
  }

  // 3. Botón de STOP
  if (leerBotonEstable(btnStop)) {
    if (digitalRead(BTN_START) == BOTON_PRESIONADO) return;

    if (seleccionMotor) {
      activarParoEmergencia();
      return;
    }

    if (!paroActivo && !pantallaInicio) {
      activarParoEmergencia();
    }
    return;
  }
}

void revisarStopInterrupcion() {
  bool stopDetectado = false;
  noInterrupts();
  stopDetectado = eventoStop;
  eventoStop = false;
  interrupts();

  if (stopDetectado && digitalRead(BTN_STOP) == BOTON_PRESIONADO && !pantallaInicio && !paroActivo) {
    if (seleccionMotor) {
      return; 
    }
    activarParoEmergencia();
  }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(115200);
  
  delay(1000); 
  Serial.println("\nINICIANDO ESP32...");

  Wire.begin(INA_SDA, INA_SCL);
  Wire.setClock(400000); 

  tft.begin();
  tft.setRotation(2);

  pinMode(LED_AZUL, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);

  digitalWrite(LED_AZUL, LOW);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_ROJO, LOW);

  tft.fillScreen(BLACK);
  dibujarInicio();

  analogReadResolution(12);
  analogSetPinAttenuation(LM35_PIN, ADC_11db);

  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(STBY, OUTPUT);

  pinMode(BTN_MOTOR, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);

  ledcAttach(PWMA, 20000, 8);

  attachInterrupt(digitalPinToInterrupt(BTN_STOP), isrStop, FALLING);

  if (!ina219.begin()) {
  errorSensor = true;
  paroActivo = true;
  tft.fillScreen(BLACK);
  printLinea(20, 50, "ERROR INA219", RED);
  digitalWrite(LED_ROJO, HIGH);
  while (1) {}
  }

  ina219.setCalibration_32V_2A();

  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();

  server.on("/", paginaPrincipal);
  server.on("/datos", enviarDatos);
  server.begin();

  apagarMotorSeguro();
  leerSensores();
  calcularCarga();
  evaluarEstado();
  actualizarLedsEstado();

  lastEnergyMillis = millis();
  iniciarWatchdog();
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  esp_task_wdt_reset();

  server.handleClient();

  revisarStopInterrupcion();
  manejarBotones();

  if (pantalla != ultimaPantalla) {
    ultimaPantalla = pantalla;
    dibujarPantalla();
    actualizarValoresPantalla();
  }

  if (millis() - lastUpdate >= tiempoRefresco) {
    lastUpdate = millis();
    leerSensores();       
    actualizarEnergia();
    calcularCarga();
    evaluarEstado();
    actualizarLedsEstado();
    actualizarValoresPantalla(); 
  }
}