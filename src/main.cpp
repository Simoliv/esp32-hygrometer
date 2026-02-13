#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include "secrets.h"

// === User configuration - edit these ===
const char* WIFI_SSID = DEFAULT_WIFI_SSID;
const char* WIFI_PASS = DEFAULT_WIFI_PASS;

// Diese Werte werden initial gesetzt, können aber über das Webinterface geändert werden
String mqttServer = DEFAULT_MQTT_SERVER;
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUser = DEFAULT_MQTT_USER;
String mqttPass = DEFAULT_MQTT_PASS;
unsigned long measureIntervalMs = 10UL * 1000UL;

// Hardware pins (siehe README.md für Verkabelung mit 74HC4051)
const int MUX_S0 = 25; // GPIO 25 → S0/A (Mux Pin 11)
const int MUX_S1 = 26; // GPIO 26 → S1/B (Mux Pin 10)
const int MUX_S2 = 27; // GPIO 27 → S2/C (Mux Pin 9)
// EN (Mux Pin 6) ist fest auf GND verdrahtet - kein GPIO nötig
const int ADC_PIN = 34; // GPIO 34 ← Z/SIG (Mux Pin 3)

const int NUM_CHANNELS = 8;
const int SAMPLES = 8;
const float VCC = 3.3;
const float RS = 100000.0; // series resistor in ohms (adjustable)

WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);
Preferences prefs;

float dryR[NUM_CHANNELS];
float wetR[NUM_CHANNELS];
bool hasDry = false;
bool hasWet = false;

uint8_t muxSelectPins[3] = {MUX_S0, MUX_S1, MUX_S2};

void setMuxChannel(int ch) {
  // Setze die 3 Adressleitungen (S0, S1, S2) um den Kanal 0-7 zu wählen
  for (int b = 0; b < 3; ++b) digitalWrite(muxSelectPins[b], (ch >> b) & 1);
}

void wifiSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());
  } else Serial.println("WiFi not connected");
}

void mqttConnect() {
  if (mqtt.connected()) return;
  
  mqtt.setServer(mqttServer.c_str(), mqttPort);
  Serial.print("Connecting MQTT...");
  if (mqtt.connect("esp32_hygro", mqttUser.c_str(), mqttPass.c_str())) {
    Serial.println("connected");
  } else {
    Serial.print("failed, rc="); Serial.print(mqtt.state());
    Serial.println(" (continuing anyway)");
  }
}

float readChannelResistance(int ch, bool debug = false) {
  // EN ist fest auf GND verdrahtet (Mux immer aktiv)
  setMuxChannel(ch);
  // Warte auf Einschwingvorgang (RC-Zeit mit Rs=100k)
  delay(30);
  long sum = 0;
  for (int i = 0; i < SAMPLES; ++i) {
    sum += analogRead(ADC_PIN);
    delay(2);
  }
  float avg = (float)sum / SAMPLES;
  float adcMax = 4095.0;
  float vout = (avg / adcMax) * VCC;
  
  if (debug) {
    Serial.print("  [DEBUG CH"); Serial.print(ch);
    Serial.print(": ADC="); Serial.print(avg, 1);
    Serial.print(", Vout="); Serial.print(vout, 3); Serial.println("V]");
  }
  
  if (vout <= 0.0001) return 1e9; // Sehr hoher Widerstand (trocken)
  // Spannungsteiler: Rprobe = Rs * (Vin/Vout - 1)
  float rprobe = RS * (VCC / vout - 1.0);
  return rprobe;
}

float indexFromR(float r, int ch) {
  if (hasDry && hasWet && dryR[ch] > 0 && wetR[ch] > 0 && dryR[ch] != wetR[ch]) {
    float lnDry = log(dryR[ch]);
    float lnWet = log(wetR[ch]);
    float lnR = log(r);
    float pct = 100.0 * (lnDry - lnR) / (lnDry - lnWet);
    if (isnan(pct)) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
  }
  return -1; // unknown
}

void handleMetrics() {
  String body;
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    float r = readChannelResistance(ch);
    float idx = indexFromR(r, ch);
    body += "# HELP hygrometer_resistance_ohms Resistance measured at probe\n";
    body += "# TYPE hygrometer_resistance_ohms gauge\n";
    body += "hygrometer_resistance_ohms{channel=\"" + String(ch) + "\"} " + String(r, 2) + "\n";
    if (idx >= 0) body += "hygrometer_index_percent{channel=\"" + String(ch) + "\"} " + String(idx, 2) + "\n";
  }
  server.send(200, "text/plain; version=0.0.4", body);
}

void handleCalibrateDry() {
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    dryR[ch] = readChannelResistance(ch);
  }
  hasDry = true;
  prefs.begin("hygro", false);
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    String key = String("dry_") + ch;
    prefs.putFloat(key.c_str(), dryR[ch]);
  }
  prefs.putBool("hasDry", true);
  prefs.end();
  server.send(200, "text/plain", "Calibrated dry for all channels\n");
}

void handleCalibrateWet() {
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    wetR[ch] = readChannelResistance(ch);
  }
  hasWet = true;
  prefs.begin("hygro", false);
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    String key = String("wet_") + ch;
    prefs.putFloat(key.c_str(), wetR[ch]);
  }
  prefs.putBool("hasWet", true);
  prefs.end();
  server.send(200, "text/plain", "Calibrated wet for all channels\n");
}

void handleRoot() {
  String html = "<html><head><title>Hygrometer Config</title>";
  html += "<style>body { font-family: sans-serif; margin: 20px; } label { display: inline-block; width: 150px; } input { margin-bottom: 5px; }</style></head><body>";
  html += "<h1>Hygrometer Configuration</h1>";
  html += "<form action='/save' method='POST'>";
  html += "<h3>MQTT Settings</h3>";
  html += "<label>Server:</label><input type='text' name='mqtt_server' value='" + mqttServer + "'><br>";
  html += "<label>Port:</label><input type='number' name='mqtt_port' value='" + String(mqttPort) + "'><br>";
  html += "<label>User:</label><input type='text' name='mqtt_user' value='" + mqttUser + "'><br>";
  html += "<label>Pass:</label><input type='password' name='mqtt_pass' value='" + mqttPass + "'><br>";
  html += "<h3>Measurement</h3>";
  html += "<label>Interval (ms):</label><input type='number' name='interval' value='" + String(measureIntervalMs) + "'><br>";
  html += "<br><input type='submit' value='Save Settings'>";
  html += "</form>";
  html += "<hr><form action='/reboot' method='POST'><input type='submit' value='Reboot ESP' onclick='return confirm(\"Are you sure?\");'></form>";
  html += "<h3>Endpoints</h3><ul><li><a href='/metrics'>Prometheus Metrics</a></li>";
  html += "<li><a href='/calibrate/dry'>Calibrate Dry</a></li>";
  html += "<li><a href='/calibrate/wet'>Calibrate Wet</a></li></ul>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("mqtt_server")) mqttServer = server.arg("mqtt_server");
  if (server.hasArg("mqtt_port")) mqttPort = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) mqttUser = server.arg("mqtt_user");
  if (server.hasArg("mqtt_pass")) mqttPass = server.arg("mqtt_pass");
  if (server.hasArg("interval")) measureIntervalMs = server.arg("interval").toInt();

  prefs.begin("hygro", false);
  prefs.putString("mqttServer", mqttServer);
  prefs.putUInt("mqttPort", mqttPort);
  prefs.putString("mqttUser", mqttUser);
  prefs.putString("mqttPass", mqttPass);
  prefs.putULong("measureInterval", measureIntervalMs);
  prefs.end();

  server.send(200, "text/html", "Settings saved. <a href='/'>Back</a>");
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(1000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Konfiguriere Multiplexer-Adressleitungen
  pinMode(MUX_S0, OUTPUT); 
  pinMode(MUX_S1, OUTPUT); 
  pinMode(MUX_S2, OUTPUT);
  // EN ist fest auf GND verdrahtet - kein pinMode nötig
  
  analogReadResolution(12);

  prefs.begin("hygro", true);
  hasDry = prefs.getBool("hasDry", false);
  hasWet = prefs.getBool("hasWet", false);
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    String dryKey = String("dry_") + ch;
    String wetKey = String("wet_") + ch;
    dryR[ch] = prefs.getFloat(dryKey.c_str(), 0.0);
    wetR[ch] = prefs.getFloat(wetKey.c_str(), 0.0);
  }
  
  // MQTT & Config laden
  mqttServer = prefs.getString("mqttServer", mqttServer);
  mqttPort = prefs.getUInt("mqttPort", mqttPort);
  mqttUser = prefs.getString("mqttUser", mqttUser);
  mqttPass = prefs.getString("mqttPass", mqttPass);
  measureIntervalMs = prefs.getULong("measureInterval", measureIntervalMs);
  
  prefs.end();

  Serial.println("\n=== Hygrometer MUX (8ch) starting ===");
  Serial.print("MUX Pins - S0:"); Serial.print(MUX_S0);
  Serial.print(" S1:"); Serial.print(MUX_S1);
  Serial.print(" S2:"); Serial.println(MUX_S2);
  Serial.print("ADC Pin: "); Serial.println(ADC_PIN);
  
  wifiSetup();
  mqttConnect();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/metrics", handleMetrics);
  server.on("/calibrate/dry", handleCalibrateDry);
  server.on("/calibrate/wet", handleCalibrateWet);
  server.begin();

  Serial.println("HTTP endpoints:");
  Serial.println("  /");
  Serial.println("  /metrics");
  Serial.println("  /calibrate/dry");
  Serial.println("  /calibrate/wet");
  Serial.println("Serial: send 'D' to save dry, 'W' to save wet (for current scan)");
}

unsigned long lastMeasurement = 0;

void loop() {
  // Network handling (non-blocking) - läuft immer!
  server.handleClient();
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Scan nur alle measureIntervalMs durchführen (non-blocking)
  if (millis() - lastMeasurement >= measureIntervalMs) {
    lastMeasurement = millis();
    
    Serial.println("\n--- Scanning all channels ---");
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
      float r = readChannelResistance(ch, true); // Debug aktiviert
      float idx = indexFromR(r, ch);
      Serial.print("CH"); Serial.print(ch); Serial.print(": R="); Serial.print(r, 0); Serial.print(" Ohm");
      if (idx >= 0) {
        Serial.print(" | Moisture="); Serial.print(idx, 1); Serial.print("%");
      }
      Serial.println();

      // MQTT publish (only if connected)
      if (mqtt.connected()) {
        String topic = String("hygrometer/channel") + ch + "/state";
        String payload = (idx >= 0) ? String(idx,2) : String(r,1);
        mqtt.publish(topic.c_str(), payload.c_str());
      }
      delay(50);
    }
    Serial.print("\nNext scan in "); Serial.print(measureIntervalMs/1000); Serial.println(" seconds\n");
  }

  // serial commands
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'D' || c == 'd') {
      for (int ch=0; ch<NUM_CHANNELS; ++ch) dryR[ch] = readChannelResistance(ch);
      hasDry = true;
      prefs.begin("hygro", false);
      for (int ch=0; ch<NUM_CHANNELS; ++ch) {
        String key = String("dry_") + ch;
        prefs.putFloat(key.c_str(), dryR[ch]);
      }
      prefs.putBool("hasDry", true);
      prefs.end();
      Serial.println("Saved dry baseline");
    } else if (c == 'W' || c == 'w') {
      for (int ch=0; ch<NUM_CHANNELS; ++ch) wetR[ch] = readChannelResistance(ch);
      hasWet = true;
      prefs.begin("hygro", false);
      for (int ch=0; ch<NUM_CHANNELS; ++ch) {
        String key = String("wet_") + ch;
        prefs.putFloat(key.c_str(), wetR[ch]);
      }
      prefs.putBool("hasWet", true);
      prefs.end();
      Serial.println("Saved wet baseline");
    }
  }
  
  // Kleine Pause um CPU nicht zu 100% auszulasten
  delay(10);
}
