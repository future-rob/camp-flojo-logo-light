/*
 * Minimal ESP32 NeoPixel controller with Wi-Fi + web UI.
 * - Serves an HTML control panel from SPIFFS.
 * - Exposes REST endpoints so the UI (or other clients) can change color/effects.
 * - Falls back to AP mode if station connection fails.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>

constexpr uint16_t MaxPixelCount = 144; // Common LED strip size
constexpr uint8_t PixelPin = 12;
constexpr uint8_t AnimationChannels = 1;
constexpr uint8_t SnakeSegmentLength = 5;
constexpr uint16_t SnakeStepDelayMs = 80;

uint16_t pixelCount = 12; // Default to 12 pixels

// Update with your own network credentials. Device falls back to AP mode if STA fails.
const char *WIFI_SSID = "AndroidAPF863";
const char *WIFI_PASSWORD = "juro4090";
const char *AP_SSID = "NeoPixel-Control";
const char *AP_PASSWORD = "12345678";

AsyncWebServer server(80);

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(MaxPixelCount, PixelPin);
NeoPixelAnimator animations(AnimationChannels);

boolean fadeToColor = true;
bool snakeDirty = true;
uint16_t snakeHead = 0;
unsigned long lastSnakeStepMs = 0;

struct FadeChannelState
{
  RgbColor StartingColor;
  RgbColor EndingColor;
};

FadeChannelState fadeChannels[AnimationChannels];

enum class EffectMode
{
  Fade,
  Solid,
  Snake,
  Off
};

struct StripState
{
  EffectMode effect = EffectMode::Fade;
  RgbColor solidColor = RgbColor(255, 80, 10);
  uint8_t brightness = 160;
};

StripState stripState;
bool solidDirty = true;
bool offDirty = true;
bool wifiConnected = false;

void SetRandomSeed();
void BlendAnimUpdate(const AnimationParam &param);
void FadeInFadeOutRinseRepeat(float luminance);
void ensureEffectIsRunning();
void initSPIFFS();
void initNetworking();
void configureRoutes();
void handleControlRequest(AsyncWebServerRequest *request);
String buildStateJson();
String modeToString(EffectMode mode);
bool applyModeFromString(const String &value);
bool setSolidColor(uint8_t r, uint8_t g, uint8_t b);
bool setBrightness(uint8_t value);
bool setPixelCount(uint16_t count);
float brightnessScale();
float brightnessToLuminance();
RgbColor applyBrightness(const RgbColor &color);
void turnStripOff();
void applySolidColor();
void writeColorToActivePixels(const RgbColor &color);
RgbColor scaleColor(const RgbColor &color, float scale);
void resetSnakeEffect();
void runSnakeEffect();

void setup()
{
  Serial.begin(115200);
  delay(200);

  initSPIFFS();

  strip.Begin();
  strip.Show();
  SetRandomSeed();

  initNetworking();
  configureRoutes();
  server.begin();

  Serial.println("NeoPixel controller ready.");
}

void loop()
{
  ensureEffectIsRunning();
  delay(1);
}

void initSPIFFS()
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("Failed to mount SPIFFS");
  }
}

void initNetworking()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  const unsigned long start = millis();
  const unsigned long timeout = 15000;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout)
  {
    delay(300);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.print("\nConnected with IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println("\nSTA connection failed, enabling AP mode.");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Connect to ");
  Serial.print(AP_SSID);
  Serial.print(" and browse to http://");
  Serial.println(WiFi.softAPIP());
}

void configureRoutes()
{
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", buildStateJson()); });

  server.on("/api/control", HTTP_GET, [](AsyncWebServerRequest *request)
            { handleControlRequest(request); });

  server.onNotFound([](AsyncWebServerRequest *request)
                    { request->send(404, "text/plain", "Not found"); });
}

void handleControlRequest(AsyncWebServerRequest *request)
{
  bool changed = false;
  bool colorChanged = false;

  if (request->hasParam("mode"))
  {
    changed |= applyModeFromString(request->getParam("mode")->value());
  }

  if (request->hasParam("brightness"))
  {
    int value = request->getParam("brightness")->value().toInt();
    changed |= setBrightness(static_cast<uint8_t>(constrain(value, 0, 255)));
  }

  if (request->hasParam("count"))
  {
    int value = request->getParam("count")->value().toInt();
    changed |= setPixelCount(static_cast<uint16_t>(constrain(value, 1, MaxPixelCount)));
  }

  if (request->hasParam("r") && request->hasParam("g") && request->hasParam("b"))
  {
    int r = request->getParam("r")->value().toInt();
    int g = request->getParam("g")->value().toInt();
    int b = request->getParam("b")->value().toInt();
    colorChanged |= setSolidColor(constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
    changed |= colorChanged;
  }

  // Don't automatically switch to solid mode when color changes

  String payload = buildStateJson();
  if (changed)
  {
    Serial.println("State updated via web UI: " + payload);
  }
  request->send(200, "application/json", payload);
}

String buildStateJson()
{
  IPAddress currentIp = wifiConnected ? WiFi.localIP() : WiFi.softAPIP();

  String json = "{";
  json += "\"mode\":\"" + modeToString(stripState.effect) + "\",";
  json += "\"brightness\":" + String(stripState.brightness) + ",";
  json += "\"color\":{";
  json += "\"r\":" + String(stripState.solidColor.R) + ",";
  json += "\"g\":" + String(stripState.solidColor.G) + ",";
  json += "\"b\":" + String(stripState.solidColor.B) + "},";
  json += "\"count\":" + String(pixelCount) + ",";
  json += "\"ip\":\"" + currentIp.toString() + "\"";
  json += "}";
  return json;
}

String modeToString(EffectMode mode)
{
  switch (mode)
  {
  case EffectMode::Solid:
    return "solid";
  case EffectMode::Snake:
    return "snake";
  case EffectMode::Off:
    return "off";
  default:
    return "fade";
  }
}

bool applyModeFromString(const String &value)
{
  String lowered = value;
  lowered.toLowerCase();

  EffectMode next = stripState.effect;
  if (lowered == "solid")
  {
    next = EffectMode::Solid;
  }
  else if (lowered == "off")
  {
    next = EffectMode::Off;
  }
  else if (lowered == "fade")
  {
    next = EffectMode::Fade;
  }
  else if (lowered == "snake")
  {
    next = EffectMode::Snake;
  }
  else
  {
    return false;
  }

  if (stripState.effect == next)
  {
    return false;
  }

  stripState.effect = next;
  animations.StopAll();
  fadeToColor = true;
  solidDirty = true;
  offDirty = true;
  snakeDirty = true;
  return true;
}

bool setSolidColor(uint8_t r, uint8_t g, uint8_t b)
{
  if (stripState.solidColor.R == r && stripState.solidColor.G == g && stripState.solidColor.B == b)
  {
    return false;
  }

  stripState.solidColor = RgbColor(r, g, b);
  solidDirty = true;
  snakeDirty = true;
  return true;
}

bool setBrightness(uint8_t value)
{
  uint8_t constrained = constrain(value, 0, 255);
  if (stripState.brightness == constrained)
  {
    return false;
  }

  stripState.brightness = constrained;
  solidDirty = true;
  offDirty = true;
  snakeDirty = true;
  animations.StopAll();
  return true;
}

bool setPixelCount(uint16_t count)
{
  uint16_t newCount = constrain(count, 1, MaxPixelCount);
  if (pixelCount == newCount)
  {
    return false;
  }

  pixelCount = newCount;
  solidDirty = true;
  offDirty = true;
  snakeDirty = true;
  animations.StopAll();
  return true;
}

float brightnessScale()
{
  float percent = static_cast<float>(stripState.brightness) / 255.0f;
  return percent * percent; // Gamma correction to make low values dimmer
}

float brightnessToLuminance()
{
  float luminance = brightnessScale() * 0.5f;
  if (stripState.brightness == 0)
  {
    return 0.0f;
  }
  return max(0.002f, luminance);
}

RgbColor applyBrightness(const RgbColor &color)
{
  return scaleColor(color, brightnessScale());
}

RgbColor scaleColor(const RgbColor &color, float scale)
{
  float clamped = constrain(scale, 0.0f, 1.0f);
  uint8_t r = static_cast<uint8_t>(color.R * clamped);
  uint8_t g = static_cast<uint8_t>(color.G * clamped);
  uint8_t b = static_cast<uint8_t>(color.B * clamped);
  return RgbColor(r, g, b);
}

void writeColorToActivePixels(const RgbColor &color)
{
  for (uint16_t pixel = 0; pixel < pixelCount; ++pixel)
  {
    strip.SetPixelColor(pixel, color);
  }
  // Clear any pixels beyond pixelCount
  for (uint16_t pixel = pixelCount; pixel < MaxPixelCount; ++pixel)
  {
    strip.SetPixelColor(pixel, RgbColor(0));
  }
}

void applySolidColor()
{
  writeColorToActivePixels(applyBrightness(stripState.solidColor));
  strip.Show();
  solidDirty = false;
  offDirty = true;
}

void turnStripOff()
{
  writeColorToActivePixels(RgbColor(0));
  strip.Show();
  offDirty = false;
  solidDirty = true;
}

void resetSnakeEffect()
{
  snakeHead = 0;
  lastSnakeStepMs = 0;
  snakeDirty = false;
  writeColorToActivePixels(RgbColor(0));
  strip.Show();
}

void runSnakeEffect()
{
  if (snakeDirty)
  {
    resetSnakeEffect();
  }

  unsigned long now = millis();
  if (lastSnakeStepMs && now - lastSnakeStepMs < SnakeStepDelayMs)
  {
    return;
  }

  lastSnakeStepMs = now;
  RgbColor baseColor = applyBrightness(stripState.solidColor);

  writeColorToActivePixels(RgbColor(0));
  for (uint8_t offset = 0; offset < SnakeSegmentLength; ++offset)
  {
    uint16_t pixel = snakeHead + offset;
    if (pixel >= pixelCount)
    {
      break;
    }

    float fade = 1.0f - (static_cast<float>(offset) / SnakeSegmentLength);
    strip.SetPixelColor(pixel, scaleColor(baseColor, fade));
  }

  strip.Show();

  snakeHead = (snakeHead + 1) % pixelCount;
}

void ensureEffectIsRunning()
{
  switch (stripState.effect)
  {
  case EffectMode::Fade:
    offDirty = true;
    solidDirty = true;
    if (!animations.IsAnimating())
    {
      FadeInFadeOutRinseRepeat(brightnessToLuminance());
    }
    animations.UpdateAnimations();
    strip.Show();
    break;

  case EffectMode::Solid:
    if (animations.IsAnimating())
    {
      animations.StopAll();
    }
    if (solidDirty)
    {
      applySolidColor();
    }
    break;

  case EffectMode::Snake:
    if (animations.IsAnimating())
    {
      animations.StopAll();
    }
    offDirty = true;
    solidDirty = true;
    runSnakeEffect();
    break;

  case EffectMode::Off:
    if (animations.IsAnimating())
    {
      animations.StopAll();
    }
    if (offDirty)
    {
      turnStripOff();
    }
    break;
  }
}

void SetRandomSeed()
{
  uint32_t seed = analogRead(0);
  delay(1);
  for (int shifts = 3; shifts < 31; shifts += 3)
  {
    seed ^= analogRead(0) << shifts;
    delay(1);
  }
  randomSeed(seed);
}

void BlendAnimUpdate(const AnimationParam &param)
{
  RgbColor updatedColor = RgbColor::LinearBlend(
      fadeChannels[param.index].StartingColor,
      fadeChannels[param.index].EndingColor,
      param.progress);

  // Only update if in fade mode to prevent interference
  if (stripState.effect == EffectMode::Fade)
  {
    writeColorToActivePixels(updatedColor);
  }
}

void FadeInFadeOutRinseRepeat(float luminance)
{
  // Generate a new random color target
  RgbColor target = HslColor(random(360) / 360.0f, 1.0f, luminance);

  // Use longer, smoother fade times for gentle color transitions
  uint16_t time = random(2000, 4000);

  // Always start from the current ending color for smooth blending
  if (animations.IsAnimating())
  {
    // Animation is running, start from where it's currently heading
    fadeChannels[0].StartingColor = fadeChannels[0].EndingColor;
  }
  else
  {
    // First time or animation just finished, use the current displayed color
    // Read from the strip to get the actual current color
    RgbColor currentColor = strip.GetPixelColor(0);
    fadeChannels[0].StartingColor = currentColor;
  }

  // Set the new target color
  fadeChannels[0].EndingColor = target;
  animations.StartAnimation(0, time, BlendAnimUpdate);
}
