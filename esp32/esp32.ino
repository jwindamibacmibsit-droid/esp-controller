#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// =====================================================
// HYDROCONTROL ESP32 DEVKIT V1
// pH sensor + ultrasonic water level + 3 active-low relays
// Express backend + Supabase
// =====================================================

// ---------- WIFI AND API ----------
const char WIFI_SSID[] = "balik";
const char WIFI_PASSWORD[] = "buraotka";

const char DEVICE_UID[] = "ESP32-HYDRO-001";
const char API_BASE_URL[] = "https://api.hydrocontrol.site";
const char DEVICE_API_KEY[] = "1578d8bde8d521949ce8495618a32791f48cb0c70ff59c474da5da022a860374";

// Temporary HTTPS testing only.
// setInsecure() does NOT verify the server certificate.
// Configure the trusted root CA before production deployment.
const bool TESTING_WITH_INSECURE_TLS = true;

// ---------- RELAY PINS (ACTIVE LOW) ----------
const int PH_RELAY = 25;
const int NUTRIENT_A_RELAY = 26;
const int NUTRIENT_B_RELAY = 27;

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

// ---------- SENSOR PINS ----------
const int PH_SENSOR_PIN = 34;
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;

// Measure your actual tank height.
const float TANK_HEIGHT_CM = 30.0;

// ---------- pH CALIBRATION ----------
// EXAMPLE VALUES ONLY.
// Replace these with voltages measured using pH 7.00
// and pH 4.00 calibration buffer solutions.
const float PH7_VOLTAGE = 2.50;
const float PH4_VOLTAGE = 3.00;

// ---------- TIMING / SAFETY ----------
const unsigned long SENSOR_INTERVAL = 10000;
const unsigned long COMMAND_INTERVAL = 2000;
const unsigned long WIFI_RETRY_INTERVAL = 5000;
const unsigned long MAX_PUMP_TIME_MS = 5000;
const unsigned long HTTP_TIMEOUT_MS = 8000;

unsigned long lastSensorUpload = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastWiFiRetry = 0;

struct PumpState {
  bool running = false;
  int pin = -1;
  String id = "";
  String name = "";
  unsigned long startedAt = 0;
  unsigned long duration = 0;
};

PumpState activePump;

// =====================================================
// RELAY CONTROL
// =====================================================

void allPumpsOff() {
  digitalWrite(PH_RELAY, RELAY_OFF);
  digitalWrite(NUTRIENT_A_RELAY, RELAY_OFF);
  digitalWrite(NUTRIENT_B_RELAY, RELAY_OFF);
}

void initializeRelays() {
  // Set output latches OFF before enabling output mode.
  digitalWrite(PH_RELAY, RELAY_OFF);
  digitalWrite(NUTRIENT_A_RELAY, RELAY_OFF);
  digitalWrite(NUTRIENT_B_RELAY, RELAY_OFF);

  pinMode(PH_RELAY, OUTPUT);
  pinMode(NUTRIENT_A_RELAY, OUTPUT);
  pinMode(NUTRIENT_B_RELAY, OUTPUT);

  allPumpsOff();
}

int getPumpPin(const String& pump) {
  if (pump == "ph") return PH_RELAY;
  if (pump == "nutrient_a") return NUTRIENT_A_RELAY;
  if (pump == "nutrient_b") return NUTRIENT_B_RELAY;
  return -1;
}

void clearPumpState() {
  activePump.running = false;
  activePump.pin = -1;
  activePump.id = "";
  activePump.name = "";
  activePump.startedAt = 0;
  activePump.duration = 0;
}

// =====================================================
// HTTPS
// =====================================================

bool configureTLS(WiFiClientSecure& client) {
  if (WiFi.status() != WL_CONNECTED) return false;

  client.setTimeout(HTTP_TIMEOUT_MS);

  if (TESTING_WITH_INSECURE_TLS) {
    client.setInsecure();
    Serial.println("WARNING: HTTPS certificate verification is disabled.");
    return true;
  }

  // For production, configure the server's trusted root CA here
  // using client.setCACert(ROOT_CA_CERTIFICATE).
  Serial.println("TLS certificate not configured.");
  return false;
}

void addDeviceHeaders(HTTPClient& http, bool jsonBody = true) {
  if (jsonBody) {
    http.addHeader("Content-Type", "application/json");
  }
  http.addHeader("x-device-token", DEVICE_API_KEY);
}

// =====================================================
// WIFI
// =====================================================

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lastWiFiRetry = millis();
  Serial.println("Connecting to Wi-Fi...");
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  allPumpsOff();

  if (activePump.running) {
    Serial.println("Wi-Fi lost: pump stopped.");
    clearPumpState();
  }

  unsigned long now = millis();

  if (now - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Retrying Wi-Fi...");
  }
}

// =====================================================
// READ PH SENSOR
// =====================================================


float readPH() {
  const int samples = 30;
  uint32_t totalMv = 0;

  for (int i = 0; i < samples; i++) {
    totalMv += analogReadMilliVolts(PH_SENSOR_PIN);
    delay(10);
  }

  float voltage = (totalMv / (float)samples) / 1000.0f;

  Serial.printf("pH sensor voltage: %.3f V\n", voltage);

  // These MUST be measured using your actual sensor.
  // Do not assume the example values are correct.
  float span = PH7_VOLTAGE - PH4_VOLTAGE;

  if (fabsf(span) < 0.05f) {
    Serial.println("ERROR: pH calibration voltages are invalid.");
    return NAN;
  }

  float ph = 7.0f +
      (3.0f / span) * (voltage - PH7_VOLTAGE);

  if (!isfinite(ph) || ph < 0.0f || ph > 14.0f) {
    Serial.println("pH invalid: calibrate the sensor first.");
    return NAN;
  }

  Serial.printf("Estimated pH: %.2f\n", ph);
  return ph;
}


// =====================================================
// READ ULTRASONIC WATER LEVEL
// =====================================================

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0) return -1.0f;

  float distance = duration * 0.0343f / 2.0f;

  if (!isfinite(distance) ||
      distance < 0.0f ||
      distance > TANK_HEIGHT_CM + 10.0f) {
    return -1.0f;
  }

  return distance;
}

float getWaterLevelCm(float distance) {
  if (distance < 0) return -1.0f;

  float level = TANK_HEIGHT_CM - distance;

  if (level < 0) level = 0;
  if (level > TANK_HEIGHT_CM) level = TANK_HEIGHT_CM;

  return level;
}

// =====================================================
// UPLOAD SENSOR DATA
// POST /api/esp/sensors
// =====================================================

void uploadSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;

  float ph = readPH();
  float distance = readDistanceCm();
  float level = getWaterLevelCm(distance);

  bool validPH = isfinite(ph) && ph >= 0 && ph <= 14;
  bool validWater = distance >= 0;

  JsonDocument doc;
  doc["device_uid"] = DEVICE_UID;

  if (validPH) {
    doc["ph"] = ph;
  }

  if (validWater) {
    doc["water_distance_cm"] = distance;
    doc["water_level_cm"] = level;
    doc["water_percentage"] =
        (level / TANK_HEIGHT_CM) * 100.0f;
  }

  String payload;
  serializeJson(doc, payload);

  Serial.println("Sensor payload: " + payload);

  WiFiClientSecure client;
  if (!configureTLS(client)) return;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  String url = String(API_BASE_URL) + "/api/esp/sensors";

  if (!http.begin(client, url)) {
    Serial.println("Failed to initialize sensor request.");
    return;
  }

  addDeviceHeaders(http);

  int status = http.POST(payload);

  Serial.printf("Sensor upload HTTP status: %d\n", status);

  if (status > 0) {
    Serial.println(http.getString());
  } else {
    Serial.println(http.errorToString(status));
  }

  http.end();
}

// =====================================================
// REPORT PUMP RESULT
// POST /api/esp/commands/:id/result
// =====================================================

bool reportPumpResult(
    const String& id,
    const char* status,
    const char* message) {

  if (WiFi.status() != WL_CONNECTED || id.isEmpty()) {
    Serial.println("Cannot report pump result.");
    return false;
  }

  WiFiClientSecure client;
  if (!configureTLS(client)) return false;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  String url = String(API_BASE_URL) +
               "/api/esp/commands/" + id + "/result";

  if (!http.begin(client, url)) return false;

  addDeviceHeaders(http);

  JsonDocument doc;
  doc["status"] = status;
  doc["result_message"] = message;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);

  Serial.printf("Pump result HTTP status: %d\n", code);

  if (code > 0) {
    Serial.println(http.getString());
  } else {
    Serial.println(http.errorToString(code));
  }

  http.end();
  return code >= 200 && code < 300;
}

// =====================================================
// STOP ACTIVE PUMP
// =====================================================

void stopActivePump(const char* message, const char* status) {
  if (!activePump.running) {
    allPumpsOff();
    return;
  }

  String id = activePump.id;
  String name = activePump.name;

  // Switch the relay OFF before making any HTTP request.
  allPumpsOff();
  clearPumpState();

  Serial.printf("Pump %s OFF: %s\n",
                name.c_str(), message);

  reportPumpResult(id, status, message);
}

// =====================================================
// SERVICE PUMP TIMER
// =====================================================

void serviceActivePump() {
  if (!activePump.running) return;

  if (WiFi.status() != WL_CONNECTED) {
    allPumpsOff();
    clearPumpState();
    Serial.println("Pump stopped because Wi-Fi disconnected.");
    return;
  }

  unsigned long elapsed = millis() - activePump.startedAt;

  if (elapsed >= activePump.duration ||
      elapsed >= MAX_PUMP_TIME_MS) {
    stopActivePump(
      "Pump cycle completed; relay switched off",
      "completed"
    );
  }
}

// =====================================================
// START PUMP FROM BACKEND COMMAND
// =====================================================

void executePumpCommand(
    const String& id,
    const String& pump,
    unsigned long durationMs) {

  int pin = getPumpPin(pump);

  if (id.isEmpty() || pin < 0) {
    Serial.println("Rejected invalid pump command.");

    if (!id.isEmpty()) {
      reportPumpResult(
        id, "failed", "Invalid command ID or pump name"
      );
    }
    allPumpsOff();
    return;
  }

  if (activePump.running) {
    reportPumpResult(
      id, "failed", "Another pump is already running"
    );
    return;
  }

  if (durationMs == 0 ||
      durationMs > MAX_PUMP_TIME_MS) {
    reportPumpResult(
      id, "failed", "Duration outside allowed limit"
    );
    allPumpsOff();
    return;
  }

  // Ensure only one pump can run at a time.
  allPumpsOff();

  activePump.running = true;
  activePump.pin = pin;
  activePump.id = id;
  activePump.name = pump;
  activePump.startedAt = millis();
  activePump.duration = durationMs;

  digitalWrite(pin, RELAY_ON);

  Serial.printf(
    "Pump %s ON for %lu ms. Command ID: %s\n",
    pump.c_str(), durationMs, id.c_str()
  );
}

// =====================================================
// POLL BACKEND COMMANDS
// GET /api/esp/commands/next
//
// Expected response:
// {"success":true,"command":null}
//
// Or:
// {"success":true,"command":{
//   "id":"COMMAND_ID",
//   "pump":"ph",
//   "duration_ms":1000
// }}
// =====================================================

void checkPumpCommands() {
  if (WiFi.status() != WL_CONNECTED ||
      activePump.running) {
    return;
  }

  WiFiClientSecure client;
  if (!configureTLS(client)) return;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  String url = String(API_BASE_URL) +
               "/api/esp/commands/next";

  if (!http.begin(client, url)) return;

  addDeviceHeaders(http, false);

  int code = http.GET();

  if (code != 200) {
    Serial.printf("Command poll HTTP status: %d\n", code);

    if (code > 0) {
      Serial.println(http.getString());
    }

    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  Serial.println("Command response: " + response);

  JsonDocument doc;
  DeserializationError error =
      deserializeJson(doc, response);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["command"].is<JsonObject>()) return;

  JsonObject command = doc["command"];

  String id = command["id"] | "";
  String pump = command["pump"] | "";
  long duration = command["duration_ms"] | 0L;

  if (duration <= 0 ||
      duration > (long)MAX_PUMP_TIME_MS) {
    if (!id.isEmpty()) {
      reportPumpResult(
        id, "failed", "Invalid or excessive pump duration"
      );
    }
    return;
  }

  executePumpCommand(
    id, pump, (unsigned long)duration
  );
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  initializeRelays();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(PH_SENSOR_PIN, ADC_11db);

  Serial.println();
  Serial.println("================================");
  Serial.println(" HYDROCONTROL ESP32 STARTING");
  Serial.println("================================");
  Serial.println("All pump relays initialized OFF.");

  startWiFi();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  maintainWiFi();
  serviceActivePump();

  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {

    // Avoid sensor HTTP requests while a pump is active.
    // This helps service the pump timer more frequently.
    if (!activePump.running &&
        now - lastSensorUpload >= SENSOR_INTERVAL) {

      lastSensorUpload = now;
      uploadSensorData();
      serviceActivePump();
    }

    if (!activePump.running &&
        now - lastCommandCheck >= COMMAND_INTERVAL) {

      lastCommandCheck = now;
      checkPumpCommands();
      serviceActivePump();
    }
  }

  serviceActivePump();
  delay(5);
}