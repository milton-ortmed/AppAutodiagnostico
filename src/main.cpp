#include <Wire.h>
#include <Adafruit_INA260.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

// Estructura para lectura del INA260 (definida al inicio para el preprocesador de Arduino)
struct InaReading {
  float mA;
  float mV;
};

#define I2C_SDA 42
#define I2C_SCL 2

struct MatrizLEDRangosCorriente {
  const char* comp_id;
  uint8_t IS31_ADDR;
  float min_mA;
  float max_mA;
  float tolerance_mA;

  MatrizLEDRangosCorriente(const char* id, uint8_t addr, float minVal = 340.0f, float maxVal = 400.0f, float tol = 2.0f)
    : comp_id(id), IS31_ADDR(addr), min_mA(minVal), max_mA(maxVal), tolerance_mA(tol) {}
};

MatrizLEDRangosCorriente coleccion_matriz_led[] = {
  MatrizLEDRangosCorriente("matriz_led_1", 0x50),
  MatrizLEDRangosCorriente("matriz_led_2", 0x53),
  MatrizLEDRangosCorriente("matriz_led_3", 0x5C)
};
const uint8_t NUM_MATRICES = sizeof(coleccion_matriz_led) / sizeof(coleccion_matriz_led[0]);

#define INA_ADDR    0x40 // Sensor de corriente
Adafruit_INA260 ina260 = Adafruit_INA260();
InaReading last_ina_reading = {0.0f, 0.0f};
InaReading passiveReading = {0.0f, 0.0f};
float min_V = 4.3f;
bool ina_sensor_ready = false;
bool ina_conversion_pending = false;
uint32_t ina_conversion_start = 0;
const uint32_t INA_CONVERSION_TIMEOUT_MS = 3000;

// Credenciales de la red Wi-Fi local (Modo Station)
const char *ssid = "INFINITUM1FBB_2.4";
const char *password = "h2ebXbfCpn";
// Dirección web: http://sensywall.local/


WebServer server(80);

// Variables independientes de página para no mezclar chips (255 = no inicializado)
uint8_t page_chips[NUM_MATRICES] = { 255, 255, 255 }; 

// Control de cantidad de LEDs encendidos (0 a 30 por matriz)
const int MAX_LEDS = 30;
int leds_matriz[NUM_MATRICES] = { 0, 0, 0 };

// Variables para almacenar resultados del autodiagnóstico
float diag_result[NUM_MATRICES] = { 0.0, 0.0, 0.0 };
bool has_diag_result = false;

// --- CÓDIGO HTML / CSS / JS DE LA PÁGINA WEB ---

// --- FUNCIONES DEL IS31FL3733 MULTI-CHIP ---

int getMatrixIndex(uint8_t addr) {
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    if (coleccion_matriz_led[i].IS31_ADDR == addr) return i;
  }
  return -1;
}

void selectPage(uint8_t addr, uint8_t page) {
  int idx = getMatrixIndex(addr);
  if (idx != -1) {
    if (page_chips[idx] == page) return;
    page_chips[idx] = page;
  }
  
  Wire.beginTransmission(addr);
  Wire.write(0xFE); Wire.write(0xC5); // Desbloquear Command Register
  Wire.endTransmission();
  
  Wire.beginTransmission(addr);
  Wire.write(0xFD); Wire.write(page); // Seleccionar página
  Wire.endTransmission();
}

void writeRegister(uint8_t addr, uint8_t page, uint8_t reg, uint8_t data) {
  selectPage(addr, page);
  Wire.beginTransmission(addr);
  Wire.write(reg); Wire.write(data);
  Wire.endTransmission();
}

void setPixel(uint8_t addr, uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
  if (x > 5 || y > 4) return; 
  uint8_t cs_r = y * 3 + 1; 
  uint8_t cs_g = y * 3 + 0; 
  uint8_t cs_b = y * 3 + 2; 
  uint8_t base_addr = x * 16; 
  writeRegister(addr, 1, base_addr + cs_r, r);
  writeRegister(addr, 1, base_addr + cs_g, g);
  writeRegister(addr, 1, base_addr + cs_b, b);
}

void fillColor(uint8_t addr, uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t y = 0; y < 5; y++) {
    for (uint8_t x = 0; x < 6; x++) {
      setPixel(addr, x, y, r, g, b); 
    }
  }
}

// Inicialización de hardware del IS31FL3733
void initIS31(uint8_t addr) {
  writeRegister(addr, 3, 0x00, 0x01); // Operación normal
  writeRegister(addr, 3, 0x01, 0xFF); // Brillo global al máximo

  selectPage(addr, 0);
  for (uint8_t i = 0; i < 0x18; i++) {
    Wire.beginTransmission(addr);
    Wire.write(i); Wire.write(0xFF); 
    Wire.endTransmission();
  }
  fillColor(addr, 0, 0, 0); // Iniciar apagado
}

// --- MEDICIÓN EN MODO DISPARO (TRIGGERED MODE) ---

/**
 * @brief Ejecuta una medición en Modo Disparo (Triggered Mode).
 * Reinicia el acumulador del INA260 desde cero en este instante, espera a que el hardware
 * complete las 256 muestras exactas y devuelve los datos 100% puros y estabilizados.
 */
InaReading readInaTriggered() {
  InaReading reading;
  
  // 1. Disparar una nueva conversión limpia
  ina260.setMode(INA260_MODE_TRIGGERED);
  
  // 2. Esperar a que la bandera Conversion Ready (CVRF) se active (timeout 3s)
  uint32_t startWait = millis();
  while (!ina260.conversionReady() && (millis() - startWait < INA_CONVERSION_TIMEOUT_MS)) {
    delay(2);
  }
  
  // 3. Leer los registros ya calculados
  reading.mA = ina260.readCurrent();
  reading.mV = ina260.readBusVoltage();
  return reading;
}

void updateInaMeasurement() {
  if (!ina_sensor_ready) return;

  if (!ina_conversion_pending) {
    ina260.setMode(INA260_MODE_TRIGGERED);
    ina_conversion_start = millis();
    ina_conversion_pending = true;
    return;
  }

  if (ina260.conversionReady()) {
    last_ina_reading.mA = ina260.readCurrent();
    last_ina_reading.mV = ina260.readBusVoltage();
    ina_conversion_pending = false;
  } else if (millis() - ina_conversion_start >= INA_CONVERSION_TIMEOUT_MS) {
    ina_conversion_pending = false;
  }
}

// --- CONTROLADORES DEL SERVIDOR WEB ---

void sendMeasurementStartAck() {
  String ack = "{\"status\":\"in_progress\",\"wait_ms\":6000}";
  server.send(200, "application/json", ack);
  delay(150);
  yield();
}

void shutdownWifiForMeasurement() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void reconnectWifiForServer() {
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  MDNS.end();
  if (MDNS.begin("sensywall")) {
    Serial.println("Servidor también disponible en: http://sensywall.local/");
  }

  server.begin();
  Serial.println("✅ Wi-Fi reactivado y servidor web reiniciado.");
}

void sendJsonData() {
  JsonDocument doc;

  doc["mA"] = last_ina_reading.mA;
  doc["mV"] = last_ina_reading.mV;
  doc["m1"] = leds_matriz[0];
  doc["m2"] = leds_matriz[1];
  doc["m3"] = leds_matriz[2];
  doc["has_diag"] = has_diag_result;
  doc["diag_m1"] = diag_result[0];
  doc["diag_m2"] = diag_result[1];
  doc["diag_m3"] = diag_result[2];

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// Genera el informe de diagnóstico, guarda el archivo JSON en LittleFS y responde al cliente
void generarRespuestaDiagnosticoSensyWall() {
  // 1. Instanciamos el documento JSON
  JsonDocument doc;

  // 2. Datos raíz
  doc["device_id"] = "SensyWall";

  // Apagar todos los LEDs de todas las matrices
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0);
  }
  delay(100);
  float voltage_V = readInaTriggered().mV / 1000.0f; // Convertir a voltios
  Serial.print("Voltaje medido: ");
  Serial.print(voltage_V);
  Serial.println(" V");

  doc["status"] = (voltage_V >= min_V) ? "ok" : "Insufficient_voltage";

  doc["passive_mA"] = passiveReading.mA;
  doc["voltage_V"] = voltage_V;
  
  // 3. Arreglo "diagnostics" dentro del documento
  JsonArray diagnostics = doc["components"].to<JsonArray>();

  if (doc["status"] == "ok") {

    // 4. Agregamos los componentes evaluando su consumo de corriente
    for (int i = 0; i < NUM_MATRICES; i++) {
      JsonObject comp = diagnostics.add<JsonObject>();
      comp["comp_id"] = coleccion_matriz_led[i].comp_id;
      comp["type"] = "matriz_LED";

      JsonObject connection = comp["connection"].to<JsonObject>();
      connection["bus"] = "I2C";
      connection["addr"] = String("0x") + String(coleccion_matriz_led[i].IS31_ADDR, HEX);

      JsonObject calibration = comp["calibration"].to<JsonObject>();
      calibration["nominal_mA"] = (coleccion_matriz_led[i].min_mA + coleccion_matriz_led[i].max_mA) / 2.0f;
      calibration["min_mA"] = coleccion_matriz_led[i].min_mA;
      calibration["max_mA"] = coleccion_matriz_led[i].max_mA;

      JsonObject diagnostic = comp["diagnostic"].to<JsonObject>();
      diagnostic["measured_mA"] = diag_result[i];

      // Evaluación frente a consumo pasivo y rangos calibrados
      if (diag_result[i] <= (passiveReading.mA + 5.0f)) {
        diagnostic["err_code"] = 5; // Ausencia del componente o sin respuesta
      } else if (diag_result[i] < coleccion_matriz_led[i].min_mA) {
        diagnostic["err_code"] = 1; // Piezas quemadas en el componente
      } else if (diag_result[i] > coleccion_matriz_led[i].max_mA) {
        diagnostic["err_code"] = 2; // Cortocircuito en el componente
      } else {
        diagnostic["err_code"] = 0; // Sin error
      }
    }

    // 5. Estado general del sistema
    bool all_ok = true;
    for (int i = 0; i < NUM_MATRICES; i++) {
      JsonObject component = diagnostics[i];
      const int err_code = component["diagnostic"]["err_code"].as<int>();
      if (err_code != 0) {
        all_ok = false;
        break;
      }
    }
    doc["status"] = all_ok ? "ok" : "fail";
    
  }

  // 6. Escribir el archivo JSON con los resultados en LittleFS
  File diagFile = LittleFS.open("/diagnostics.json", "w");
  if (diagFile) {
    serializeJsonPretty(doc, diagFile);
    diagFile.close();
    Serial.println("✅ Archivo /diagnostics.json guardado en LittleFS con los resultados.");
  } else {
    Serial.println("❌ Error al abrir /diagnostics.json para escritura en LittleFS.");
  }

  // 7. Enviar la respuesta JSON al cliente web
  String response;
  serializeJson(doc, response); 
  Serial.println("JSON de Diagnóstico Generado:");
  Serial.println(response);
  server.send(200, "application/json", response);
}

void handleCalibrationSensyWall() {
  sendMeasurementStartAck();
  shutdownWifiForMeasurement();

  Serial.println("⚙️ Iniciando proceso de calibración de matrices LED sin Wi-Fi activo...");

  // 1. Apagar todos los LEDs de todas las matrices
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0);
  }
  delay(100);
  passiveReading = readInaTriggered();
  Serial.print("Consumo pasivo medido: ");
  Serial.print(passiveReading.mA);
  Serial.println(" mA");

  // 2. Probar secuencialmente cada matriz a brillo máximo blanco
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 255, 255, 255);
    delay(100); // Breve estabilización eléctrica
    InaReading r = readInaTriggered();
    diag_result[i] = r.mA;

    // Guardar valores calibrados en el arreglo coleccion_matriz_led
    coleccion_matriz_led[i].min_mA = r.mA - coleccion_matriz_led[i].tolerance_mA;
    coleccion_matriz_led[i].max_mA = r.mA + coleccion_matriz_led[i].tolerance_mA;

    Serial.print("Calibrada ");
    Serial.print(coleccion_matriz_led[i].comp_id);
    Serial.print(": nominal=");
    Serial.print(r.mA);
    Serial.print(" mA, rango=[");
    Serial.print(coleccion_matriz_led[i].min_mA);
    Serial.print(" - ");
    Serial.print(coleccion_matriz_led[i].max_mA);
    Serial.println("] mA");

    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0);
    delay(50);
  }

  has_diag_result = true;

  // 3. Crear documento JSON con la calibración
  JsonDocument calDoc;
  calDoc["device_id"] = "SensyWall";
  calDoc["passive_mA"] = passiveReading.mA;
  calDoc["voltage_V"] = passiveReading.mV / 1000.0f; // Convertir a voltios
  JsonArray mats = calDoc["components"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    JsonObject m = mats.add<JsonObject>();
    m["comp_id"] = coleccion_matriz_led[i].comp_id;
    m["type"] = "matriz_LED";

    JsonObject connection = m["connection"].to<JsonObject>();
    connection["bus"] = "I2C";
    connection["addr"] = String("0x") + String(coleccion_matriz_led[i].IS31_ADDR, HEX);

    JsonObject calibration = m["calibration"].to<JsonObject>();
    calibration["nominal_mA"] = diag_result[i];
    calibration["min_mA"] = coleccion_matriz_led[i].min_mA;
    calibration["max_mA"] = coleccion_matriz_led[i].max_mA;
  }

  // 4. Guardar archivo /calibration.json en LittleFS para persistencia
  File calFile = LittleFS.open("/calibration.json", "w");
  if (calFile) {
    serializeJsonPretty(calDoc, calFile);
    calFile.close();
    Serial.println("✅ Archivo /calibration.json guardado en LittleFS.");
  }

  reconnectWifiForServer();
}

/**
 * @brief Rutina de autodiagnóstico:
 * 1. Apaga todas las matrices.
 * 2. Enciende los 30 LEDs de cada matriz secuencialmente y toma lectura de corriente.
 * 3. Apaga las matrices.
 * 4. Evalúa la corriente frente a coleccion_matriz_led, escribe /diagnostics.json y envía respuesta.
 */
void handleAutodiag() {
  sendMeasurementStartAck();
  shutdownWifiForMeasurement();

  Serial.println("🔬 Iniciando autodiagnóstico de matrices LED sin Wi-Fi activo...");

  // 1. Apagar todos los LEDs de todas las matrices
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0);
  }
  delay(100);

  // 2. Probar secuencialmente cada matriz
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 255, 255, 255);
    delay(100); // Breve estabilización eléctrica
    InaReading r = readInaTriggered();
    diag_result[i] = r.mA;

    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0);
    delay(50);
  }

  has_diag_result = true;

  // 3. Evaluar resultados y guardar archivo JSON en LittleFS
  generarRespuestaDiagnosticoSensyWall();
  reconnectWifiForServer();
}

// --- SETUP Y LOOP ---

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // Inicializar sensor INA260 primero
  if (!ina260.begin(INA_ADDR, &Wire)) {
    Serial.println("❌ Error: No se encontró el sensor INA260. Revisa conexiones I2C.");
  } else {
    ina_sensor_ready = true;
    Serial.println("✅ INA260 detectado.");
    ina260.setVoltageConversionTime(INA260_TIME_140_us);   // Voltaje ultrarrápido (140 us)
    ina260.setCurrentConversionTime(INA260_TIME_8_244_ms); // Corriente precisa (8.244 ms)
    ina260.setAveragingCount(INA260_COUNT_256);            // 256 muestras de promedio
    ina260.setMode(INA260_MODE_TRIGGERED);                 // Modo Disparo
  }

  // Conectar a la red Wi-Fi en modo Station
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando a la red Wi-Fi: ");
  Serial.println(ssid);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("✅ Wi-Fi conectado exitosamente.");
  Serial.print("Servidor web disponible en: http://");
  Serial.println(WiFi.localIP());

  // Inicializar mDNS para acceder mediante http://sensywall.local/
  if (MDNS.begin("sensywall")) {
    Serial.println("Servidor también disponible en: http://sensywall.local/");
  }

  
  // Inicializar sistema de archivos LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error al montar el sistema de archivos LittleFS.");
  } else {
    Serial.println("✅ LittleFS montado correctamente.");
  }

  // Restaurar calibración previa si existe en LittleFS
  if (LittleFS.exists("/calibration.json")) {
    File calFile = LittleFS.open("/calibration.json", "r");
    if (calFile) {
      JsonDocument calDoc;
      if (!deserializeJson(calDoc, calFile)) {
        if (calDoc["passive_mA"].is<float>()) {
          passiveReading.mA = calDoc["passive_mA"].as<float>();
        }

        JsonArray mats = calDoc["components"].as<JsonArray>();
        for (JsonObject m : mats) {
          const char* cid = m["comp_id"];
          JsonObject calibration = m["calibration"].as<JsonObject>();

          for (uint8_t i = 0; i < NUM_MATRICES; i++) {
            if (strcmp(coleccion_matriz_led[i].comp_id, cid) == 0) {
              if (calibration["min_mA"].is<float>()) {
                coleccion_matriz_led[i].min_mA = calibration["min_mA"].as<float>();
              }
              if (calibration["max_mA"].is<float>()) {
                coleccion_matriz_led[i].max_mA = calibration["max_mA"].as<float>();
              }
            }
          }
        }
        Serial.println("✅ Calibración previa restaurada desde /calibration.json");
      }
      calFile.close();
    }
  }

  // Configurar rutas de archivos estáticos desde LittleFS
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.serveStatic("/script.js", LittleFS, "/script.js");
  server.serveStatic("/diagnostics.json", LittleFS, "/diagnostics.json");
  server.serveStatic("/calibration.json", LittleFS, "/calibration.json");

  server.on("/data", sendJsonData);
  server.on("/calibrate", handleCalibrationSensyWall);
  server.on("/autodiag", handleAutodiag);
  server.begin();

  // Inicialización de las matrices de LEDs IS31FL3733
  delay(100);
  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    initIS31(coleccion_matriz_led[i].IS31_ADDR);
    fillColor(coleccion_matriz_led[i].IS31_ADDR, 0, 0, 0); // Asegurar inicio con 0 LEDs encendidos
  }
  
  Serial.println("Sistema de autodiagnóstico listo.");
  Serial.println("Versión con reconexión de red WiFi modo station y servidor web activo.");
}

void loop() {
  // Atender peticiones del servidor web continuamente
  server.handleClient();
  updateInaMeasurement();
}