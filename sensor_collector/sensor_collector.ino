/*
 * ============================================================
 *  Nodo Sensor - Cuarto de Servidores (Proyecto DataGuard)
 *  ESP8266 + DHT22 + MQ-2 + LCD I2C + LEDs + Buzzer
 *  Publica telemetria via MQTT a broker Mosquitto (Raspberry Pi)
 * ============================================================
 *
 *  Cableado:
 *    A0 -> MQ-2 (analogico)
 *    D0 -> DHT22 (datos)     [si da NaN, mover a D3 o D4]
 *    D1 -> SCL del LCD I2C
 *    D2 -> SDA del LCD I2C
 *    D5 -> LED verde   (estado normal)
 *    D6 -> LED amarillo (advertencia)
 *    D7 -> LED rojo     (critico)
 *    D8 -> Buzzer
 */

//esp8266
//http://arduino.esp8266.com/stable/package_esp8266com_index.json

#include <ESP8266WiFi.h>
#include <PubSubClient.h>   // PubSubClient Library by Nick O'Leary
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ===================== CONFIGURACION =====================
// --- WiFi ---
const char* WIFI_SSID     = "ARRIS-A716";
const char* WIFI_PASSWORD = "70DFF7A1A716";

// --- MQTT (Raspberry Pi / Mosquitto) ---
const char* MQTT_BROKER   = "10.85.1.76";   // <-- IP de tu Raspberry Pi
const int   MQTT_PORT     = 1883;              // puerto MQTT plano (red local)
const char* MQTT_CLIENT_ID = "nodo-rack-01";
const char* MQTT_TOPIC    = "datacenter/rack/telemetria";
// Si configuraste usuario/clave en Mosquitto, llena estos; si no, dejalos vacios:
const char* MQTT_USER     = "";
const char* MQTT_PASS     = "";

// --- Umbrales de alarma ---
const float TEMP_WARN  = 26.0;   // advertencia (amarillo)
const float TEMP_CRIT  = 30.0;   // critico (rojo + buzzer)
const float HUM_WARN   = 70.0;   // humedad alta (advertencia)
const int   SMOKE_WARN = 300;    // lectura cruda MQ-2 (0-1023): advertencia
const int   SMOKE_CRIT = 450;    // critico (rojo + buzzer)

// --- Tiempos ---
const unsigned long INTERVALO_MS = 15000;  // publicar cada 15 s

// ===================== PINES =====================
#define PIN_MQ2     A0
#define PIN_DHT     D4     // si NaN, cambiar a D3 o D4
#define PIN_LED_V   D5
#define PIN_LED_A   D6
#define PIN_LED_R   D7
#define PIN_BUZZER  D8

#define DHTTYPE DHT22

// ===================== OBJETOS =====================
DHT dht(PIN_DHT, DHTTYPE);
WiFiClient espClient;
PubSubClient mqtt(espClient);
LiquidCrystal_I2C* lcd = nullptr;   // se crea tras detectar la direccion I2C

unsigned long ultimaLectura = 0;
uint8_t lcdAddr = 0x27;             // valor por defecto; el escaner lo corrige

// ===================== ESCANER I2C =====================
// Recorre el bus I2C y devuelve la primera direccion que responde.
uint8_t detectarLCD() {
  byte encontrada = 0;
  Serial.println(F("Escaneando bus I2C..."));
  for (byte dir = 1; dir < 127; dir++) {
    Wire.beginTransmission(dir);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  Dispositivo I2C en 0x"));
      if (dir < 16) Serial.print('0');
      Serial.println(dir, HEX);
      if (encontrada == 0) encontrada = dir;  // primera que aparece
    }
  }
  if (encontrada == 0) {
    Serial.println(F("  No se encontro ningun dispositivo I2C. Revisa cableado SDA/SCL."));
    return 0x27;  // fallback
  }
  Serial.print(F("Usando direccion LCD: 0x"));
  Serial.println(encontrada, HEX);
  return encontrada;
}

// ===================== WIFI =====================
void conectarWiFi() {
  Serial.print(F("Conectando a WiFi "));
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("\nWiFi OK. IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\nNo se pudo conectar a WiFi (se reintenta en loop)."));
  }
}

// ===================== MQTT =====================
void conectarMQTT() {
  int intentos = 0;
  while (!mqtt.connected() && intentos < 3) {
    Serial.print(F("Conectando a MQTT broker "));
    Serial.print(MQTT_BROKER);
    Serial.print(F(" ... "));
    bool ok;
    if (strlen(MQTT_USER) > 0)
      ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    else
      ok = mqtt.connect(MQTT_CLIENT_ID);

    if (ok) {
      Serial.println(F("conectado."));
    } else {
      Serial.print(F("fallo, rc="));
      Serial.print(mqtt.state());
      Serial.println(F(" (reintento en 2s)"));
      delay(2000);
      intentos++;
    }
  }
}

// ===================== LOGICA DE ESTADO =====================
// Determina el nivel de alarma: 0=normal, 1=advertencia, 2=critico
int evaluarEstado(float temp, float hum, int humo) {
  bool critico = (temp >= TEMP_CRIT) || (humo >= SMOKE_CRIT);
  bool warn    = (temp >= TEMP_WARN) || (hum >= HUM_WARN) || (humo >= SMOKE_WARN);
  if (critico) return 2;
  if (warn)    return 1;
  return 0;
}

// Controla LEDs y buzzer segun el estado (alarma LOCAL, independiente de la nube)
void aplicarSalidas(int estado) {
  digitalWrite(PIN_LED_V, estado == 0);
  digitalWrite(PIN_LED_A, estado == 1);
  digitalWrite(PIN_LED_R, estado == 2);
  digitalWrite(PIN_BUZZER, estado == 2 ? HIGH : LOW);  // buzzer solo en critico
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n=== Nodo Sensor Rack - DataGuard ==="));

  // Pines de salida
  pinMode(PIN_LED_V, OUTPUT);
  pinMode(PIN_LED_A, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // Test rapido de LEDs al arrancar (secuencia visual)
  digitalWrite(PIN_LED_V, HIGH); delay(200);
  digitalWrite(PIN_LED_A, HIGH); delay(200);
  digitalWrite(PIN_LED_R, HIGH); delay(200);
  digitalWrite(PIN_LED_V, LOW);
  digitalWrite(PIN_LED_A, LOW);
  digitalWrite(PIN_LED_R, LOW);

  // Sensores
  dht.begin();

  // I2C en D2 (SDA) y D1 (SCL) -> en ESP8266 son GPIO4 y GPIO5
  Wire.begin(D2, D1);
  lcdAddr = detectarLCD();
  lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
  lcd->init();
  lcd->backlight();
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(F("Nodo Rack 01"));
  lcd->setCursor(0, 1);
  lcd->print(F("Iniciando..."));

  // Red
  conectarWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
}

// ===================== LOOP =====================
void loop() {
  // Mantener WiFi y MQTT vivos
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  if (millis() - ultimaLectura >= INTERVALO_MS) {
    ultimaLectura = millis();

    // --- Leer sensores ---
    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();
    int   humo = analogRead(PIN_MQ2);   // 0-1023

    // Validar DHT22
    if (isnan(temp) || isnan(hum)) {
      Serial.println(F("ERROR: lectura DHT22 invalida (NaN). Revisa pin D0 -> prueba D3/D4."));
      lcd->clear();
      lcd->setCursor(0, 0);
      lcd->print(F("Error sensor"));
      lcd->setCursor(0, 1);
      lcd->print(F("DHT22 NaN"));
      return;  // saltar este ciclo
    }

    // --- Evaluar estado y aplicar alarma local ---
    int estado = evaluarEstado(temp, hum, humo);
    aplicarSalidas(estado);

    const char* etiqueta = (estado == 2) ? "CRITICO" : (estado == 1) ? "ADVERT." : "NORMAL";

    // --- Mostrar en LCD ---
    lcd->clear();
    lcd->setCursor(0, 0);
    lcd->print(F("T:"));
    lcd->print(temp, 1);
    lcd->print(F("C H:"));
    lcd->print(hum, 0);
    lcd->print(F("%"));
    lcd->setCursor(0, 1);
    lcd->print(F("Humo:"));
    lcd->print(humo);
    lcd->print(F(" "));
    lcd->print(etiqueta);

    // --- Construir JSON ---
    StaticJsonDocument<192> doc;
    doc["device"] = MQTT_CLIENT_ID;
    doc["temp"]   = round(temp * 10) / 10.0;
    doc["hum"]    = round(hum * 10) / 10.0;
    doc["smoke"]  = humo;
    doc["alarm"]  = (estado == 2);
    doc["state"]  = etiqueta;
    doc["ts"]     = millis();   // marca temporal relativa; la Pi/AWS pondra la real

    char payload[192];
    serializeJson(doc, payload);

    // --- Publicar por MQTT ---
    if (mqtt.connected()) {
      bool pub = mqtt.publish(MQTT_TOPIC, payload);
      Serial.print(pub ? F("[MQTT OK] ") : F("[MQTT FALLO] "));
    } else {
      Serial.print(F("[SIN MQTT] "));
    }
    Serial.println(payload);
  }
}
