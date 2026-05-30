#include <WiFi.h>
#include <WebServer.h>

// ======================================================
// WIFI ACCESS POINT
// ======================================================

const char* ssid     = "BancoDC";
const char* password = "12345678";

// ======================================================
// WEB SERVER
// ======================================================

WebServer server(80);

// ======================================================
// MOVING AVERAGE
// ======================================================

const int N = 5;

// Buffers

float bufferVoltaje[N];
float bufferCorriente[N];
float bufferTemperatura[N];

// Índice circular

int indice = 0;

// Cantidad válida de muestras

int muestrasValidas = 0;

// ======================================================
// VARIABLES FILTRADAS
// ======================================================

float voltaje     = 0;
float corriente   = 0;
float potencia    = 0;
float temperatura = 0;

// ======================================================
// PROMEDIO MOVIL
// ======================================================

float promedio(float arreglo[]) {

    float suma = 0;

    for(int i = 0; i < muestrasValidas; i++) {

        suma += arreglo[i];
    }

    return suma / muestrasValidas;
}

// ======================================================
// GENERAR DATOS
// ======================================================

void generarDatos() {

    // ==================================================
    // DATOS RAW
    // ==================================================

    float voltajeRaw =
        12.0 +
        random(-20, 40) / 100.0;

    float corrienteRaw =
        0.20 +
        random(-2, 5) / 100.0;

    float temperaturaRaw =
        30.0 +
        random(-10, 50) / 10.0;

    // ==================================================
    // GUARDAR NUEVO DATO
    // ==================================================

    bufferVoltaje[indice] =
        voltajeRaw;

    bufferCorriente[indice] =
        corrienteRaw;

    bufferTemperatura[indice] =
        temperaturaRaw;

    // ==================================================
    // AUMENTAR MUESTRAS VALIDAS
    // ==================================================

    if(muestrasValidas < N) {

        muestrasValidas++;
    }

    // ==================================================
    // CALCULAR PROMEDIOS
    // ==================================================

    voltaje =
        promedio(bufferVoltaje);

    corriente =
        promedio(bufferCorriente);

    temperatura =
        promedio(bufferTemperatura);

    // ==================================================
    // POTENCIA
    // ==================================================

    potencia =
        voltaje * corriente;

    // ==================================================
    // AVANZAR INDICE
    // ==================================================

    indice++;

    if(indice >= N) {

        indice = 0;
    }
}

// ======================================================
// ENVIAR JSON
// ======================================================

void enviarDatos() {

    generarDatos();

    String json = "[";

    json += String(voltaje, 2);
    json += ",";

    json += String(corriente, 3);
    json += ",";

    json += String(potencia, 2);
    json += ",";

    json += String(temperatura, 1);

    json += "]";

    // ==================================================
    // CORS
    // ==================================================

    server.sendHeader(
        "Access-Control-Allow-Origin",
        "*"
    );

    server.sendHeader(
        "Access-Control-Allow-Methods",
        "GET"
    );

    server.sendHeader(
        "Access-Control-Allow-Headers",
        "*"
    );

    // ==================================================
    // ENVIAR JSON
    // ==================================================

    server.send(
        200,
        "application/json",
        json
    );

    Serial.println(json);
}

// ======================================================
// PAGINA PRINCIPAL
// ======================================================

void paginaPrincipal() {

    String html = R"rawliteral(

    <!DOCTYPE html>

    <html>

    <head>

        <title>ESP32 BancoDC</title>

        <style>

            body {

                background: #0a0f1c;
                color: white;

                font-family: Arial;

                text-align: center;

                margin-top: 80px;
            }

            h1 {

                color: #38bdf8;
            }

        </style>

    </head>

    <body>

        <h1>ESP32 BANCO DC</h1>

        <h2>Servidor funcionando</h2>

        <p>Ruta de datos:</p>

        <h3>/datos</h3>

    </body>

    </html>

    )rawliteral";

    server.send(
        200,
        "text/html",
        html
    );
}

// ======================================================
// SETUP
// ======================================================

void setup() {

    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("INICIANDO ESP32...");

    // ==================================================
    // ACCESS POINT
    // ==================================================

    WiFi.softAP(
        ssid,
        password
    );

    IPAddress IP =
        WiFi.softAPIP();

    Serial.println();
    Serial.println("WIFI CREADO");

    Serial.print("SSID: ");
    Serial.println(ssid);

    Serial.print("PASSWORD: ");
    Serial.println(password);

    Serial.print("IP: ");
    Serial.println(IP);

    // ==================================================
    // RUTAS
    // ==================================================

    server.on(
        "/",
        paginaPrincipal
    );

    server.on(
        "/datos",
        enviarDatos
    );

    // ==================================================
    // SERVER
    // ==================================================

    server.begin();

    Serial.println();
    Serial.println("SERVIDOR INICIADO");
}

// ======================================================
// LOOP
// ======================================================

void loop() {

    server.handleClient();
}