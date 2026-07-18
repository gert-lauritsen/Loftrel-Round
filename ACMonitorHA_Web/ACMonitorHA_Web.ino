/*
  ACMonitorHA - ESP-12E / ESP8266

  GPIO5  = AC detection input (active LOW by default)
  GPIO2  = status LED (active LOW)
  GPIO15 = relay output

  Features:
    - MQTT + Home Assistant discovery
    - GPIO5 binary sensor
    - Relay switch
    - Toggle event and HA device trigger on GPIO5 state change
    - Built-in web interface
    - JSON API
    - mDNS: http://acmonitorha.local/
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
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

const char* DEVICE_ID   = "acmonitorha_01";
const char* DEVICE_NAME = "ACMonitorHA";
const char* MDNS_NAME   = "acmonitorha";
const char* FW_VERSION  = "2.0.0";

const uint8_t PIN_DIGITAL_AC = 5;
const uint8_t PIN_LED        = 2;
const uint8_t PIN_RELAY      = 15;

const bool GPIO5_ACTIVE_LOW = true;
const bool LED_ACTIVE_LOW   = true;
const bool RELAY_ACTIVE_HIGH = true;

const unsigned long PUBLISH_INTERVAL_MS = 1000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

// ---------- MQTT TOPICS ----------
String baseTopic;
String availabilityTopic;
String stateTopic;
String eventTopic;
String relaySetTopic;
String triggerTopic;

WiFiClient espClient;
PubSubClient mqtt(espClient);
ESP8266WebServer server(80);

bool acState = false;
bool lastAcState = false;
bool relayState = false;
bool firstStateRead = true;
unsigned long lastPublish = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long toggleCounter = 0;

// ---------- HTML ----------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ACMonitorHA</title>
  <style>
    :root{color-scheme:dark;--bg:#0f172a;--panel:#1e293b;--muted:#94a3b8;--ok:#22c55e;--bad:#ef4444;--accent:#38bdf8;--text:#f8fafc}
    *{box-sizing:border-box}
    body{margin:0;background:linear-gradient(135deg,#020617,#0f172a);font-family:Arial,sans-serif;color:var(--text);min-height:100vh}
    .wrap{max-width:850px;margin:auto;padding:24px}
    h1{margin:0 0 6px;font-size:2rem}.sub{color:var(--muted);margin-bottom:22px}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}
    .card{background:rgba(30,41,59,.94);border:1px solid #334155;border-radius:16px;padding:18px;box-shadow:0 12px 30px rgba(0,0,0,.25)}
    .label{font-size:.82rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted)}
    .value{font-size:1.55rem;font-weight:700;margin-top:8px}
    .dot{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:8px;background:#64748b}
    .on{background:var(--ok)}.off{background:var(--bad)}
    .buttons{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
    button{border:0;border-radius:10px;padding:11px 16px;font-weight:700;cursor:pointer;background:var(--accent);color:#082f49}
    button.secondary{background:#475569;color:white}button.danger{background:#ef4444;color:white}
    footer{margin-top:20px;color:var(--muted);font-size:.85rem;text-align:center}
    code{color:#bae6fd}
  </style>
</head>
<body>
<div class="wrap">
  <h1>ACMonitorHA</h1>
  <div class="sub">ESP-12E AC monitor and relay controller</div>

  <div class="grid">
    <div class="card"><div class="label">AC input / GPIO5</div><div class="value"><span id="acDot" class="dot"></span><span id="ac">Loading...</span></div></div>
    <div class="card"><div class="label">Relay</div><div class="value"><span id="relayDot" class="dot"></span><span id="relay">Loading...</span></div>
      <div class="buttons">
        <button onclick="relayCmd('on')">ON</button>
        <button class="danger" onclick="relayCmd('off')">OFF</button>
        <button class="secondary" onclick="relayCmd('toggle')">TOGGLE</button>
      </div>
    </div>
    <div class="card"><div class="label">MQTT</div><div class="value"><span id="mqttDot" class="dot"></span><span id="mqtt">Loading...</span></div></div>
    <div class="card"><div class="label">GPIO5 toggle count</div><div id="counter" class="value">-</div></div>
    <div class="card"><div class="label">IP address</div><div id="ip" class="value">-</div></div>
    <div class="card"><div class="label">Wi-Fi signal</div><div id="rssi" class="value">-</div></div>
    <div class="card"><div class="label">Uptime</div><div id="uptime" class="value">-</div></div>
    <div class="card"><div class="label">Free heap</div><div id="heap" class="value">-</div></div>
  </div>

  <footer>Firmware <span id="version">-</span> · API: <code>/api/state</code></footer>
</div>
<script>
function setState(id,dot,on,onText='ON',offText='OFF'){
  document.getElementById(id).textContent=on?onText:offText;
  const d=document.getElementById(dot); d.className='dot '+(on?'on':'off');
}
async function update(){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    const s=await r.json();
    setState('ac','acDot',s.ac,'AC PRESENT','NO AC');
    setState('relay','relayDot',s.relay);
    setState('mqtt','mqttDot',s.mqtt,'CONNECTED','DISCONNECTED');
    document.getElementById('counter').textContent=s.toggleCounter;
    document.getElementById('ip').textContent=s.ip;
    document.getElementById('rssi').textContent=s.rssi+' dBm';
    document.getElementById('uptime').textContent=s.uptime;
    document.getElementById('heap').textContent=s.freeHeap+' bytes';
    document.getElementById('version').textContent=s.version;
  }catch(e){
    document.getElementById('mqtt').textContent='WEB ERROR';
  }
}
async function relayCmd(cmd){
  await fetch('/api/relay/'+cmd,{method:'POST'});
  update();
}
update(); setInterval(update,1000);
</script>
</body>
</html>
)HTML";

void setLed(bool on) {
  digitalWrite(PIN_LED, LED_ACTIVE_LOW ? !on : on);
}

void setRelay(bool on) {
  relayState = on;
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? on : !on);
}

bool readAcStateFromGpio5() {
  const bool raw = digitalRead(PIN_DIGITAL_AC);
  return GPIO5_ACTIVE_LOW ? !raw : raw;
}

String uptimeText() {
  unsigned long total = millis() / 1000UL;
  unsigned long days = total / 86400UL;
  total %= 86400UL;
  unsigned long hours = total / 3600UL;
  total %= 3600UL;
  unsigned long minutes = total / 60UL;
  unsigned long seconds = total % 60UL;
  char text[40];
  snprintf(text, sizeof(text), "%lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  return String(text);
}

void clearOldAnalogDiscovery() {
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

  mqtt.publish(
    ("homeassistant/device_automation/" + String(DEVICE_ID) + "/ac_toggle/config").c_str(),
    ("{\"automation_type\":\"trigger\","
     "\"topic\":\"" + triggerTopic + "\","
     "\"type\":\"button_short_press\","
     "\"subtype\":\"ac_toggle\","
     "\"payload\":\"TOGGLE\","
     "\"device\":{\"identifiers\":[\"" + String(DEVICE_ID) + "\"],\"name\":\"" + String(DEVICE_NAME) + "\"}}"
    ).c_str(), true);

  mqtt.publish(
    ("homeassistant/binary_sensor/" + String(DEVICE_ID) + "/gpio5/config").c_str(),
    ("{\"name\":\"AC GPIO5 State\","
     "\"unique_id\":\"" + String(DEVICE_ID) + "_gpio5\","
     "\"state_topic\":\"" + stateTopic + "\","
     "\"availability_topic\":\"" + availabilityTopic + "\","
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
     "\"device_class\":\"power\","
     "\"value_template\":\"{{ value_json.gpio5 }}\"," + dev + "}"
    ).c_str(), true);

  mqtt.publish(
    ("homeassistant/switch/" + String(DEVICE_ID) + "/relay/config").c_str(),
    ("{\"name\":\"AC Relay\","
     "\"unique_id\":\"" + String(DEVICE_ID) + "_relay\","
     "\"command_topic\":\"" + relaySetTopic + "\","
     "\"state_topic\":\"" + stateTopic + "\","
     "\"availability_topic\":\"" + availabilityTopic + "\","
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
     "\"value_template\":\"{{ value_json.relay }}\"," + dev + "}"
    ).c_str(), true);

  mqtt.publish(
    ("homeassistant/sensor/" + String(DEVICE_ID) + "/toggle_event/config").c_str(),
    ("{\"name\":\"AC Toggle Event\","
     "\"unique_id\":\"" + String(DEVICE_ID) + "_toggle_event\","
     "\"state_topic\":\"" + eventTopic + "\","
     "\"availability_topic\":\"" + availabilityTopic + "\","
     "\"value_template\":\"{{ value_json.counter }}\"," + dev + "}"
    ).c_str(), true);
}

void publishState() {
  acState = readAcStateFromGpio5();
  setLed(acState);

  if (mqtt.connected()) {
    String payload = "{\"gpio5\":\"" + String(acState ? "ON" : "OFF") +
                     "\",\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\"}";
    mqtt.publish(stateTopic.c_str(), payload.c_str(), true);
  }

  if (firstStateRead) {
    lastAcState = acState;
    firstStateRead = false;
    return;
  }

  if (acState != lastAcState) {
    toggleCounter++;
    if (mqtt.connected()) {
      String eventPayload = "{\"counter\":" + String(toggleCounter) +
                            ",\"state\":\"" + String(acState ? "ON" : "OFF") + "\"}";
      mqtt.publish(eventTopic.c_str(), eventPayload.c_str(), false);
      mqtt.publish(triggerTopic.c_str(), "TOGGLE", false);
    }
    lastAcState = acState;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (String(topic) == relaySetTopic) {
    if (msg == "ON") setRelay(true);
    else if (msg == "OFF") setRelay(false);
    else if (msg == "TOGGLE") setRelay(!relayState);
    publishState();
  }
}

void startWiFi() {
  Serial.println("Connecting to: " + String(WIFI_SSID));
  WiFi.config(local_IP, gateway, subnet);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  lastWifiAttempt = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL_MS) return;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiAttempt = millis();
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttempt = millis();

  if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                   availabilityTopic.c_str(), 0, true, "offline")) {
    mqtt.publish(availabilityTopic.c_str(), "online", true);
    mqtt.subscribe(relaySetTopic.c_str());
    publishDiscovery();
    publishState();
  }
}

void sendJsonState() {
  String json = "{";
  json += "\"ac\":" + String(acState ? "true" : "false");
  json += ",\"relay\":" + String(relayState ? "true" : "false");
  json += ",\"mqtt\":" + String(mqtt.connected() ? "true" : "false");
  json += ",\"toggleCounter\":" + String(toggleCounter);
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"uptime\":\"" + uptimeText() + "\"";
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"version\":\"" + String(FW_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", HTTP_GET, sendJsonState);

  server.on("/api/relay/on", HTTP_POST, []() {
    setRelay(true); publishState(); server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/relay/off", HTTP_POST, []() {
    setRelay(false); publishState(); server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/relay/toggle", HTTP_POST, []() {
    setRelay(!relayState); publishState(); server.send(200, "application/json", "{\"ok\":true}");
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_DIGITAL_AC, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);

  setRelay(false);
  setLed(false);

  baseTopic = "acmonitorha/" + String(DEVICE_ID);
  availabilityTopic = baseTopic + "/availability";
  stateTopic = baseTopic + "/state";
  eventTopic = baseTopic + "/event";
  relaySetTopic = baseTopic + "/relay/set";
  triggerTopic = baseTopic + "/trigger/ac_toggle";

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  startWiFi();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(100);
  }
   
  setupWebServer();

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
    }
    Serial.print("Web: http://");
    Serial.println(WiFi.localIP());
    Serial.println("mDNS: http://acmonitorha.local/");
  }
}

void loop() {
  maintainWiFi();
  maintainMQTT();

  if (mqtt.connected()) mqtt.loop();
  server.handleClient();
  MDNS.update();

  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = millis();
    publishState();
  }

  delay(1);
}
