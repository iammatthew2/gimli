#include <Adafruit_Protomatter.h>
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

enum class RenderMode { ScrollLeft, ScrollRight, Clear, Test, Test2 };

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

void renderFrameIfDue() {
  uint32_t now = millis();

  if ((renderMode == RenderMode::Test || renderMode == RenderMode::Test2) &&
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

  recalcTextMetrics();
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