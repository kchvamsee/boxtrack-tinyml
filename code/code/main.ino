/*
  BoxTrack — ESP32-S3 WebSocket Server
  =====================================
  Uses: MPU6050 by Electronic Cats
        WebSockets by Markus Sattler (v2.7.2)
        ArduinoJson by Benoit Blanchon (v7.4.3)
        Edge Impulse: Boxing_master_inferencing

  Model specs (from your EI export):
    - Frequency    : 83.33 Hz  -> INTERVAL_MS = 12
    - Raw samples  : 166
    - Axes per frame: 6  (ax, ay, az, gx, gy, gz)
    - Labels       : 4  (hook, jab, rest, uppercut)
*/

#include <Wire.h>
#include <MPU6050.h>                          // Electronic Cats
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Boxing_master_inferencing.h>        // your EI library

// ── WiFi credentials ──
const char* SSID     = "<enter you ssid>";
const char* PASSWORD = "<enter your password>";

// ── WebSocket server on port 81 ──
WebSocketsServer wsServer(81);

// ── MPU6050 ──
MPU6050 mpu;
int16_t ax, ay, az;
int16_t gx, gy, gz;

// ── Accel + gyro scale factors (default MPU6050 ranges) ──
// Accel: +-2g  -> divide by 16384.0
// Gyro : +-250 deg/s -> divide by 131.0
#define ACCEL_SCALE   16384.0f
#define GYRO_SCALE    131.0f

// ── Inference timing ──
// EI_CLASSIFIER_INTERVAL_MS = 12  (83.33 Hz)
// EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME = 6  (ax,ay,az,gx,gy,gz)
// EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE = 166 * 6 = 996
#define CONFIDENCE_THRESHOLD  0.70f

static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static int   feature_ix     = 0;
static long  last_sample_ms = 0;

// ── WebSocket event handler ──
void onWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = wsServer.remoteIP(num);
    Serial.printf("[WS] Client #%u connected from %s\n", num, ip.toString().c_str());
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Client #%u disconnected\n", num);
  }
}

// ── Broadcast detected move as JSON ──
void broadcastMove(const char* label, float confidence) {
  if (strcmp(label, "rest") == 0) return;           // silently ignore rest
  if (confidence < CONFIDENCE_THRESHOLD) return;    // ignore low confidence

  StaticJsonDocument<128> doc;
  doc["label"]      = label;
  doc["confidence"] = confidence;

  char buf[128];
  serializeJson(doc, buf);
  wsServer.broadcastTXT(buf);

  Serial.printf("[MOVE] %s  (%.0f%%)\n", label, confidence * 100);
}

// ── Run Edge Impulse inference ──
void runInference() {
  signal_t signal;
  numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

  ei_impulse_result_t result;
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("[EI] Classifier error: %d\n", err);
    return;
  }

  // Debug: print all scores
  Serial.print("[EI] ");
  for (uint8_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.printf("%s:%.2f  ", result.classification[i].label,
                               result.classification[i].value);
  }
  Serial.println();

  // Find highest confidence
  float   best_score = 0;
  uint8_t best_idx   = 0;
  for (uint8_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best_score) {
      best_score = result.classification[i].value;
      best_idx   = i;
    }
  }

  broadcastMove(result.classification[best_idx].label, best_score);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // ── Init I2C + MPU6050 ──
  Wire.begin();
  mpu.initialize();

  if (!mpu.testConnection()) {
    Serial.println("MPU6050 connection FAILED - check wiring!");
    while (1);
  }
  Serial.println("MPU6050 connected OK");
  Serial.printf("EI frame size: %d  (samples: %d x axes: %d)\n",
    EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
    EI_CLASSIFIER_RAW_SAMPLE_COUNT,
    EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME);

  // ── Connect WiFi ──
  Serial.printf("Connecting to %s", SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.printf("\nConnected!  IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf(">>> Paste into BoxTrack: ws://%s:81\n", WiFi.localIP().toString().c_str());

  // ── Start WebSocket server ──
  wsServer.begin();
  wsServer.onEvent(onWSEvent);
  Serial.println("WebSocket server started on port 81");
}

void loop() {
  wsServer.loop();

  // Sample at 83.33 Hz (every 12 ms — matches EI_CLASSIFIER_INTERVAL_MS)
  if (millis() - last_sample_ms >= EI_CLASSIFIER_INTERVAL_MS) {
    last_sample_ms = millis();

    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

    // Fill 6 values per sample: ax, ay, az, gx, gy, gz
    // This matches RAW_SAMPLES_PER_FRAME = 6
    if (feature_ix + 6 <= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      features[feature_ix++] = (float)ax / ACCEL_SCALE;
      features[feature_ix++] = (float)ay / ACCEL_SCALE;
      features[feature_ix++] = (float)az / ACCEL_SCALE;
      features[feature_ix++] = (float)gx / GYRO_SCALE;
      features[feature_ix++] = (float)gy / GYRO_SCALE;
      features[feature_ix++] = (float)gz / GYRO_SCALE;
    }

    // Buffer full -> run inference then reset
    if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      runInference();
      feature_ix = 0;
    }
  }
}

/*
  JSON sent to browser on each detected move:
  { "label": "jab",      "confidence": 0.93 }
  { "label": "hook",     "confidence": 0.88 }
  { "label": "uppercut", "confidence": 0.91 }

  "rest" is silently ignored and never sent to browser.
*/
