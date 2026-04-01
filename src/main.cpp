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

enum class RenderMode { ScrollLeft, ScrollRight, Clear, Test };

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
    Serial.println("test");
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
    textX = MATRIX_WIDTH;
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
    textX = -static_cast<int16_t>(textWidth);
  }
}

void renderTestPattern() {
  static uint8_t phase = 0;
  matrix.fillScreen(0);

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

  matrix.show();
  phase = static_cast<uint8_t>((phase + 1U) % 12U);
}

void renderFrameIfDue() {
  uint32_t now = millis();
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