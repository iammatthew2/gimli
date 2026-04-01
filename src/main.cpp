#include <Adafruit_LIS3DH.h>
#include <Adafruit_Protomatter.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "secrets.h"

namespace {

constexpr uint16_t MATRIX_WIDTH = 32;
constexpr uint16_t MATRIX_HEIGHT = 16;

constexpr char MQTT_TOPIC_TEXT[] = "apps/gimli/text";
constexpr char MQTT_CLIENT_ID[] = "gimli-matrixportal";

constexpr uint16_t FRAME_MS_DEFAULT = 30;
constexpr uint16_t FRAME_MS_MIN = 10;
constexpr uint16_t FRAME_MS_MAX = 200;
constexpr uint32_t TEST_DURATION_MS = 30000;
constexpr uint32_t ACCEL_SAMPLE_MS = 40;
constexpr float FALL_TRIGGER_DELTA_MS2 = 6.0F;
constexpr float FALL_TILT_SCALE = 0.010F;
constexpr float FALL_BASE_GRAVITY = 0.030F;
constexpr float FALL_DRAG = 0.985F;

constexpr uint32_t WIFI_RETRY_MS = 1000;
constexpr uint32_t MQTT_RETRY_MS = 3000;

uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin = 2;
uint8_t latchPin = 47;
uint8_t oePin = 14;

constexpr uint8_t kNumAddrPins = (MATRIX_HEIGHT == 16)   ? 3
                                 : (MATRIX_HEIGHT == 32) ? 4
                                 : (MATRIX_HEIGHT == 64) ? 5
                                                         : 0;

static_assert(kNumAddrPins != 0,
              "Unsupported matrix height. Use 16, 32, or 64.");

Adafruit_Protomatter matrix(MATRIX_WIDTH, 4, 1, rgbPins, kNumAddrPins, addrPins,
                            clockPin, latchPin, oePin, true);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_LIS3DH accel = Adafruit_LIS3DH();

enum class RenderMode { ScrollLeft, ScrollRight, Clear, Test, Test2, Test3 };

RenderMode renderMode = RenderMode::ScrollLeft;
String currentText = "GIMLI";
int16_t textX = MATRIX_WIDTH;
int16_t textY = 6;
uint16_t textWidth = 0;
uint16_t textHeight = 0;
uint16_t frameDelayMs = FRAME_MS_DEFAULT;
uint16_t textColor = 0xFFFF;

uint32_t lastFrameMs = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t testStartedMs = 0;
uint32_t lastAccelSampleMs = 0;

bool accelReady = false;
bool hasLastAccel = false;
bool test3Falling = false;
float lastAx = 0.0F;
float lastAy = 0.0F;
float lastAz = 0.0F;

struct BlockTemplate {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

constexpr uint8_t TEST3_BLOCK_COUNT = 11;
const BlockTemplate kTest3Template[TEST3_BLOCK_COUNT] = {
    {0, 0, 7, 4, 255, 80, 80},     {9, 1, 4, 3, 90, 220, 120},
    {15, 0, 6, 5, 80, 140, 255},   {23, 2, 8, 4, 255, 180, 60},
    {1, 6, 5, 6, 190, 100, 255},   {8, 6, 9, 3, 80, 230, 210},
    {19, 7, 4, 7, 255, 90, 200},   {24, 8, 7, 3, 255, 255, 110},
    {6, 11, 10, 4, 110, 170, 255}, {18, 12, 5, 3, 255, 120, 120},
    {24, 12, 8, 4, 120, 255, 150},
};

struct LiveBlock {
  float x;
  float y;
  float vx;
  float vy;
  int16_t w;
  int16_t h;
  uint16_t color;
};

LiveBlock test3Blocks[TEST3_BLOCK_COUNT];

void initTest3Blocks() {
  for (uint8_t i = 0; i < TEST3_BLOCK_COUNT; ++i) {
    const BlockTemplate& src = kTest3Template[i];
    test3Blocks[i].x = static_cast<float>(src.x);
    test3Blocks[i].y = static_cast<float>(src.y);
    test3Blocks[i].vx = 0.0F;
    test3Blocks[i].vy = 0.0F;
    test3Blocks[i].w = src.w;
    test3Blocks[i].h = src.h;
    test3Blocks[i].color = matrix.color565(src.r, src.g, src.b);
  }
}

void updateTest3Physics() {
  if (!accelReady) {
    return;
  }

  uint32_t now = millis();
  if ((now - lastAccelSampleMs) < ACCEL_SAMPLE_MS) {
    return;
  }
  lastAccelSampleMs = now;

  sensors_event_t event;
  accel.getEvent(&event);

  float ax = event.acceleration.x;
  float ay = event.acceleration.y;
  float az = event.acceleration.z;

  if (!hasLastAccel) {
    hasLastAccel = true;
    lastAx = ax;
    lastAy = ay;
    lastAz = az;
    return;
  }

  float dax = ax - lastAx;
  float day = ay - lastAy;
  float daz = az - lastAz;
  float deltaMag = sqrtf(dax * dax + day * day + daz * daz);

  if (!test3Falling && deltaMag >= FALL_TRIGGER_DELTA_MS2) {
    test3Falling = true;
    Serial.printf("test3 fall trigger delta=%.2f\n", deltaMag);
  }

  if (test3Falling) {
    for (uint8_t i = 0; i < TEST3_BLOCK_COUNT; ++i) {
      LiveBlock& b = test3Blocks[i];

      b.vx += ax * FALL_TILT_SCALE;
      b.vy += ay * FALL_TILT_SCALE + FALL_BASE_GRAVITY;
      b.vx *= FALL_DRAG;
      b.vy *= FALL_DRAG;
      b.x += b.vx;
      b.y += b.vy;
    }
  }

  lastAx = ax;
  lastAy = ay;
  lastAz = az;
}

void recalcTextMetrics() {
  int16_t x1 = 0;
  int16_t y1 = 0;
  matrix.setTextSize(1);
  matrix.getTextBounds(currentText.c_str(), 0, textY, &x1, &y1, &textWidth,
                       &textHeight);

  if (renderMode == RenderMode::ScrollRight) {
    textX = -static_cast<int16_t>(textWidth);
  } else {
    textX = MATRIX_WIDTH;
  }
}

void applyRenderText(const JsonDocument& doc) {
  const char* incomingText = doc["text"] | "";
  if (incomingText[0] != '\0') {
    currentText = incomingText;
  }

  const char* direction = doc["direction"] | "left";
  if (strcmp(direction, "right") == 0) {
    renderMode = RenderMode::ScrollRight;
  } else {
    renderMode = RenderMode::ScrollLeft;
  }

  int incomingSpeed = doc["speed"] | FRAME_MS_DEFAULT;
  frameDelayMs = static_cast<uint16_t>(
      constrain(incomingSpeed, FRAME_MS_MIN, FRAME_MS_MAX));

  recalcTextMetrics();

  Serial.printf("render_text text=\"%s\" direction=%s speed=%u\n",
                currentText.c_str(), direction, frameDelayMs);
}

void handleTextEvent(const JsonDocument& doc) {
  const char* event = doc["event"] | "";

  if (strcmp(event, "render_text") == 0) {
    applyRenderText(doc);
    return;
  }

  if (strcmp(event, "clear") == 0) {
    renderMode = RenderMode::Clear;
    Serial.println("clear");
    return;
  }

  if (strcmp(event, "test") == 0) {
    renderMode = RenderMode::Test;
    testStartedMs = millis();
    Serial.println("test");
    return;
  }

  if (strcmp(event, "test2") == 0) {
    renderMode = RenderMode::Test2;
    testStartedMs = millis();
    Serial.println("test2");
    return;
  }

  if (strcmp(event, "test3") == 0) {
    renderMode = RenderMode::Test3;
    testStartedMs = millis();
    test3Falling = false;
    hasLastAccel = false;
    initTest3Blocks();
    Serial.println("test3");
    return;
  }

  Serial.printf("Unknown event: %s\n", event);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_TEXT) != 0) {
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("Invalid JSON payload: %s\n", error.c_str());
    return;
  }

  handleTextEvent(doc);
}

void connectWifiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();
  if ((now - lastWifiAttemptMs) < WIFI_RETRY_MS) {
    return;
  }

  lastWifiAttemptMs = now;
  Serial.printf("Connecting to WiFi SSID %s\n", SECRET_SSID);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
}

void connectMqttIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) {
    return;
  }

  uint32_t now = millis();
  if ((now - lastMqttAttemptMs) < MQTT_RETRY_MS) {
    return;
  }

  lastMqttAttemptMs = now;
  Serial.printf("Connecting to MQTT %s:%u\n", MQTT_BROKER, MQTT_PORT);
  if (!mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.printf("MQTT connect failed, rc=%d\n", mqttClient.state());
    return;
  }

  mqttClient.subscribe(MQTT_TOPIC_TEXT);
  Serial.printf("Subscribed: %s\n", MQTT_TOPIC_TEXT);
}

void renderClear() {
  matrix.fillScreen(0);
  matrix.show();
}

void renderScrollLeft() {
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  matrix.setTextColor(textColor);
  matrix.setCursor(textX, textY);
  matrix.print(currentText);
  matrix.show();

  textX -= 1;
  if (textX < -static_cast<int16_t>(textWidth)) {
    renderMode = RenderMode::Clear;
    Serial.println("render_text complete");
  }
}

void renderScrollRight() {
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  matrix.setTextColor(textColor);
  matrix.setCursor(textX, textY);
  matrix.print(currentText);
  matrix.show();

  textX += 1;
  if (textX > static_cast<int16_t>(MATRIX_WIDTH)) {
    renderMode = RenderMode::Clear;
    Serial.println("render_text complete");
  }
}

void renderTestPattern() {
  static uint8_t phase = 0;
  matrix.fillScreen(0);

  bool rowsMode = (millis() - testStartedMs) >= (TEST_DURATION_MS / 2U);

  if (!rowsMode) {
    for (int16_t x = 0; x < MATRIX_WIDTH; ++x) {
      uint8_t section = static_cast<uint8_t>((x + phase) / 4U) % 3U;
      uint16_t color = 0;
      if (section == 0) {
        color = matrix.color565(255, 0, 0);
      } else if (section == 1) {
        color = matrix.color565(0, 255, 0);
      } else {
        color = matrix.color565(0, 0, 255);
      }

      matrix.drawLine(x, 0, x, MATRIX_HEIGHT - 1, color);
    }
  } else {
    for (int16_t y = 0; y < MATRIX_HEIGHT; ++y) {
      uint8_t section = static_cast<uint8_t>((y + phase) / 2U) % 3U;
      uint16_t color = 0;
      if (section == 0) {
        color = matrix.color565(255, 100, 0);
      } else if (section == 1) {
        color = matrix.color565(0, 180, 255);
      } else {
        color = matrix.color565(220, 0, 255);
      }

      matrix.drawLine(0, y, MATRIX_WIDTH - 1, y, color);
    }
  }

  matrix.show();
  phase = static_cast<uint8_t>((phase + 1U) % 24U);
}

void renderTest2Pattern() {
  static uint8_t phase = 0;
  matrix.fillScreen(0);

  for (int16_t y = 0; y < MATRIX_HEIGHT; ++y) {
    for (int16_t x = 0; x < MATRIX_WIDTH; ++x) {
      // Only light a subset of pixels to keep plenty of negative space.
      uint8_t gate = static_cast<uint8_t>((x + (phase / 2U)) % 5U);
      uint8_t band = static_cast<uint8_t>((y + phase) % 4U);
      bool lit = (gate == 0U) && (band <= 1U);
      if (!lit) {
        continue;
      }

      uint8_t v =
          static_cast<uint8_t>(((x * 17) ^ (y * 29) ^ (phase * 11)) & 0xFF);
      uint8_t r = static_cast<uint8_t>((v + (phase * 5U)) & 0xFF);
      uint8_t g = static_cast<uint8_t>((180U + v / 3U) & 0xFF);
      uint8_t b = static_cast<uint8_t>((255U - v) & 0xFF);
      matrix.drawPixel(x, y, matrix.color565(r, g, b));
    }
  }

  int16_t cx = static_cast<int16_t>((phase * 3U) % MATRIX_WIDTH);
  int16_t cy = static_cast<int16_t>((phase * 2U) % MATRIX_HEIGHT);
  matrix.drawLine(cx, 0, MATRIX_WIDTH - 1 - cx, MATRIX_HEIGHT - 1,
                  matrix.color565(120, 120, 120));
  if ((phase % 2U) == 0U) {
    matrix.drawLine(0, cy, MATRIX_WIDTH - 1, MATRIX_HEIGHT - 1 - cy,
                    matrix.color565(160, 120, 20));
  }

  matrix.show();
  phase = static_cast<uint8_t>((phase + 1U) % 64U);
}

void renderTest3Pattern() {
  updateTest3Physics();
  matrix.fillScreen(0);

  for (uint8_t i = 0; i < TEST3_BLOCK_COUNT; ++i) {
    const LiveBlock& b = test3Blocks[i];
    int16_t x = static_cast<int16_t>(b.x + 0.5F);
    int16_t y = static_cast<int16_t>(b.y + 0.5F);
    matrix.fillRect(x, y, b.w, b.h, b.color);
  }

  matrix.show();
}

void renderFrameIfDue() {
  uint32_t now = millis();

  if ((renderMode == RenderMode::Test || renderMode == RenderMode::Test2 ||
       renderMode == RenderMode::Test3) &&
      (now - testStartedMs) >= TEST_DURATION_MS) {
    renderMode = RenderMode::Clear;
    Serial.println("test complete (30s)");
  }

  if ((now - lastFrameMs) < frameDelayMs) {
    return;
  }
  lastFrameMs = now;

  switch (renderMode) {
    case RenderMode::ScrollLeft:
      renderScrollLeft();
      break;
    case RenderMode::ScrollRight:
      renderScrollRight();
      break;
    case RenderMode::Clear:
      renderClear();
      break;
    case RenderMode::Test:
      renderTestPattern();
      break;
    case RenderMode::Test2:
      renderTest2Pattern();
      break;
    case RenderMode::Test3:
      renderTest3Pattern();
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);

  ProtomatterStatus matrixStatus = matrix.begin();
  Serial.printf("Protomatter begin status: %d\n", matrixStatus);

  if (matrixStatus != PROTOMATTER_OK) {
    while (true) {
      delay(1000);
    }
  }

  if (accel.begin(0x19)) {
    accelReady = true;
    accel.setRange(LIS3DH_RANGE_4_G);
    Serial.println("LIS3DH ready at 0x19");
  } else {
    Serial.println("LIS3DH not detected at 0x19 (test3 will remain static)");
  }

  recalcTextMetrics();
  initTest3Blocks();
  renderScrollLeft();

  WiFi.mode(WIFI_STA);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  connectWifiIfNeeded();
  connectMqttIfNeeded();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  renderFrameIfDue();
}