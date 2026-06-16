/*
 * ============================================================
 *  Nodo Sensor - Cuarto de Servidores (Proyecto DataGuard)
 *  ESP8266 + DHT22 + MQ-2 + LCD I2C + LED RGB + Buzzer
 *  Publica telemetria via MQTT a broker Mosquitto (Raspberry Pi)
 * ============================================================
 *
 *  Cableado:
 *    A0 -> MQ-2 (analogico)
 *    D4 -> DHT22 (datos)
 *    D1 -> SCL del LCD I2C
 *    D2 -> SDA del LCD I2C
 *    D5 -> LED RGB pata R (rojo)
 *    D6 -> LED RGB pata G (verde)
 *    D7 -> LED RGB pata B (azul)
 *    GND -> LED RGB pata comun (la larga)   << CATODO COMUN
 *    D8 -> Buzzer
 *
 *  LED RGB CATODO COMUN: cada color se enciende con HIGH.
 *  Colores por estado:
 *    NORMAL      -> verde   (G)
 *    ADVERTENCIA -> amarillo (R + G)
 *    CRITICO     -> rojo    (R) + buzzer
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
const float TEMP_WARN  = 28.0;   // advertencia (amarillo)
const float TEMP_CRIT  = 32.0;   // critico (rojo + buzzer)
const float HUM_WARN   = 80.0;   // humedad alta (advertencia)
const int   SMOKE_WARN = 300;    // lectura cruda MQ-2 (0-1023): advertencia
const int   SMOKE_CRIT = 450;    // critico (rojo + buzzer)

// --- Tiempos ---
const unsigned long INTERVALO_MS = 15000;  // publicar cada 15 s

// ===================== PINES =====================
#define PIN_MQ2     A0
#define PIN_DHT     D4
#define PIN_LED_R   D5      // LED RGB - rojo
#define PIN_LED_G   D6      // LED RGB - verde
#define PIN_LED_B   D7      // LED RGB - azul
#define PIN_BUZZER  D8

// LED RGB de CATODO COMUN: HIGH enciende, LOW apaga.
// (Si fuera anodo comun, se invierte: LOW enciende.)
#define RGB_ON   HIGH
#define RGB_OFF  LOW

#define DHTTYPE DHT22

// ICONOS
byte gota_icon[8] = {B00000, B00100, B00100, B01110, B11111, B11111, B01110, B00000};
byte termo_icon[8] = {B01110, B01010, B01010, B01010, B01110, B01110, B01110, B01110};
byte aire_icon[8] = {B01101, B10010, B00000, B01001, B10110, B00000, B01101, B10010};
byte grado_icon[8] = {B00110, B01001, B01001, B00110, B00000, B00000, B00000, B00000};

// ===================== OBJETOS =====================
DHT dht(PIN_DHT, DHTTYPE);
WiFiClient espClient;
PubSubClient mqtt(espClient);
LiquidCrystal_I2C* lcd = nullptr;   // se crea tras detectar la direccion I2C

unsigned long ultimaLectura = 0;
uint8_t lcdAddr = 0x27;             // valor por defecto; el escaner lo corrige

// ===================== CONTROL DEL LED RGB =====================
// Enciende el LED RGB con la combinacion R/G/B indicada.
void setRGB(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? RGB_ON : RGB_OFF);
  digitalWrite(PIN_LED_G, g ? RGB_ON : RGB_OFF);
  digitalWrite(PIN_LED_B, b ? RGB_ON : RGB_OFF);
}

// ===================== ESCANER I2C =====================
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

// Controla LED RGB y buzzer segun el estado (alarma LOCAL, independiente de la nube)
void aplicarSalidas(int estado) {
  switch (estado) {
    case 0:  // NORMAL -> verde
      setRGB(false, true, false);
      digitalWrite(PIN_BUZZER, LOW);
      break;
    case 1:  // ADVERTENCIA -> amarillo (rojo + verde)
      setRGB(true, true, false);
      digitalWrite(PIN_BUZZER, LOW);
      break;
    case 2:  // CRITICO -> rojo + buzzer
      setRGB(true, false, false);
      digitalWrite(PIN_BUZZER, HIGH);
      break;
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n=== Nodo Sensor Rack - DataGuard ==="));

  // Pines de salida
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  setRGB(false, false, false);   // apagar LED al inicio

  // Test rapido del LED RGB al arrancar (verde -> amarillo -> rojo -> azul)
  setRGB(false, true, false);  delay(400);   // verde
  setRGB(true,  true, false);  delay(400);   // amarillo
  setRGB(true,  false,false);  delay(400);   // rojo
  setRGB(false, false,true);   delay(400);   // azul (solo prueba)
  setRGB(false, false,false);                // apagar

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

  lcd->createChar(1, termo_icon);
  lcd->createChar(2, grado_icon);
  lcd->createChar(3, gota_icon);
  lcd->createChar(4, aire_icon);
  
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
      Serial.println(F("ERROR: lectura DHT22 invalida (NaN)"));
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
    lcd->write(byte(1));
    lcd->print(F(":"));
    lcd->print(temp, 1);
    lcd->write(byte(2));
    lcd->print(F("C "));
    lcd->write(byte(3));
    lcd->print(F(":"));
    lcd->print(hum, 0);
    lcd->print(F("%"));
    lcd->setCursor(0, 1);
    lcd->write(byte(4));
    lcd->print(F(":"));
    lcd->print(humo);
    lcd->print(F("ppm "));
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
