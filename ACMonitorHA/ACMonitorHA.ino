/*
  ACMonitorHA - ESP-12E / ESP8266
  Pins:
    A0     = DC analog level from AC detector
    GPIO5  = digital AC state input
    GPIO2  = LED, active LOW on many ESP boards
    GPIO15 = relay output

  MQTT / Home Assistant:
    - ADC voltage sensor
    - GPIO5 binary sensor
    - Hysteresis AC binary sensor
    - Relay switch
    - Toggle/state-change event topic
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
const char* DEVICE_ID   = "acmonitorha_01";
const char* DEVICE_NAME = "ACMonitorHA";

// ADC scaling
// Bare ESP-12E A0 max is normally 1.0 V.
// NodeMCU/Wemos boards often have divider and may use 3.3 V.
const float ADC_FULL_SCALE_V = 3.3;

// Hysteresis thresholds
float LOW_THRESHOLD_V  = 1.3;
float HIGH_THRESHOLD_V = 1.6;

// Pins
const uint8_t PIN_DIGITAL_AC = 5;   // GPIO5 / D1
const uint8_t PIN_LED        = 2;   // GPIO2
const uint8_t PIN_RELAY      = 15;  // GPIO15

// LED polarity
const bool LED_ACTIVE_LOW = true;

// Publish intervals
const unsigned long PUBLISH_INTERVAL_MS = 500;

// ---------- MQTT TOPICS ----------
String baseTopic;
String availabilityTopic;
String stateTopic;
String eventTopic;        // JSON sensor/counter
String relaySetTopic;
String triggerTopic;      // ren trigger payload

WiFiClient espClient;
PubSubClient mqtt(espClient);

bool acAnalogState = false;
bool lastAcAnalogState = false;
bool gpio5State = false;
bool relayState = false;

unsigned long lastPublish = 0;
unsigned long toggleCounter = 0;



// ---------- HELPERS ----------
void setLed(bool on) {
  digitalWrite(PIN_LED, LED_ACTIVE_LOW ? !on : on);
}

float readAdcVoltage() {
  int raw = analogRead(A0); // 0..1023
  return (raw / 1023.0f) * ADC_FULL_SCALE_V;
}

bool updateHysteresis(float voltage) {
  if (!acAnalogState && voltage >= HIGH_THRESHOLD_V) {
    acAnalogState = true;
  } else if (acAnalogState && voltage <= LOW_THRESHOLD_V) {
    acAnalogState = false;
  }
  return acAnalogState;
}

void publishDiscovery() {
  String dev =
    "\"device\":{\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
    "\"name\":\"" + String(DEVICE_NAME) + "\","
    "\"manufacturer\":\"Gert Lauritsen\","
    "\"model\":\"ESP-12E AC Monitor\"}";

  // ADC voltage sensor
  mqtt.publish(
    ("homeassistant/sensor/" + String(DEVICE_ID) + "/adc_voltage/config").c_str(),
    ("{\"name\":\"AC ADC Voltage\","
     "\"unique_id\":\"" + String(DEVICE_ID) + "_adc_voltage\","
     "\"state_topic\":\"" + stateTopic + "\","
     "\"availability_topic\":\"" + availabilityTopic + "\","
     "\"unit_of_measurement\":\"V\","
     "\"device_class\":\"voltage\","
     "\"value_template\":\"{{ value_json.adc_voltage }}\","
     + dev + "}").c_str(),
    true
  );

  // MQTT Device Trigger for AC toggle
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
     "\"value_template\":\"{{ value_json.gpio5 }}\","
     + dev + "}").c_str(),
    true
  );

  // Analog hysteresis AC state
  mqtt.publish(
    ("homeassistant/binary_sensor/" + String(DEVICE_ID) + "/ac_state/config").c_str(),
    ("{\"name\":\"AC Analog State\","
     "\"unique_id\":\"" + String(DEVICE_ID) + "_ac_state\","
     "\"state_topic\":\"" + stateTopic + "\","
     "\"availability_topic\":\"" + availabilityTopic + "\","
     "\"payload_on\":\"ON\","
     "\"payload_off\":\"OFF\","
     "\"device_class\":\"power\","
     "\"value_template\":\"{{ value_json.ac_state }}\","
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

  // Event sensor: updates only when AC analog state changes
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
  float voltage = readAdcVoltage();
  gpio5State = digitalRead(PIN_DIGITAL_AC);

  updateHysteresis(voltage);

  setLed(acAnalogState);

  String payload =
    "{"
    "\"adc_voltage\":" + String(voltage, 3) + ","
    "\"ac_state\":\"" + String(acAnalogState ? "OFF" : "ON") + "\","
    "\"gpio5\":\"" + String(gpio5State ? "OFF" : "ON") + "\","
    "\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\","
    "\"low_threshold\":" + String(LOW_THRESHOLD_V, 2) + ","
    "\"high_threshold\":" + String(HIGH_THRESHOLD_V, 2) +
    "}";

  mqtt.publish(stateTopic.c_str(), payload.c_str(), true);

  if (acAnalogState != lastAcAnalogState) {
    toggleCounter++;

    String eventPayload =
      "{"
      "\"counter\":" + String(toggleCounter) + ","
      "\"state\":\"" + String(acAnalogState ? "ON" : "OFF") + "\","
      "\"adc_voltage\":" + String(voltage, 3) +
      "}";

    // Til sensor/log
    mqtt.publish(eventTopic.c_str(), eventPayload.c_str(), false);

    // Til HA device trigger
    mqtt.publish(triggerTopic.c_str(), "TOGGLE", false);

    lastAcAnalogState = acAnalogState;
  }
  //Serial.println(payload);
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
  Serial.println("Connecting to: "+String(WIFI_SSID)); 
  WiFi.config(local_IP, gateway, subnet);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
    // Set hostname, e.g., "myesp"
 /* if (!MDNS.begin(String(room))) {
    Serial.println("Error starting mDNS");
    return;
  }*/  
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

  eventTopic   = baseTopic + "/event";
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