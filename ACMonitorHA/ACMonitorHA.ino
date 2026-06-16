/*
  ACMonitorHA - ESP-12E / ESP8266

  Pins:
    GPIO5  = digital AC state input
             In this firmware GPIO5 is treated as ACTIVE LOW:
               GPIO5 LOW  = AC ON / voltage present
               GPIO5 HIGH = AC OFF / no voltage
    GPIO2  = LED output, active LOW on many ESP boards
    GPIO15 = relay output

  MQTT / Home Assistant:
    - GPIO5 binary sensor
    - Relay switch
    - Toggle/state-change event sensor
    - MQTT device trigger on GPIO5 state change

  Analog sampling, ADC voltage and analog hysteresis state have been removed.
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

// ---------- USER CONFIG ----------
const char* WIFI_SSID = sWIFI_SSID;
const char* WIFI_PASS = sWIFI_PASS;

const char* MQTT_HOST = sMQTT_SERVER;
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = sMQTT_USER;
const char* MQTT_PASS = sMQTT_PASS;

IPAddress local_IP(LOCAL_IP);
IPAddress gateway(GATEWAY_IP);
IPAddress subnet(SUBNET_MASK);

// Device name
const char* DEVICE_ID   = "acmonitorha_02";
const char* DEVICE_NAME = "ACMonitorHA";

// Pins
const uint8_t PIN_DIGITAL_AC = 5;   // GPIO5 / D1
const uint8_t PIN_LED        = 2;   // GPIO2
const uint8_t PIN_RELAY      = 15;  // GPIO15

// Input/output polarity
const bool GPIO5_ACTIVE_LOW = true; // LOW = AC ON
const bool LED_ACTIVE_LOW   = true; // LOW = LED ON on many ESP8266 boards

// Publish intervals
const unsigned long PUBLISH_INTERVAL_MS = 500;

// ---------- MQTT TOPICS ----------
String baseTopic;
String availabilityTopic;
String stateTopic;
String eventTopic;        // JSON sensor/counter log
String relaySetTopic;
String triggerTopic;      // clean HA device trigger payload

WiFiClient espClient;
PubSubClient mqtt(espClient);

bool acState = false;
bool lastAcState = false;
bool relayState = false;

unsigned long lastPublish = 0;
unsigned long toggleCounter = 0;
bool firstStateRead = true;

// ---------- HELPERS ----------
void setLed(bool on) {
  digitalWrite(PIN_LED, LED_ACTIVE_LOW ? !on : on);
}

bool readAcStateFromGpio5() {
  bool raw = digitalRead(PIN_DIGITAL_AC);
  return GPIO5_ACTIVE_LOW ? !raw : raw;
}

void clearOldAnalogDiscovery() {
  // Removes obsolete retained HA discovery entries from earlier firmware versions.
  mqtt.publish(("homeassistant/sensor/" + String(DEVICE_ID) + "/adc_voltage/config").c_str(), "", true);
  mqtt.publish(("homeassistant/binary_sensor/" + String(DEVICE_ID) + "/ac_state/config").c_str(), "", true);
}

void publishDiscovery() {
  String dev =
    "\"device\":{\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
    "\"name\":\"" + String(DEVICE_NAME) + "\","
    "\"manufacturer\":\"Gert Lauritsen\","
    "\"model\":\"ESP-12E AC Monitor\"}";

  clearOldAnalogDiscovery();

  // MQTT Device Trigger for GPIO5/AC toggle
  mqtt.publish(
    ("homeassistant/device_automation/" + String(DEVICE_ID) + "/ac_toggle/config").c_str(),
    ("{"
      "\"automation_type\":\"trigger\"," 
      "\"topic\":\"" + triggerTopic + "\"," 
      "\"type\":\"button_short_press\"," 
      "\"subtype\":\"ac_toggle\"," 
      "\"payload\":\"TOGGLE\"," 
      "\"device\":{" 
        "\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
        "\"name\":\"" + String(DEVICE_NAME) + "\""
      "}"
    "}").c_str(),
    true
  );

  // GPIO5 binary sensor
  mqtt.publish(
    ("homeassistant/binary_sensor/" + String(DEVICE_ID) + "/gpio5/config").c_str(),
    ("{\"name\":\"AC GPIO5 State\"," 
     "\"unique_id\":\"" + String(DEVICE_ID) + "_gpio5\"," 
     "\"state_topic\":\"" + stateTopic + "\"," 
     "\"availability_topic\":\"" + availabilityTopic + "\"," 
     "\"payload_on\":\"ON\"," 
     "\"payload_off\":\"OFF\"," 
     "\"device_class\":\"power\"," 
     "\"value_template\":\"{{ value_json.gpio5 }}\"," 
     + dev + "}").c_str(),
    true
  );

  // Relay switch
  mqtt.publish(
    ("homeassistant/switch/" + String(DEVICE_ID) + "/relay/config").c_str(),
    ("{\"name\":\"AC Relay\"," 
     "\"unique_id\":\"" + String(DEVICE_ID) + "_relay\"," 
     "\"command_topic\":\"" + relaySetTopic + "\"," 
     "\"state_topic\":\"" + stateTopic + "\"," 
     "\"availability_topic\":\"" + availabilityTopic + "\"," 
     "\"payload_on\":\"ON\"," 
     "\"payload_off\":\"OFF\"," 
     "\"value_template\":\"{{ value_json.relay }}\"," 
     + dev + "}").c_str(),
    true
  );

  // Event sensor: updates only when GPIO5/AC state changes
  mqtt.publish(
    ("homeassistant/sensor/" + String(DEVICE_ID) + "/toggle_event/config").c_str(),
    ("{\"name\":\"AC Toggle Event\"," 
     "\"unique_id\":\"" + String(DEVICE_ID) + "_toggle_event\"," 
     "\"state_topic\":\"" + eventTopic + "\"," 
     "\"availability_topic\":\"" + availabilityTopic + "\"," 
     "\"value_template\":\"{{ value_json.counter }}\"," 
     + dev + "}").c_str(),
    true
  );
}

void publishState() {
  acState = readAcStateFromGpio5();
  setLed(acState);

  String payload =
    "{"
    "\"gpio5\":\"" + String(acState ? "ON" : "OFF") + "\","
    "\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\""
    "}";

  mqtt.publish(stateTopic.c_str(), payload.c_str(), true);

  if (firstStateRead) {
    lastAcState = acState;
    firstStateRead = false;
    return;
  }

  if (acState != lastAcState) {
    toggleCounter++;

    String eventPayload =
      "{"
      "\"counter\":" + String(toggleCounter) + ","
      "\"state\":\"" + String(acState ? "ON" : "OFF") + "\""
      "}";

    // Sensor/log event
    mqtt.publish(eventTopic.c_str(), eventPayload.c_str(), false);

    // HA device trigger, tied directly to GPIO5 state change
    mqtt.publish(triggerTopic.c_str(), "TOGGLE", false);

    lastAcState = acState;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == relaySetTopic) {
    relayState = msg == "ON";
    digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
    publishState();
  }
}

void connectWiFi() {
  Serial.println("Connecting to: " + String(WIFI_SSID));
  WiFi.config(local_IP, gateway, subnet);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
}

void connectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect(
          DEVICE_ID,
          MQTT_USER,
          MQTT_PASS,
          availabilityTopic.c_str(),
          0,
          true,
          "offline"
        )) {
      mqtt.publish(availabilityTopic.c_str(), "online", true);
      mqtt.subscribe(relaySetTopic.c_str());
      publishDiscovery();
      publishState();
    } else {
      delay(2000);
    }
  }
}

// ---------- SETUP / LOOP ----------
void setup() {
  pinMode(PIN_DIGITAL_AC, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);

  Serial.begin(115200);
  Serial.println("");
  Serial.println("-------------------------------------------------------------------");

  digitalWrite(PIN_RELAY, LOW);
  setLed(false);

  baseTopic = "acmonitorha/" + String(DEVICE_ID);
  availabilityTopic = baseTopic + "/availability";
  stateTopic = baseTopic + "/state";
  eventTopic = baseTopic + "/event";
  relaySetTopic = baseTopic + "/relay/set";
  triggerTopic = baseTopic + "/trigger/ac_toggle";

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  Serial.println("Setup Completed");
  Serial.println(WiFi.macAddress());
  Serial.println("-------------------------------------------------------------------");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }

  mqtt.loop();

  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = millis();
    publishState();
  }
}
