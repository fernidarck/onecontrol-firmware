#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Keypad.h>
#include <Preferences.h>

const char* mqtt_server = "187.124.146.232";
const char* supabaseUrl = "https://vxvrrmtiexpmaqnscydv.supabase.co/rest/v1/devices?chip_id=eq.";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InZ4dnJybXRpZXhwbWFxbnNjeWR2Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3Nzc2Njg2NjQsImV4cCI6MjA5MzI0NDY2NH0.Hl3AXIXDkwGQlc5zOeVdVH0g64KIFYDvpP0xeORZ0uE";

#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CONFIG_CHAR_UUID "cba1d466-344c-4be3-ab31-107001afc06a"
#define WIFI_LIST_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define ID_CHAR_UUID     "d27b1658-6927-466d-8e6d-25866163351d"

const byte ROWS = 4; const byte COLS = 3;
char keys[ROWS][COLS] = {{'1','2','3'},{'4','5','6'},{'7','8','9'},{'*','0','#'}};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS]  = {26, 25, 33};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const int relayPin   = 4;
const int ledInterno = 2;
WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;

String savedSSID, savedPASS, chipIdStr;
String wifiListJson    = "[]";
String codigoAcumulado = "";
BLECharacteristic* pWifiListChar = nullptr;
BLEServer*         pServerGlobal = nullptr;
bool bleNeedsAdvertising = false;

unsigned long lastSupabaseCheck = 0;
unsigned long lastMqttAttempt   = 0;

// ── Acciones ──────────────────────────────────────────────
void abrirAccion(String source) {
  Serial.println(">>> ABRIENDO desde: " + source);
  digitalWrite(relayPin, LOW);
  digitalWrite(ledInterno, HIGH);
  delay(1000);
  digitalWrite(relayPin, HIGH);
  digitalWrite(ledInterno, LOW);
}

// ── MQTT ──────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  if (message == "abrir" || message == "open") abrirAccion("MQTT");
}

void reconnectMQTT() {
  if (client.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();

  String topic = "onecontrol/" + chipIdStr + "/comando";
  if (client.connect(chipIdStr.c_str())) {
    client.subscribe(topic.c_str());
    Serial.println("MQTT OK: " + topic);
  } else {
    Serial.println("MQTT fallo rc=" + String(client.state()));
  }
}

// ── Supabase ──────────────────────────────────────────────
void checkSupabase() {
  if (millis() - lastSupabaseCheck < 5000) return;
  lastSupabaseCheck = millis();

  String url = String(supabaseUrl) + chipIdStr;
  NetworkClientSecure sc;
  sc.setInsecure();
  HTTPClient http;
  http.begin(sc, url);
  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", "Bearer " + String(supabaseKey));

  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, http.getString()) && doc.size() > 0 && doc[0]["last_command"] == "open") {
      abrirAccion("App");
      NetworkClientSecure sc2; sc2.setInsecure();
      HTTPClient patch;
      patch.begin(sc2, url);
      patch.addHeader("apikey", supabaseKey);
      patch.addHeader("Authorization", "Bearer " + String(supabaseKey));
      patch.addHeader("Content-Type", "application/json");
      patch.addHeader("Prefer", "return=minimal");
      patch.PATCH("{\"last_command\": null}");
      patch.end();
    }
  } else {
    Serial.println("Supabase HTTP: " + String(code));
  }
  http.end();
}

// ── BLE Callbacks ─────────────────────────────────────────
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { Serial.println("BLE: app conectada."); }
  void onDisconnect(BLEServer*) { bleNeedsAdvertising = true; }
};

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String value = pChar->getValue().c_str();
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, value) == DeserializationError::Ok) {
      if (doc["action"] == "reset") {
        preferences.begin("config", false); preferences.clear(); preferences.end();
        ESP.restart();
      }
      String ssid = doc["s"] | "";
      String pass = doc["p"] | "";
      if (ssid != "") {
        preferences.begin("config", false);
        preferences.putString("ssid", ssid);
        preferences.putString("pass", pass);
        preferences.end();
        ESP.restart();
      }
    } else {
      int sep = value.indexOf('|');
      if (sep != -1) {
        preferences.begin("config", false);
        preferences.putString("ssid", value.substring(0, sep));
        preferences.putString("pass", value.substring(sep + 1));
        preferences.end();
        ESP.restart();
      }
    }
  }
};

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT); pinMode(ledInterno, OUTPUT);
  digitalWrite(relayPin, HIGH);

  uint64_t chipid = ESP.getEfuseMac();
  char id_buffer[17];
  sprintf(id_buffer, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  chipIdStr = String(id_buffer);
  Serial.println("Chip ID: " + chipIdStr);

  preferences.begin("config", true);
  savedSSID = preferences.getString("ssid", "");
  savedPASS = preferences.getString("pass", "");
  preferences.end();

  // Escanear WiFi para la app (antes de BLE para no interferir)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  DynamicJsonDocument scanDoc(1024);
  JsonArray arr = scanDoc.to<JsonArray>();
  for (int i = 0; i < min(n, 8); i++) arr.add(WiFi.SSID(i));
  serializeJson(scanDoc, wifiListJson);

  // BLE
  BLEDevice::init("OneControl-Gate");
  BLEDevice::setMTU(517);
  pServerGlobal = BLEDevice::createServer();
  pServerGlobal->setCallbacks(new MyServerCallbacks());
  BLEService* pService = pServerGlobal->createService(SERVICE_UUID);

  BLECharacteristic* pConfigChar = pService->createCharacteristic(CONFIG_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pConfigChar->setCallbacks(new ConfigCallbacks());

  pWifiListChar = pService->createCharacteristic(WIFI_LIST_UUID, BLECharacteristic::PROPERTY_READ);
  pWifiListChar->setValue(wifiListJson.c_str());

  BLECharacteristic* pIdChar = pService->createCharacteristic(ID_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
  pIdChar->setValue(id_buffer);

  pService->start();
  pServerGlobal->getAdvertising()->start();
  Serial.println("BLE iniciado.");

  // WiFi + MQTT
  if (savedSSID != "") {
    Serial.println("Conectando a WiFi: " + savedSSID);
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    client.setServer(mqtt_server, 1883);
    client.setCallback(mqttCallback);
  } else {
    Serial.println("Sin WiFi. Esperando config BLE.");
  }
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  // Reiniciar BLE advertising de forma segura (no desde callback)
  if (bleNeedsAdvertising) {
    bleNeedsAdvertising = false;
    delay(100);
    pServerGlobal->getAdvertising()->start();
    Serial.println("BLE: anunciando de nuevo.");
  }

  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
    client.loop();
    checkSupabase();
  }

  char key = keypad.getKey();
  if (key) {
    if (key == '#') {
      if (codigoAcumulado.length() > 0) {
        client.publish(("onecontrol/" + chipIdStr + "/peticion").c_str(), codigoAcumulado.c_str());
        Serial.println("Codigo: " + codigoAcumulado);
      }
      codigoAcumulado = "";
    } else if (key != '*') {
      codigoAcumulado += key;
    }
  }
}
