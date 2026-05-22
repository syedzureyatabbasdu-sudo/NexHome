#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Adafruit_AHTX0.h>

// ─── Credentials ─────────────────────────────────────────────────────────────
const char* WIFI_SSID = "YOUR_INTERNET";
const char* WIFI_PASS = "YOUR_PASSWORD";

const char* MQTT_HOST  = "broker.hivemq.com";
const int   MQTT_PORT  = 1883;
const char* MQTT_USER  = "";
const char* MQTT_PASS  = "";
const char* BASE_TOPIC = "nexhome";

// ─── Timing ──────────────────────────────────────────────────────────────────
const unsigned long SENSOR_INTERVAL = 3000;

// ─── Pins ────────────────────────────────────────────────────────────────────
#define RELAY1_PIN 16
#define RELAY2_PIN 14
#define RELAY3_PIN 12
#define RELAY4_PIN 13
#define MQ6_PIN    3

#define RELAY_ACTIVE_LOW true

// ─── Objects ─────────────────────────────────────────────────────────────────
Adafruit_AHTX0 aht;
WiFiClient     espClient;
PubSubClient   mqtt(espClient);

String clientId;

// ─── Relay state ─────────────────────────────────────────────────────────────
bool      relayState[5] = {false, false, false, false, false};
const int relayPins[5]  = {0, RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};

// ─── Timers ──────────────────────────────────────────────────────────────────
unsigned long lastSensorPublish    = 0;
unsigned long lastReconnectAttempt = 0;

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[Boot] Starting..."));

  // Relay pins
  for (int i = 1; i <= 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    setRelay(i, false);
  }
  pinMode(MQ6_PIN, INPUT);

  // Wi-Fi
  connectWifi();

  // AHT20
  if (!aht.begin()) {
    Serial.println(F("[AHT20] Not found — check wiring!"));
  } else {
    Serial.println(F("[AHT20] Ready."));
  }

  // OTA
  setupOTA();

  // MQTT
  clientId = "nexhome-" + String(ESP.getChipId(), HEX);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

  connectMQTT();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // OTA must be first
  ArduinoOTA.handle();

  // Wi-Fi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    return;
  }

  // MQTT reconnect with back-off
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      connectMQTT();
    }
  } else {
    mqtt.loop();
  }

  // Periodic sensor publish
  unsigned long now = millis();
  if (now - lastSensorPublish >= SENSOR_INTERVAL) {
    lastSensorPublish = now;
    publishSensors();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Wi-Fi
// ─────────────────────────────────────────────────────────────────────────────
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WiFi] Connecting"));

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("\n[WiFi] Connected — IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\n[WiFi] Failed to connect."));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OTA setup
// ─────────────────────────────────────────────────────────────────────────────
void setupOTA() {
  // Hostname that appears in Arduino IDE → Tools → Port
  ArduinoOTA.setHostname("nexhome-node");

  // Optional password to prevent unauthorized flashing:
  // ArduinoOTA.setPassword("admin");

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("[OTA] Start: " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("\n[OTA] Done — rebooting."));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR)   Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR)     Serial.println(F("End Failed"));
  });

  ArduinoOTA.begin();
  Serial.println(F("[OTA] Ready."));
}

// ─────────────────────────────────────────────────────────────────────────────
//  MQTT
// ─────────────────────────────────────────────────────────────────────────────
void connectMQTT() {
  String lwtTopic = String(BASE_TOPIC) + "/status";

  bool connected;
  if (strlen(MQTT_USER) > 0) {
    connected = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                             lwtTopic.c_str(), 1, true, "offline");
  } else {
    connected = mqtt.connect(clientId.c_str(), NULL, NULL,
                             lwtTopic.c_str(), 1, true, "offline");
  }

  if (connected) {
    mqtt.publish(lwtTopic.c_str(), "online", true);

    for (int i = 1; i <= 4; i++) {
      String topic = String(BASE_TOPIC) + "/relay/" + i;
      mqtt.subscribe(topic.c_str());
    }

    for (int i = 1; i <= 4; i++) {
      publishRelayState(i);
    }

    Serial.println(F("[MQTT] Connected."));
  } else {
    Serial.print(F("[MQTT] Failed, rc="));
    Serial.println(mqtt.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t   = String(topic);
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  for (int i = 1; i <= 4; i++) {
    String relayTopic = String(BASE_TOPIC) + "/relay/" + i;
    if (t == relayTopic) {
      bool on = (msg == "1" || msg == "on" || msg == "ON" || msg == "true");
      setRelay(i, on);
      publishRelayState(i);
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Relay helpers
// ─────────────────────────────────────────────────────────────────────────────
void setRelay(int num, bool on) {
  relayState[num] = on;
  bool pinLevel   = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(relayPins[num], pinLevel ? HIGH : LOW);
}
void publishRelayState(int num) {
  String topic   = String(BASE_TOPIC) + "/relay/" + num + "/state";
  String payload = relayState[num] ? "1" : "0";
  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensors
// ─────────────────────────────────────────────────────────────────────────────
void publishSensors() {
  sensors_event_t humidity_event, temp_event;
  aht.getEvent(&humidity_event, &temp_event);

  float temp = temp_event.temperature;
  float hum  = humidity_event.relative_humidity;

  if (!isnan(temp) && !isnan(hum)) {
    String json = "{\"temp\":";
    json += String(temp, 1);
    json += ",\"hum\":";
    json += String(hum, 1);
    json += "}";

    mqtt.publish((String(BASE_TOPIC) + "/sensors/dht").c_str(), json.c_str());
  }

  int    gasState   = digitalRead(MQ6_PIN);
  String gasPayload = (gasState == LOW) ? "1" : "0";
  mqtt.publish((String(BASE_TOPIC) + "/sensors/gas").c_str(), gasPayload.c_str());
}