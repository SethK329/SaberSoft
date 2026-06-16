// ============================================================
//  lightsaber.ino / main.cpp
//  ESP32-S3 NeoPixel Lightsaber Firmware
//  Framework: Arduino  |  Library: Adafruit NeoPixel
//
//  Architecture sections (search for the === markers):
//    1. CONSTANTS & CONFIGURATION
//    2. INCLUDES & GLOBALS
//    3. STATE MACHINE
//    4. BUTTON HANDLING
//    5. ANIMATIONS
//    6. COLOR SYSTEM
//    7. WI-FI CONFIG MODE
//    8. PERSISTENCE / NVS STORAGE
//    9. DEEP SLEEP HELPERS  (stubs for future use)
//   10. SETUP & LOOP
// ============================================================


// ============================================================
//  SECTION 1 — CONSTANTS & CONFIGURATION
//  Edit everything in this block to match your hardware.
// ============================================================

// --- LED Strip ---
#define LED_PIN        5        // GPIO pin connected to LED data line
#define LED_COUNT      60      // number of LEDs in your strip
#define LED_TYPE       NEO_GRB + NEO_KHZ800  // WS2812B = GRB; SK6812 = change to NEO_GRBW

// --- Brightness (0–255) ---
#define BRIGHTNESS_DEFAULT  125   // startup brightness (NVS value overrides this)
#define BRIGHTNESS_MIN        5   // lowest allowed brightness
#define BRIGHTNESS_MAX      125   // highest allowed brightness      

// --- Button ---
#define BUTTON_PIN      4         // GPIO for the momentary push button
// Button is wired: GPIO → button → GND (internal pull-up enabled in code)

// --- Timing (milliseconds) ---
#define DEBOUNCE_MS          50   // ignore noise shorter than this
#define SHORT_PRESS_MAX     500   // press < 500 ms = short press (color cycle)
#define LONG_PRESS_MIN      600   // press ≥ 600 ms = long press  (on/off toggle)
#define CONFIG_HOLD_MS     5000   // hold 5 s → enter Wi-Fi config mode
#define CONFIG_EXIT_HOLD_MS 1000  // hold 1 s in config mode → exit Wi-Fi config mode

// --- Animation speeds ---
#define IGNITE_STEP_MS    12      // ms between each LED lighting during ignition
#define RETRACT_STEP_MS   10      // ms between each LED turning off during retraction
#define COLOR_FADE_STEPS  30      // number of steps for in-blade color crossfade
#define COLOR_FADE_MS     15      // ms between each crossfade step

// --- Motion / MPU6050 ---
#define MOTION_SDA_PIN              8    // MPU6050 SDA
#define MOTION_SCL_PIN              9    // MPU6050 SCL
#define MPU6050_ADDR             0x68    // AD0 tied LOW; use 0x69 if AD0 is HIGH
#define MOTION_SAMPLE_MS           10    // 100 Hz motion sampling
#define MOTION_LOG_MS             500    // throttle Serial logs to 2 per second
#define CLASH_EFFECT_MS           180    // white flash duration after a solid impact
#define CLASH_FLASH_WIDTH          13    // localized clash flash width in LEDs
#define CLASH_COOLDOWN_MS         350    // ignore repeat hits during blade bounce
#define CLASH_MIN_TOTAL_G        2.80f   // absolute acceleration required for impact
#define CLASH_MIN_DELTA_G        1.65f   // acceleration spike required for impact
#define CLASH_LOCATION_WINDOW_MS   30    // short peak capture window for zone estimate
#define CLASH_LOCATION_SAMPLE_MS    3    // delay between extra peak samples
#define CLASH_SCORE_MIN_ACCEL_G  0.10f   // guard against divide-by-zero / invalid data
#define CLASH_SCORE_BOTTOM_MAX   25.0f   // below this = clear bottom hit
#define CLASH_SCORE_TOP_MIN     110.0f   // above this can be a clear top hit
#define CLASH_TOP_MIN_GYRO_DPS  220.0f   // top hit must have real rotational energy
#define SWING_GYRO_DPS         180.0f    // subtle brightness response while swinging

// --- Wi-Fi Access Point ---
#define WIFI_SSID   "SaberConfig"   // AP name (no password)
#define WIFI_PORT   80               // web server port

// --- NVS storage keys ---
#define NVS_NAMESPACE   "saber"
#define NVS_KEY_COLOR   "color_idx"
#define NVS_KEY_BRIGHT  "brightness"
#define NVS_KEY_CUSTOM  "custom_rgb"


// ============================================================
//  SECTION 2 — INCLUDES & GLOBALS
// ============================================================

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>          // ESP32 NVS wrapper (built into Arduino-ESP32)
#include <WiFi.h>
#include <WebServer.h>            // ESP32 built-in non-blocking webserver
#include <Wire.h>

// --- NeoPixel strip object ---
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, LED_TYPE);

// --- NVS preferences ---
Preferences prefs;

// --- Web server (only active in CONFIG_MODE) ---
WebServer server(WIFI_PORT);

// --- Runtime state ---
uint8_t  currentBrightness = BRIGHTNESS_DEFAULT;
uint8_t  currentColorIdx   = 0;     // index into the color palette
bool     saberIsOn         = false;


// ============================================================
//  SECTION 3 — STATE MACHINE
//
//  States:
//    OFF        — blade dark, waiting for input
//    IGNITING   — ignition animation running base→tip
//    ON         — blade solid, responding to button
//    RETRACTING — retraction animation running tip→base
//    CONFIG     — Wi-Fi AP active, webserver running
// ============================================================

enum SaberState {
    STATE_OFF,
    STATE_IGNITING,
    STATE_ON,
    STATE_RETRACTING,
    STATE_CONFIG
};

SaberState currentState = STATE_OFF;

enum ClashZone {
    CLASH_ZONE_BOTTOM = 0,
    CLASH_ZONE_MIDDLE = 1,
    CLASH_ZONE_TOP = 2
};

// Animation progress tracking
int  animLedIndex   = 0;       // which LED the animation is currently at
long animLastStep   = 0;       // millis() timestamp of last animation tick

// Color fade tracking
bool    fadingColor    = false;
uint8_t fadeStep       = 0;
uint32_t fadeFromColor = 0;
uint32_t fadeToColor   = 0;
long    fadeLastStep   = 0;

// Motion tracking
bool  mpuReady             = false;
long  motionLastSample     = 0;
long  motionLastLog        = 0;
long  clashLastTrigger     = 0;
float lastAccelMagnitudeG  = 1.0f;
float accelXG              = 0.0f;
float accelYG              = 0.0f;
float accelZG              = 0.0f;
float gyroXDps             = 0.0f;
float gyroYDps             = 0.0f;
float gyroZDps             = 0.0f;

// Clash effect tracking
bool clashActive           = false;
long clashStartedAt        = 0;
long clashLastStep         = 0;
ClashZone currentClashZone = CLASH_ZONE_MIDDLE;


// ============================================================
//  SECTION 6 — COLOR SYSTEM  (defined early; used everywhere)
//
//  Add or remove colors from this palette freely.
//  Colors are stored as packed 32-bit RGB: 0x00RRGGBB
// ============================================================

struct SaberColor {
    const char* name;
    uint8_t r, g, b;
};

// ---- Preset color palette ----
SaberColor COLOR_PALETTE[] = {
    {"Blue", 0x00, 0x60, 0xFF},
    {"Green", 0x00, 0xDD, 0x00},
    {"Red", 0xFF, 0x00, 0x00},
    {"Purple", 0x40, 0x00, 0xFF},
    {"Yellow", 0xDF, 0xFF, 0x00},
    {"White", 0xEF, 0xEF, 0xFF},
    {"Custom", 0xFF, 0x99, 0x00}
};

const uint8_t PALETTE_SIZE = sizeof(COLOR_PALETTE) / sizeof(COLOR_PALETTE[0]);
const uint8_t CUSTOM_COLOR_IDX = PALETTE_SIZE - 1;

// Returns the current blade color as a packed strip color value
uint32_t currentBladeColor() {
    const SaberColor& c = COLOR_PALETTE[currentColorIdx];
    return strip.Color(c.r, c.g, c.b);
}

// Interpolate between two packed colors by fraction t (0–255)
uint32_t lerpColor(uint32_t from, uint32_t to, uint8_t t) {
    uint8_t fr = (from >> 16) & 0xFF;
    uint8_t fg = (from >>  8) & 0xFF;
    uint8_t fb = (from      ) & 0xFF;
    uint8_t tr = (to   >> 16) & 0xFF;
    uint8_t tg = (to   >>  8) & 0xFF;
    uint8_t tb = (to        ) & 0xFF;

    uint8_t r = fr + ((int)(tr - fr) * t / 255);
    uint8_t g = fg + ((int)(tg - fg) * t / 255);
    uint8_t b = fb + ((int)(tb - fb) * t / 255);
    return strip.Color(r, g, b);
}


// ============================================================
//  SECTION 4 — BUTTON HANDLING
//
//  A single button on BUTTON_PIN with internal pull-up.
//  LOW  = pressed  (button connects pin to GND)
//  HIGH = released
//
//  Timing logic:
//    pressStart   = millis() when LOW first detected (after debounce)
//    On release:
//      duration < SHORT_PRESS_MAX  → short press  → cycle color
//      duration ≥ LONG_PRESS_MIN   → long press   → toggle saber on/off
//    While held:
//      duration ≥ CONFIG_HOLD_MS   → enter config mode
//      duration ≥ CONFIG_EXIT_HOLD_MS in CONFIG → exit config mode
// ============================================================

// Forward declarations
void startIgnition();
void startRetraction();
void cycleColor();
void enterConfigMode();
void exitConfigMode();
void loadSettings();
void saveSettings();
void saveSettingsFromWeb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
void initMotion();
void tickMotion();
void tickClashEffect();
void triggerClashEffect(float totalG, float deltaG);
ClashZone classifyClashZone(float accelMagnitudeG, float gyroMagnitudeDps, float& impactScore);

// Button state
bool     btnLastRaw      = HIGH;    // last raw GPIO read
bool     btnDebounced    = HIGH;    // debounced state
long     btnLastChange   = 0;       // millis() of last raw-state change (for debounce)
long     btnPressStart   = 0;       // millis() when button was pressed
bool     btnHeld         = false;   // true while button is held down
bool     configTriggered = false;   // prevent re-triggering config during same hold

void handleButton() {
    bool raw = digitalRead(BUTTON_PIN);  // LOW when pressed

    // --- Debounce ---
    if (raw != btnLastRaw) {
        btnLastChange = millis();
        btnLastRaw    = raw;
    }

    if ((millis() - btnLastChange) < DEBOUNCE_MS) {
        return;  // still bouncing, ignore
    }

    bool pressed = (raw == LOW);

    // --- Detect press start ---
    if (pressed && !btnHeld) {
        btnHeld         = true;
        btnPressStart   = millis();
        configTriggered = false;
    }

    // --- While held: handle config mode entry/exit ---
    if (btnHeld && pressed && !configTriggered) {
        long holdDuration = millis() - btnPressStart;

        if (currentState == STATE_CONFIG && holdDuration >= CONFIG_EXIT_HOLD_MS) {
            configTriggered = true;  // don't trigger again this hold
            exitConfigMode();
            return;
        }

        if (holdDuration >= CONFIG_HOLD_MS) {
            configTriggered = true;  // don't trigger again this hold
            enterConfigMode();
            return;
        }
    }

    // --- Detect release ---
    if (!pressed && btnHeld) {
        btnHeld = false;

        if (configTriggered) {
            return;  // config was already handled; ignore the release
        }

        long duration = millis() - btnPressStart;

        if (duration < SHORT_PRESS_MAX) {
            // Short press → cycle color (only if saber is ON or OFF)
            if (currentState == STATE_ON) {
                cycleColor();
            }
        } else if (duration >= LONG_PRESS_MIN) {
            // Long press → toggle on/off
            if (currentState == STATE_OFF) {
                startIgnition();
            } else if (currentState == STATE_ON) {
                startRetraction();
            }
            // Ignore long press during animations or config
        }
    }

    btnDebounced = pressed ? LOW : HIGH;
}
// ============================================================
//  SECTION 8 — PERSISTENCE / NVS STORAGE
//
//  We store:
//    color_idx  — palette index (0–PALETTE_SIZE-1)
//    brightness — uint8 (5–255)
//    custom_rgb — packed RGB color for the custom palette slot
// ============================================================

void loadSettings()
{
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    const SaberColor& defaultCustom = COLOR_PALETTE[CUSTOM_COLOR_IDX];
    uint32_t customRgb = prefs.getUInt(NVS_KEY_CUSTOM,
                                       ((uint32_t)defaultCustom.r << 16) |
                                       ((uint32_t)defaultCustom.g << 8) |
                                       (uint32_t)defaultCustom.b);
    currentColorIdx = prefs.getUChar(NVS_KEY_COLOR, 0);
    currentBrightness = prefs.getUChar(NVS_KEY_BRIGHT, BRIGHTNESS_DEFAULT);
    prefs.end();

    COLOR_PALETTE[CUSTOM_COLOR_IDX].r = (customRgb >> 16) & 0xFF;
    COLOR_PALETTE[CUSTOM_COLOR_IDX].g = (customRgb >> 8) & 0xFF;
    COLOR_PALETTE[CUSTOM_COLOR_IDX].b = customRgb & 0xFF;

    // Bounds check
    if (currentColorIdx >= PALETTE_SIZE)
        currentColorIdx = 0;
    currentBrightness = constrain(currentBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

    Serial.printf("[NVS] Loaded: color=%s brightness=%d\n",
                  COLOR_PALETTE[currentColorIdx].name, currentBrightness);
}

void saveSettings()
{
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putUChar(NVS_KEY_COLOR, currentColorIdx);
    prefs.putUChar(NVS_KEY_BRIGHT, currentBrightness);
    prefs.end();
    Serial.printf("[NVS] Saved: color=%s brightness=%d\n",
                  COLOR_PALETTE[currentColorIdx].name, currentBrightness);
}

// when custom color picker is used update custom color slot in pallete
uint8_t closestPaletteIndex(uint8_t r, uint8_t g, uint8_t b)
{
    // Update the custom color slot directly in the palette
    COLOR_PALETTE[CUSTOM_COLOR_IDX].r = r;
    COLOR_PALETTE[CUSTOM_COLOR_IDX].g = g;
    COLOR_PALETTE[CUSTOM_COLOR_IDX].b = b;
    return CUSTOM_COLOR_IDX; // Return the index of the custom slot
}

// Called after web UI save — finds closest preset, saves brightness
void saveSettingsFromWeb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    currentColorIdx = closestPaletteIndex(r, g, b);
    currentBrightness = constrain(brightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    strip.setBrightness(currentBrightness);
    saveSettings();
}

// ============================================================
//  SECTION 5 — ANIMATIONS
// ============================================================

// ---- Color cycle (called on short press) ----
void cycleColor() {
    uint8_t nextIdx = (currentColorIdx + 1) % PALETTE_SIZE;

    if (currentState == STATE_ON) {
        // Fade to new color while blade is live
        fadeFromColor  = currentBladeColor();
        currentColorIdx = nextIdx;
        fadeToColor    = currentBladeColor();
        fadeStep       = 0;
        fadingColor    = true;
        fadeLastStep   = millis();
        Serial.printf("[Color] Fading to %s\n", COLOR_PALETTE[nextIdx].name);
    }

    saveSettings();   // persist the new selection
}

// ---- Tick the color crossfade (non-blocking, called from loop) ----
void tickColorFade() {
    if (!fadingColor) return;
    if ((millis() - fadeLastStep) < COLOR_FADE_MS) return;

    fadeLastStep = millis();
    fadeStep++;

    if (fadeStep >= COLOR_FADE_STEPS) {
        // Fade complete — set all LEDs to final color
        strip.fill(currentBladeColor());
        strip.show();
        fadingColor = false;
        return;
    }

    // Interpolate
    uint8_t t = (uint8_t)((fadeStep * 255) / COLOR_FADE_STEPS);
    uint32_t blended = lerpColor(fadeFromColor, fadeToColor, t);
    strip.fill(blended);
    strip.show();
}

// ---- Ignition: light LEDs base→tip ----
void startIgnition() {
    if (currentState != STATE_OFF) return;

    currentState = STATE_IGNITING;
    animLedIndex = 0;
    animLastStep = millis();
    fadingColor  = false;

    Serial.printf("[Saber] Igniting — %s\n", COLOR_PALETTE[currentColorIdx].name);
}

void tickIgnition() {
    if (currentState != STATE_IGNITING) return;
    if ((millis() - animLastStep) < IGNITE_STEP_MS) return;

    animLastStep = millis();

    // Slight brightness ramp: full brightness after ~25% of blade is lit
    float progress = (float)animLedIndex / (float)LED_COUNT;
    uint8_t ramped = (uint8_t)(currentBrightness * min(1.0f, progress * 4.0f));
    strip.setBrightness(max((uint8_t)BRIGHTNESS_MIN, ramped));

    uint32_t color = currentBladeColor();
    strip.setPixelColor(animLedIndex, color);
    strip.show();

    animLedIndex++;

    if (animLedIndex >= LED_COUNT) {
        // Animation complete — snap to full brightness and go ON
        strip.setBrightness(currentBrightness);
        strip.fill(currentBladeColor());
        strip.show();
        currentState = STATE_ON;
        saberIsOn    = true;
        Serial.println("[Saber] ON");
    }
}

// ---- Retraction: turn off LEDs tip→base ----
void startRetraction() {
    if (currentState != STATE_ON) return;

    currentState = STATE_RETRACTING;
    animLedIndex = LED_COUNT - 1;  // start at the tip
    animLastStep = millis();
    fadingColor  = false;

    Serial.println("[Saber] Retracting");
}

void tickRetraction() {
    if (currentState != STATE_RETRACTING) return;
    if ((millis() - animLastStep) < RETRACT_STEP_MS) return;

    animLastStep = millis();

    strip.setPixelColor(animLedIndex, 0);  // turn off this LED
    strip.show();

    animLedIndex--;

    if (animLedIndex < 0) {
        // All LEDs off — go to OFF state
        strip.clear();
        strip.show();
        currentState = STATE_OFF;
        saberIsOn    = false;
        Serial.println("[Saber] OFF");
    }
}
long flickerLastStep = 0;

void tickFlicker()
{
    if (clashActive)
        return;
    if (fadingColor)
        return;
    if ((millis() - flickerLastStep) < 90)
        return;
    flickerLastStep = millis();

    uint8_t minB = (currentBrightness > 18)
                       ? currentBrightness - 18
                       : BRIGHTNESS_MIN;
    float gyroMagnitude = sqrtf(gyroXDps * gyroXDps + gyroYDps * gyroYDps + gyroZDps * gyroZDps);
    uint8_t swingBoost = (uint8_t)constrain((gyroMagnitude - SWING_GYRO_DPS) * 0.08f, 0.0f, 22.0f);
    uint8_t flickerB = minB + (uint8_t)random(0, currentBrightness - minB + 1);
    flickerB = min((uint8_t)BRIGHTNESS_MAX, (uint8_t)(flickerB + swingBoost));

    strip.setBrightness(flickerB);
    strip.fill(currentBladeColor());
    strip.show();
}

// ============================================================
//  SECTION 5B — MOTION & CLASH EFFECTS
// ============================================================

bool writeMpuRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readMpuSample()
{
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(0x3B);  // ACCEL_XOUT_H
    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)14) != 14)
        return false;

    int16_t ax = (Wire.read() << 8) | Wire.read();
    int16_t ay = (Wire.read() << 8) | Wire.read();
    int16_t az = (Wire.read() << 8) | Wire.read();
    Wire.read(); Wire.read();  // temperature, unused
    int16_t gx = (Wire.read() << 8) | Wire.read();
    int16_t gy = (Wire.read() << 8) | Wire.read();
    int16_t gz = (Wire.read() << 8) | Wire.read();

    // Configured for +/-8g and +/-500 dps in initMotion().
    accelXG = (float)ax / 4096.0f;
    accelYG = (float)ay / 4096.0f;
    accelZG = (float)az / 4096.0f;
    gyroXDps = (float)gx / 65.5f;
    gyroYDps = (float)gy / 65.5f;
    gyroZDps = (float)gz / 65.5f;

    return true;
}

float currentAccelMagnitudeG()
{
    return sqrtf(accelXG * accelXG + accelYG * accelYG + accelZG * accelZG);
}

float currentGyroMagnitudeDps()
{
    return sqrtf(gyroXDps * gyroXDps + gyroYDps * gyroYDps + gyroZDps * gyroZDps);
}

void captureClashPeaks(float& peakAccelG, float& peakGyroDps)
{
    long startedAt = millis();

    while ((millis() - startedAt) < CLASH_LOCATION_WINDOW_MS) {
        delay(CLASH_LOCATION_SAMPLE_MS);
        if (!readMpuSample())
            continue;

        peakAccelG = max(peakAccelG, currentAccelMagnitudeG());
        peakGyroDps = max(peakGyroDps, currentGyroMagnitudeDps());
    }
}

ClashZone classifyClashZone(float accelMagnitudeG, float gyroMagnitudeDps, float& impactScore)
{
    impactScore = 0.0f;

    if (!isfinite(accelMagnitudeG) || !isfinite(gyroMagnitudeDps) ||
        accelMagnitudeG < CLASH_SCORE_MIN_ACCEL_G || gyroMagnitudeDps < 0.0f) {
        return CLASH_ZONE_MIDDLE;
    }

    impactScore = gyroMagnitudeDps / max(accelMagnitudeG, CLASH_SCORE_MIN_ACCEL_G);
    if (!isfinite(impactScore)) {
        impactScore = 0.0f;
        return CLASH_ZONE_MIDDLE;
    }

    if (impactScore < CLASH_SCORE_BOTTOM_MAX)
        return CLASH_ZONE_BOTTOM;
    if (impactScore >= CLASH_SCORE_TOP_MIN && gyroMagnitudeDps >= CLASH_TOP_MIN_GYRO_DPS)
        return CLASH_ZONE_TOP;
    return CLASH_ZONE_MIDDLE;
}

int clashZoneCenterLed(ClashZone zone)
{
    switch (zone) {
        case CLASH_ZONE_BOTTOM:
            return 10;
        case CLASH_ZONE_TOP:
            return 50;
        case CLASH_ZONE_MIDDLE:
        default:
            return 30;
    }
}

void initMotion()
{
    Wire.begin(MOTION_SDA_PIN, MOTION_SCL_PIN);
    Wire.setClock(400000);

    delay(50);
    Wire.beginTransmission(MPU6050_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[Motion] MPU6050 not found at 0x%02X (SDA GPIO%d, SCL GPIO%d)\n",
                      MPU6050_ADDR, MOTION_SDA_PIN, MOTION_SCL_PIN);
        mpuReady = false;
        return;
    }

    bool configured = true;
    configured &= writeMpuRegister(0x6B, 0x00);  // PWR_MGMT_1: wake
    configured &= writeMpuRegister(0x1A, 0x03);  // CONFIG: DLPF ~44 Hz accel / 42 Hz gyro
    configured &= writeMpuRegister(0x1B, 0x08);  // GYRO_CONFIG: +/-500 dps
    configured &= writeMpuRegister(0x1C, 0x10);  // ACCEL_CONFIG: +/-8g

    mpuReady = configured && readMpuSample();
    lastAccelMagnitudeG = sqrtf(accelXG * accelXG + accelYG * accelYG + accelZG * accelZG);

    if (mpuReady) {
        Serial.printf("[Motion] MPU6050 ready on SDA GPIO%d / SCL GPIO%d\n",
                      MOTION_SDA_PIN, MOTION_SCL_PIN);
        Serial.println("[Motion] Logs: ax/ay/az in g, gx/gy/gz in deg/s, total and delta in g");
    } else {
        Serial.println("[Motion] MPU6050 detected but could not be configured");
    }
}

void tickMotion()
{
    if (!mpuReady)
        return;
    if ((millis() - motionLastSample) < MOTION_SAMPLE_MS)
        return;
    motionLastSample = millis();

    if (!readMpuSample()) {
        Serial.println("[Motion] MPU6050 read failed");
        return;
    }

    float totalG = currentAccelMagnitudeG();
    float deltaG = fabsf(totalG - lastAccelMagnitudeG);
    lastAccelMagnitudeG = (lastAccelMagnitudeG * 0.72f) + (totalG * 0.28f);

    if ((millis() - motionLastLog) >= MOTION_LOG_MS) {
        motionLastLog = millis();
        Serial.printf("[Motion] ax=%+.2fg ay=%+.2fg az=%+.2fg total=%.2fg delta=%.2fg | gx=%+.0f gy=%+.0f gz=%+.0f dps\n",
                      accelXG, accelYG, accelZG, totalG, deltaG, gyroXDps, gyroYDps, gyroZDps);
    }

    bool impactIsHardEnough = totalG >= CLASH_MIN_TOTAL_G && deltaG >= CLASH_MIN_DELTA_G;
    bool clashCooledDown = (millis() - clashLastTrigger) >= CLASH_COOLDOWN_MS;
    if (impactIsHardEnough && clashCooledDown && !clashActive) {
        triggerClashEffect(totalG, deltaG);
    }
}

void triggerClashEffect(float totalG, float deltaG)
{
    float peakAccelG = totalG;
    float peakGyroDps = currentGyroMagnitudeDps();
    float impactScore = 0.0f;

    captureClashPeaks(peakAccelG, peakGyroDps);
    currentClashZone = classifyClashZone(peakAccelG, peakGyroDps, impactScore);

    clashActive = true;
    clashStartedAt = millis();
    clashLastStep = 0;
    clashLastTrigger = millis();
    fadingColor = false;

    Serial.printf("[Clash] accel=%.2fg gyro=%.0fdps score=%.1f zone=%d delta=%.2fg\n",
                  peakAccelG, peakGyroDps, impactScore, (int)currentClashZone, deltaG);
}

void tickClashEffect()
{
    if (!clashActive)
        return;

    long elapsed = millis() - clashStartedAt;
    if (elapsed >= CLASH_EFFECT_MS) {
        clashActive = false;
        strip.setBrightness(currentBrightness);
        strip.fill(currentBladeColor());
        strip.show();
        return;
    }

    if ((millis() - clashLastStep) < 18)
        return;
    clashLastStep = millis();

    uint8_t whiteLevel = (elapsed < 45) ? 255 : (uint8_t)map(elapsed, 45, CLASH_EFFECT_MS, 225, 80);
    int centerLed = clashZoneCenterLed(currentClashZone);
    int firstClashLed = constrain(centerLed - ((CLASH_FLASH_WIDTH - 1) / 2), 0, LED_COUNT - 1);
    int lastClashLed = constrain(centerLed + (CLASH_FLASH_WIDTH / 2), firstClashLed, LED_COUNT - 1);

    strip.setBrightness(BRIGHTNESS_MAX);
    strip.fill(currentBladeColor());
    for (int i = firstClashLed; i <= lastClashLed; i++) {
        uint8_t shimmer = random(0, 50);
        uint8_t c = (uint8_t)max(90, whiteLevel - shimmer);
        strip.setPixelColor(i, strip.Color(c, c, c));
    }
    strip.show();
}

// ============================================================
//  SECTION 7 — WI-FI CONFIGURATION MODE
//
//  Triggered by 5-second hold.
//  ESP32 creates a soft AP named "SaberConfig" (no password).
//  A tiny single-page HTML app lets the user:
//    • pick a blade color via color picker
//    • adjust brightness via slider
//    • save settings to NVS
//  After saving, Wi-Fi shuts down and the saber goes to OFF.
//
//  The webserver uses ESP32's built-in WebServer which is
//  event-driven — server.handleClient() is called from loop()
//  so it never blocks.
// ============================================================

// Forward declarations
void loadSettings();
void saveSettingsFromWeb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
void exitConfigMode();
// Minimal embedded HTML/CSS/JS config page
// Stored in PROGMEM to save RAM
static const char CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saber Config</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0a0a12;color:#e0e0ff;font-family:system-ui,sans-serif;
       display:flex;flex-direction:column;align-items:center;min-height:100vh;
       padding:2rem 1rem}
  h1{font-size:1.6rem;letter-spacing:.15em;margin-bottom:.3rem;color:#8af}
  .sub{color:#556;font-size:.85rem;margin-bottom:2rem}
  .card{background:#13131f;border:1px solid #223;border-radius:12px;
        padding:1.5rem;width:100%;max-width:340px;display:flex;
        flex-direction:column;gap:1.4rem}
  label{font-size:.8rem;letter-spacing:.08em;color:#889;margin-bottom:.4rem;
        display:block}
  input[type=color]{width:100%;height:52px;border:none;border-radius:8px;
                    cursor:pointer;background:none;padding:0}
  input[type=range]{width:100%;accent-color:#8af}
  .row{display:flex;justify-content:space-between;align-items:center;
       font-size:.9rem}
  button{width:100%;padding:.9rem;border:none;border-radius:8px;
         background:#3355ff;color:#fff;font-size:1rem;font-weight:600;
         letter-spacing:.05em;cursor:pointer;transition:background .2s}
  button:hover{background:#5577ff}
  #status{text-align:center;font-size:.85rem;min-height:1.2em;color:#8f8}
  #preview{width:100%;height:14px;border-radius:4px;margin-top:.2rem;
            transition:background .2s}
</style>
</head>
<body>
<h1>&#9889; SABER CONFIG</h1>
<p class="sub">Connected to SaberConfig AP</p>
<div class="card">
  <div>
    <label>BLADE COLOR</label>
    <input type="color" id="col" value="#0060ff">
    <div id="preview" style="background:#0060ff"></div>
  </div>
  <div>
    <label>BRIGHTNESS</label>
    <div class="row">
      <span>&#9651; Dim</span>
      <span>Bright &#9651;</span>
    </div>
    <input type="range" id="brt" min="5" max="255" value="160">
  </div>
  <p id="status"></p>
  <button onclick="save()">SAVE &amp; CLOSE</button>
</div>
<script>
const col=document.getElementById('col');
const brt=document.getElementById('brt');
const preview=document.getElementById('preview');
const status=document.getElementById('status');

col.addEventListener('input',()=>preview.style.background=col.value);

function save(){
  status.textContent='Saving…';
  const hex=col.value.replace('#','');
  const r=parseInt(hex.substring(0,2),16);
  const g=parseInt(hex.substring(2,4),16);
  const b=parseInt(hex.substring(4,6),16);
  fetch('/save?r='+r+'&g='+g+'&b='+b+'&brt='+brt.value)
    .then(res=>res.text())
    .then(t=>{
      status.textContent=t;
      setTimeout(()=>{status.textContent='Wi-Fi shutting down…'},1800);
    })
    .catch(()=>status.textContent='Error — try again');
}
</script>
</body>
</html>
)rawhtml";

// --- Serve the config page ---
void handleRoot() {
    server.send_P(200, "text/html", CONFIG_HTML);
}

// --- Handle /save?r=&g=&b=&brt= ---
void handleSave() {
    if (!server.hasArg("r") || !server.hasArg("g") ||
        !server.hasArg("b") || !server.hasArg("brt")) {
        server.send(400, "text/plain", "Missing parameters");
        return;
    }

    uint8_t r   = (uint8_t)constrain(server.arg("r").toInt(),   0, 255);
    uint8_t g   = (uint8_t)constrain(server.arg("g").toInt(),   0, 255);
    uint8_t b   = (uint8_t)constrain(server.arg("b").toInt(),   0, 255);
    uint8_t brt = (uint8_t)constrain(server.arg("brt").toInt(), BRIGHTNESS_MIN, BRIGHTNESS_MAX);

    Serial.printf("[Config] Saving r=%d g=%d b=%d brt=%d\n", r, g, b, brt);
    saveSettingsFromWeb(r, g, b, brt);

    server.send(200, "text/plain", "Saved! Returning to normal mode.");

    // Schedule Wi-Fi shutdown after response is sent
    // We set a flag and handle it in loop() to avoid blocking here
    // (A small delay is acceptable here since we're in CONFIG state)
    delay(2000);
    exitConfigMode();
}

// Track whether config mode is active so loop() can handle client
bool configModeActive = false;

void enterConfigMode() {
    if (currentState == STATE_CONFIG) return;  // already in config

    Serial.println("[Config] Entering Wi-Fi config mode");

    // Turn blade off cleanly first
    strip.clear();
    strip.show();
    currentState = STATE_CONFIG;
    saberIsOn    = false;
    fadingColor  = false;

    // Flash the first few LEDs purple briefly to signal config entry
    for (int i = 0; i < min(5, LED_COUNT); i++) {
        strip.setPixelColor(i, strip.Color(120, 0, 200));
    }
    strip.show();
    delay(400);
    strip.clear();
    strip.show();

    // Start Wi-Fi AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID);  // open, no password
    delay(100);

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[Config] AP started. Connect to '%s' → http://%s\n",
                  WIFI_SSID, ip.toString().c_str());

    // Register URL handlers
    server.on("/",     handleRoot);
    server.on("/save", handleSave);
    server.onNotFound([]() {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });

    server.begin();
    configModeActive = true;

    Serial.println("[Config] Webserver running");
}

void exitConfigMode() {
    Serial.println("[Config] Exiting — shutting down Wi-Fi");

    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    configModeActive = false;
    currentState     = STATE_OFF;

    Serial.println("[Config] Wi-Fi off. Returning to normal operation.");
}




// ============================================================
//  SECTION 9 — DEEP SLEEP HELPERS  (stubs for future use)
//
//  Deep sleep will cut power to the LED strip and radio.
//  To wake, configure ext0 wakeup on the button GPIO.
//  These are intentionally left as stubs — integrate when
//  you add a battery gauge or auto-sleep timer.
// ============================================================

// Configure the button as ext0 wakeup source and enter deep sleep.
// Call from loop() after a long idle period if desired.
void goDeepSleep() {
    Serial.println("[Sleep] Entering deep sleep — press button to wake");
    strip.clear();
    strip.show();
    delay(100);

    // Button is active-LOW (internal pull-up), so wake on LOW level
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
    esp_deep_sleep_start();
    // Execution resumes at setup() after wake
}

// Check what caused the last wake-up (for future auto-sleep logic)
void printWakeReason() {
    esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
    if (reason == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("[Boot] Woke from deep sleep via button");
    } else {
        Serial.println("[Boot] Normal power-on boot");
    }
}


// ============================================================
//  SECTION 10 — SETUP & LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(300);   // brief pause for USB CDC to initialise on ESP32-S3
    Serial.println("\n[Boot] Lightsaber firmware starting");

    // --- Print wakeup reason (future deep-sleep support) ---
    printWakeReason();

    // --- Button pin ---
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("[Boot] Button on GPIO%d (active LOW, internal pull-up)\n", BUTTON_PIN);

    // --- NeoPixel strip ---
    strip.begin();
    strip.setBrightness(BRIGHTNESS_DEFAULT);
    strip.clear();
    strip.show();
    Serial.printf("[Boot] LED strip: %d pixels on GPIO%d\n", LED_COUNT, LED_PIN);

    // --- Load saved settings from NVS ---
    loadSettings();
    strip.setBrightness(currentBrightness);

    // --- Motion sensor ---
    initMotion();

    // --- Wi-Fi completely off at boot ---
    WiFi.mode(WIFI_OFF);
    Serial.println("[Boot] Wi-Fi disabled");

    // --- Startup blink: briefly flash 1 LED to confirm power ---
    strip.setPixelColor(0, strip.Color(30, 30, 30));
    strip.show();
    delay(120);
    strip.clear();
    strip.show();

    Serial.println("[Boot] Ready. Long-press button to ignite.");
}

void loop() {
    // ---- Handle button input every loop tick ----
    handleButton();

    // ---- Run the appropriate state logic ----
    switch (currentState) {

        case STATE_OFF:
            // Nothing to do — just waiting for input
            break;

        case STATE_IGNITING:
            tickIgnition();
            break;

        case STATE_ON:
            // Motion sampling drives swing brightness and solid-impact clashes
            tickMotion();
            tickClashEffect();

            // Tick the color crossfade if one is in progress
            if (!clashActive) {
                tickColorFade();
                tickFlicker();
            }
            break;

        case STATE_RETRACTING:
            tickRetraction();
            break;

        case STATE_CONFIG:
            // Serve web requests — non-blocking; returns immediately if no client
            if (configModeActive) {
                server.handleClient();
            }
            break;
    }

    // ---- Yield to RTOS / watchdog ----
    // ESP32 Arduino framework feeds the watchdog automatically in most cases,
    // but explicit yields prevent rare starvation in tight loops.
    yield();
}


// ============================================================
//  FUTURE EXTENSION HOOKS  (not implemented — placeholders)
//
//  Search for "TODO:" to find all future extension points.
//
//  TODO: Sound — add DFPlayer Mini or I2S DAC; trigger on
//        ignite/retract/clash. Pin: define SOUND_TX_PIN.
//
//  DONE: Motion — MPU6050 via I2C on GPIO8/GPIO9; detects
//        swings and solid-impact clashes in STATE_ON.
//
//  DONE: Clash effect — bright white flash + shimmer for
//        ~300 ms, then return to blade color.
//
//  DONE: Blade flicker — random subtle brightness variation
//        in STATE_ON to simulate plasma instability.
//
//  TODO: Gesture controls — double-swing for color change,
//        horizontal flick for power toggle, etc.
//
//  TODO: Auto-sleep — track millis() since last interaction;
//        after e.g. 5 minutes in STATE_OFF, call goDeepSleep().
// ============================================================
