#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

// === User configuration - edit these ===
const char* WIFI_SSID = DEFAULT_WIFI_SSID;
const char* WIFI_PASS = DEFAULT_WIFI_PASS;

// Diese Werte werden initial gesetzt, können aber über das Webinterface geändert werden
String mqttServer = DEFAULT_MQTT_SERVER;
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUser = DEFAULT_MQTT_USER;
String mqttPass = DEFAULT_MQTT_PASS;
bool mqttEnabled = true;
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

// SHT31 & LCD
Adafruit_SHT31 sht31 = Adafruit_SHT31();
LiquidCrystal_I2C lcd(0x27, 16, 2); 

float dryR[NUM_CHANNELS];
float wetR[NUM_CHANNELS];
bool hasDry = false;
bool hasWet = false;
int refChannel = -1; // -1 = no reference channel
float globalWetR = 0; 
float currentRefR = -1.0;

// Luftwerte (SHT31)
float ambientTemp = 0;
float ambientHum = 0;
bool hasSHT = false;
bool hasLCD = false;
bool lcdEnabled = true;
bool autoRefresh = false;

// LCD Scrolling
unsigned long lastLcdScroll = 0;
int lcdScrollBatch = 0; // 0: C0-1, 1: C2-3, 2: C4-5, 3: C6-7

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
  if (!mqttEnabled || mqtt.connected()) return;
  
  mqtt.setServer(mqttServer.c_str(), mqttPort);
  Serial.print("Connecting MQTT...");
  if (mqtt.connect("esp32_hygro", mqttUser.c_str(), mqttPass.c_str())) {
    Serial.println("connected");
  } else {
    Serial.print("failed, rc="); Serial.print(mqtt.state());
    Serial.println(" (continuing anyway)");
  }
}

float readChannelResistance(int ch, float* outAdc = nullptr, float* outVout = nullptr, bool debug = false) {
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
  
  if (outAdc) *outAdc = avg;
  if (outVout) *outVout = vout;
  
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

void getEffectiveLimits(int ch, float &d, float &w) {
  // 1. Determine local or global Dry value
  d = dryR[ch];
  if (refChannel >= 0 && refChannel < NUM_CHANNELS && ch != refChannel && currentRefR > 0) {
    d = currentRefR;
  }
  
  // 2. Determine local or global Wet value
  w = (globalWetR > 0) ? globalWetR : wetR[ch];

  // 3. Fallbacks if still 0
  if (d <= 0) d = 5000000.0; // 5M fallback
  if (w <= 0) w = 20000.0;    // 20k fallback
}

float indexFromR(float r, int ch) {
  // Hard-Limit für extrem hohe Widerstände (offener Kontakt / staubtrocken)
  if (r > 500000000.0) return 0; 
  
  float d, w;
  getEffectiveLimits(ch, d, w);

  // Sicherstellen, dass Dry immer der höhere Ohm-Wert ist für die Logik
  // Falls versehentlich vertauscht, korrigieren wir das hier lokal
  float dryLimit = (d > w) ? d : w;
  float wetLimit = (d > w) ? w : d;

  if (dryLimit != wetLimit) {
    float lnDry = log(dryLimit);
    float lnWet = log(wetLimit);
    float lnR = log(r);
    
    // Formel: (ln(Dry) - ln(Aktuell)) / (ln(Dry) - ln(Wet))
    // Wenn R = Dry -> 0%
    // Wenn R = Wet -> 100%
    float pct = 100.0 * (lnDry - lnR) / (lnDry - lnWet);
    
    if (isnan(pct)) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
  }
  return 0;
}

void handleMetrics() {
  String body;
  
  if (hasSHT) {
    body += "# HELP hygrometer_ambient_temperature_celsius Ambient temperature from SHT31\n";
    body += "# TYPE hygrometer_ambient_temperature_celsius gauge\n";
    body += "hygrometer_ambient_temperature_celsius " + String(ambientTemp, 2) + "\n";
    body += "# HELP hygrometer_ambient_humidity_percent Ambient humidity from SHT31\n";
    body += "# TYPE hygrometer_ambient_humidity_percent gauge\n";
    body += "hygrometer_ambient_humidity_percent " + String(ambientHum, 2) + "\n";
  }

  // Configuration Info
  body += "# HELP hygrometer_config_reference_dry_channel Channel used as dry baseline (-1 if none)\n";
  body += "hygrometer_config_reference_dry_channel " + String(refChannel) + "\n";
  body += "# HELP hygrometer_config_global_wet_ohms Global fixed wet limit\n";
  body += "hygrometer_config_global_wet_ohms " + String(globalWetR) + "\n";

  // Update references R if configured
  if (refChannel >= 0 && refChannel < NUM_CHANNELS) {
    currentRefR = readChannelResistance(refChannel);
  }

  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    float curAdc, curVout;
    float r = readChannelResistance(ch, &curAdc, &curVout);
    float d, w;
    getEffectiveLimits(ch, d, w);
    float idx = indexFromR(r, ch);

    body += "# HELP hygrometer_adc_raw Raw ADC value from mux\n";
    body += "hygrometer_adc_raw{channel=\"" + String(ch) + "\"} " + String(curAdc, 1) + "\n";
    
    body += "# HELP hygrometer_voltage_volts Measured voltage at Z pin\n";
    body += "hygrometer_voltage_volts{channel=\"" + String(ch) + "\"} " + String(curVout, 3) + "\n";

    body += "# HELP hygrometer_resistance_ohms Raw resistance measured at probe\n";
    body += "hygrometer_resistance_ohms{channel=\"" + String(ch) + "\"} " + String(r, 2) + "\n";
    
    body += "# HELP hygrometer_effective_dry_ohms Used dry limit for index calculation\n";
    body += "hygrometer_effective_dry_ohms{channel=\"" + String(ch) + "\"} " + String(d, 2) + "\n";
    
    body += "# HELP hygrometer_effective_wet_ohms Used wet limit for index calculation\n";
    body += "hygrometer_effective_wet_ohms{channel=\"" + String(ch) + "\"} " + String(w, 2) + "\n";
    
    if (idx >= 0) {
      body += "# HELP hygrometer_index_percent Calculated moisture index\n";
      body += "hygrometer_index_percent{channel=\"" + String(ch) + "\"} " + String(idx, 2) + "\n";
    }
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
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  if (autoRefresh) {
    html += "<meta http-equiv='refresh' content='10'>";
  }
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Hygrometer Control</title>";
  html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css' rel='stylesheet'>";
  html += "<link href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css' rel='stylesheet'>";
  html += "<style>";
  html += "body { background-color: #f8f9fa; }";
  html += ".card { margin-bottom: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
  html += ".status-val { font-size: 1.2rem; font-weight: bold; }";
  html += ".ch-label { width: 40px; display: inline-block; }";
  html += ".fa-circle-question { font-size: 0.8rem; color: #6c757d; cursor: help; margin-left: 2px; }";
  html += ".bg-orange { background-color: #fd7e14 !important; color: white !important; }"; // Bootstrap Orange Custom
  html += "</style></head><body>";
  
  html += "<nav class='navbar navbar-dark bg-primary mb-4'><div class='container-fluid'>";
  html += "<span class='navbar-brand'><i class='fa-solid fa-droplet me-2'></i>Hygrometer Dashboard</span>";
  html += "</div></nav>";

  html += "<div class='container'><div class='row'>";
  
  // Left Column: Environment & Status
  html += "<div class='col-md-4'>";
  html += "<div class='card'><div class='card-header bg-white'><i class='fa-solid fa-wind me-2'></i>Ambient Sensors</div><div class='card-body'>";
  if (hasSHT) {
    html += "<p><i class='fa-solid fa-temperature-half me-2 text-danger'></i>Temp: <span class='status-val'>" + String(ambientTemp, 1) + " °C</span></p>";
    html += "<p><i class='fa-solid fa-cloud-showers-heavy me-2 text-primary'></i>Humidity: <span class='status-val'>" + String(ambientHum, 0) + " %</span></p>";
  } else {
    html += "<div class='alert alert-warning small py-1 px-2'>SHT31 not found</div>";
  }
  html += "</div></div>";

  html += "<div class='card'><div class='card-header bg-white'><i class='fa-solid fa-link me-2'></i>Endpoints</div><div class='card-body d-grid gap-2'>";
  html += "<a href='/metrics' class='btn btn-outline-secondary btn-sm text-start'><i class='fa-solid fa-chart-line me-2'></i>Prometheus Metrics</a>";
  html += "<form action='/reboot' method='POST' onsubmit='return confirm(\"Reboot ESP32?\");'>";
  html += "<button type='submit' class='btn btn-danger btn-sm w-100'><i class='fa-solid fa-power-off me-2'></i>Reboot ESP</button></form>";
  html += "</div></div>";
  html += "</div>";

  // Right Column: Configuration
  html += "<div class='col-md-8'>";
  html += "<form action='/save' method='POST'>";
  
  // Channel Table
  html += "<div class='card'><div class='card-header bg-white d-flex justify-content-between align-items-center small'>";
  html += "<span><i class='fa-solid fa-screwdriver-wrench me-2'></i>Probe Measurements</span>";
  html += "<span class='badge bg-info text-dark'>Ref Dry: " + String(refChannel >= 0 ? "CH" + String(refChannel) : "None") + "</span></div>";
  html += "<div class='card-body p-0'><div class='table-responsive'><table class='table table-hover table-sm mb-0'><thead><tr class='table-light'>";
  html += "<th>CH <i class='fa-solid fa-circle-question' title='Multiplexer Channel (0-7)'></i></th>";
  html += "<th>Raw Ω <i class='fa-solid fa-circle-question' title='Human readable resistance (k=kiloohm, M=megaohm)'></i></th>";
  html += "<th>Plain (Metric) <i class='fa-solid fa-circle-question' title='Exact decimal resistance in Ohms for metrics and calibration'></i></th>";
  html += "<th>ADC <i class='fa-solid fa-circle-question' title='Raw Digital value (0-4095) from the ESP32 ADC pin'></i></th>";
  html += "<th>Vout <i class='fa-solid fa-circle-question' title='Converted voltage reading (0-3.3V)'></i></th>";
  html += "<th class='text-end'>Moisture Index <i class='fa-solid fa-circle-question' title='Calculated percentage relative to Dry and Wet references'></i></th></tr></thead><tbody>";
  for (int i = 0; i < NUM_CHANNELS; ++i) {
    float curAdc, curVout;
    float curR = readChannelResistance(i, &curAdc, &curVout);
    float idx = indexFromR(curR, i);
    html += "<tr><td class='fw-bold'>" + String(i) + "</td>";
    html += "<td><small>" + (curR > 999999 ? String(curR/1000000.0, 1) + "M" : (curR > 999 ? String(curR/1000.0, 0) + "k" : String(curR, 0))) + " Ω</small></td>";
    html += "<td><code>" + String(curR, 2) + "</code></td>";
    html += "<td><small class='text-muted'>" + String(curAdc, 1) + "</small></td>";
    html += "<td><small class='text-muted'>" + String(curVout, 3) + "V</small></td>";
    String badgeColor = "bg-success";
    String textColor = "text-dark";
    if (idx > 85) { badgeColor = "bg-danger"; textColor = "text-white"; }
    else if (idx > 60) { badgeColor = "bg-orange"; textColor = "text-white"; }
    else if (idx > 25) { badgeColor = "bg-warning"; textColor = "text-dark"; }
    
    html += "<td class='text-end'><span class='badge " + badgeColor + " " + textColor + "'>" + String(idx, 0) + "%</span></td></tr>";
  }
  html += "</tbody></table></div></div></div>";

  // System Settings
  html += "<div class='card'><div class='card-header bg-white'><i class='fa-solid fa-gears me-2'></i>System Settings</div><div class='card-body small'>";
  html += "<div class='row g-3'>";
  html += "<div class='col-sm-6'><label class='form-label mb-0'>Dry Reference</label><select class='form-select form-select-sm' name='ref_ch'><option value='-1'>None (Manual)</option>";
  for (int i = 0; i < NUM_CHANNELS; ++i) html += "<option value='" + String(i) + "' " + (refChannel == i ? "selected" : "") + ">CH " + String(i) + "</option>";
  html += "</select></div>";
  html += "<div class='col-sm-6'><label class='form-label mb-0'>Global Wet Value (100%)</label><input type='number' step='any' class='form-control form-control-sm' name='global_wet_r' value='" + String(globalWetR, 2) + "' placeholder='e.g. 20000.00'></div>";
  
  // Interval with unit selector
  unsigned long displayInterval = measureIntervalMs / 1000;
  String unit = "s";
  if (measureIntervalMs >= 60000 && measureIntervalMs % 60000 == 0) {
    displayInterval = measureIntervalMs / 60000;
    unit = "m";
  }
  html += "<div class='col-sm-6'><label class='form-label mb-0'>Interval (Unit)</label><div class='input-group input-group-sm'>";
  html += "<input type='number' class='form-control' name='interval_val' value='" + String(displayInterval) + "'>";
  html += "<select class='form-select' style='max-width: 80px;' name='interval_unit'>";
  html += "<option value='s' " + String(unit == "s" ? "selected" : "") + ">sec</option>";
  html += "<option value='m' " + String(unit == "m" ? "selected" : "") + ">min</option>";
  html += "</select></div></div>";

  html += "<div class='col-sm-6 d-flex align-items-end gap-3'>";
  html += "<div class='form-check form-switch'><input class='form-check-input' type='checkbox' name='mqtt_enabled' value='1' " + String(mqttEnabled ? "checked" : "") + "><label class='form-check-label'>MQTT</label></div>";
  html += "<div class='form-check form-switch'><input class='form-check-input' type='checkbox' name='lcd_enabled' value='1' " + String(lcdEnabled ? "checked" : "") + "><label class='form-check-label'>LCD</label></div>";
  html += "<div class='form-check form-switch'><input class='form-check-input' type='checkbox' name='auto_refresh' value='1' " + String(autoRefresh ? "checked" : "") + "><label class='form-check-label'>Auto-Refresh</label></div>";
  html += "</div>";

  html += "<div class='col-sm-12'><hr class='my-2'></div>";
  html += "<div class='col-md-6'><label class='form-label mb-0 small'>MQTT Server</label><input type='text' class='form-control form-control-sm' name='mqtt_server' value='" + mqttServer + "'></div>";
  html += "<div class='col-md-2'><label class='form-label mb-0 small'>Port</label><input type='number' class='form-control form-control-sm' name='mqtt_port' value='" + String(mqttPort) + "'></div>";
  html += "<div class='col-md-2'><label class='form-label mb-0 small'>User</label><input type='text' class='form-control form-control-sm' name='mqtt_user' value='" + mqttUser + "'></div>";
  html += "<div class='col-md-2'><label class='form-label mb-0 small'>Pass</label><input type='password' class='form-control form-control-sm' name='mqtt_pass' value='" + mqttPass + "'></div>";
  
  html += "<div class='col-12 mt-4'><button type='submit' class='btn btn-primary w-100 btn-sm'><i class='fa-solid fa-floppy-disk me-2'></i>Save Configuration</button></div>";
  html += "</div></div></div>";
  
  html += "</div></form></div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  mqttEnabled = server.hasArg("mqtt_enabled");
  lcdEnabled = server.hasArg("lcd_enabled");
  autoRefresh = server.hasArg("auto_refresh");
  
  if (server.hasArg("mqtt_server")) mqttServer = server.arg("mqtt_server");
  if (server.hasArg("mqtt_port")) mqttPort = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) mqttUser = server.arg("mqtt_user");
  if (server.hasArg("mqtt_pass")) mqttPass = server.arg("mqtt_pass");
  
  if (server.hasArg("interval_val")) {
    long val = server.arg("interval_val").toInt();
    String unit = server.arg("interval_unit");
    if (unit == "m") measureIntervalMs = val * 60000;
    else measureIntervalMs = val * 1000;
  }

  if (server.hasArg("ref_ch")) refChannel = server.arg("ref_ch").toInt();
  if (server.hasArg("global_wet_r")) globalWetR = server.arg("global_wet_r").toFloat();

  prefs.begin("hygro", false);
  
  prefs.putInt("refChannel", refChannel);
  prefs.putFloat("globalWetR", globalWetR);

  prefs.putBool("mqttEnabled", mqttEnabled);
  prefs.putBool("lcdEnabled", lcdEnabled);
  prefs.putBool("autoRefresh", autoRefresh);
  prefs.putString("mqttServer", mqttServer);
  prefs.putUInt("mqttPort", mqttPort);
  prefs.putString("mqttUser", mqttUser);
  prefs.putString("mqttPass", mqttPass);
  prefs.putULong("measureInterval", measureIntervalMs);
  prefs.end();

  if (!mqttEnabled && mqtt.connected()) {
    mqtt.disconnect();
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(1000);
  ESP.restart();
}

// Helper to update LCD display
void updateLcd() {
  if (!hasLCD) return;

  if (!lcdEnabled) {
    lcd.noBacklight();
    lcd.clear();
    return;
  }
  
  lcd.backlight();

  // Update references if needed
  if (refChannel >= 0 && refChannel < NUM_CHANNELS) {
    currentRefR = readChannelResistance(refChannel);
  }

  // Zeile 1: Luftwerte
  lcd.setCursor(0, 0);
  if (hasSHT) {
    lcd.print("L:"); lcd.print(ambientTemp, 1); lcd.print("C ");
    lcd.print(ambientHum, 0); lcd.print("%RH   "); // Leerzeichen zum Löschen alter Reste
  } else {
    lcd.print("Scanning...     ");
  }

  // Zeile 2: Bodenkanäle scrollen (2 pro Seite)
  lcd.setCursor(0, 1);
  int startCh = lcdScrollBatch * 2;
  for (int i = 0; i < 2; ++i) {
    int ch = startCh + i;
    float r = readChannelResistance(ch);
    float idx = indexFromR(r, ch);
    
    lcd.print("C"); lcd.print(ch); lcd.print(":");
    
    // Kennzeichnung für unkalibrierte Werte mit '?'
    bool calibrated = (dryR[ch] > 0 || (ch != refChannel && refChannel >= 0)) && (wetR[ch] > 0);
    
    if (idx < 10) lcd.print(" ");
    if (idx < 100) lcd.print(" ");
    lcd.print((int)idx); 
    lcd.print(calibrated ? "% " : "? ");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // I2C Init
  Wire.begin(21, 22);
  
  // Check LCD
  Wire.beginTransmission(0x27);
  if (Wire.endTransmission() == 0) {
    hasLCD = true;
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0,0);
    lcd.print("Hygrometer Init");
  } else {
    hasLCD = false;
    Serial.println("Display (0x27) not found");
  }

  if (!sht31.begin(0x44)) {
    Serial.println("Could not find SHT31 sensor (0x44)");
    hasSHT = false;
  } else {
    hasSHT = true;
  }
  
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
  refChannel = prefs.getInt("refChannel", -1);
  globalWetR = prefs.getFloat("globalWetR", 0);
  
  // MQTT & Config laden
  mqttEnabled = prefs.getBool("mqttEnabled", true);
  lcdEnabled = prefs.getBool("lcdEnabled", true);
  autoRefresh = prefs.getBool("autoRefresh", false);
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
    
    // Read SHT31
    Serial.println("=== Reading Sensors ===");
    if (hasSHT) {
      ambientTemp = sht31.readTemperature();
      ambientHum = sht31.readHumidity();
      
      Serial.print("Sensor 1 (0x44): T="); Serial.print(ambientTemp, 1); Serial.print("°C, H=");
      Serial.print(ambientHum, 1); Serial.println("%");

      if (mqttEnabled && mqtt.connected()) {
        mqtt.publish("hygrometer/ambient/temperature", String(ambientTemp, 2).c_str());
        mqtt.publish("hygrometer/ambient/humidity", String(ambientHum, 2).c_str());
      }
    } else {
      Serial.println("Sensor 1 (0x44): Not connected or error");
    }
    
    // Status des Displays (als Sensor 2 bezeichnet)
    if (hasLCD) {
        Serial.println("Sensor 2 (0x27 Display): Connected");
    } else {
        Serial.println("Sensor 2 (0x27 Display): Not connected or error");
    }
    Serial.println("-------------------");

    Serial.println("\n--- Scanning all channels ---");
    
    // Predetermine reference resistances if configured
    if (refChannel >= 0 && refChannel < NUM_CHANNELS) {
      currentRefR = readChannelResistance(refChannel);
      Serial.print("Dry Ref (CH"); Serial.print(refChannel); Serial.print("): "); Serial.print(currentRefR, 0); Serial.println(" Ohm");
    } else {
      currentRefR = -1.0;
    }

    if (globalWetR > 0) {
       Serial.print("Global Wet Limit: "); Serial.print(globalWetR, 0); Serial.println(" Ohm");
    }

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
      float r = readChannelResistance(ch, nullptr, nullptr, true); // Debug aktiviert
      float idx = indexFromR(r, ch);
      Serial.print("CH"); Serial.print(ch); Serial.print(": R="); Serial.print(r, 0); Serial.print(" Ohm");
      if (idx >= 0) {
        Serial.print(" | Moisture="); Serial.print(idx, 1); Serial.print("%");
      }
      Serial.println();

      // MQTT publish (only if enabled and connected)
      if (mqttEnabled && mqtt.connected()) {
        String topic = String("hygrometer/channel") + ch + "/state";
        String payload = (idx >= 0) ? String(idx,2) : String(r,1);
        mqtt.publish(topic.c_str(), payload.c_str());
      }
      delay(50);
    }

    Serial.print("\nNext scan in "); Serial.print(measureIntervalMs/1000); Serial.println(" seconds\n");
  }

  // LCD Scrolling (alle 5 Sekunden die nächsten 2 Kanäle)
  if (hasLCD && millis() - lastLcdScroll >= 5000) {
    lastLcdScroll = millis();
    updateLcd();
    lcdScrollBatch = (lcdScrollBatch + 1) % 4; // 0, 1, 2, 3 -> zurück zu 0
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
