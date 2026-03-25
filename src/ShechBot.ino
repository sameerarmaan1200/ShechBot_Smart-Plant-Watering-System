/* ======================================================================== *
 * SHECHBOT PROJECT - FINAL CODE                          *
 * ======================================================================== */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <RTClib.h>
#include <EEPROM.h>

// ------------------------------------------------------------------------
// Hardware Initialization
// ------------------------------------------------------------------------

// LCD (I2C address 0x27, 20x4)
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;

// Keypad configuration
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS] = {A1, A2, 10, 2};
Keypad kp = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Actuators & sensors
const int pumpPin    = 3;       // PWM pin for pump (with MOSFET)
const int sensorPin  = A3;      // Soil moisture sensor
const int batteryPin = A0;      // Battery voltage divider

// Battery calibration
const float V_REF      = 5.0;   // Arduino supply voltage
const float R1         = 10000.0; // Voltage divider R1 (10k)
const float R2         = 2200.0;  // Voltage divider R2 (2.2k)
const float BATT_MAX_V = 12.6;  // Li-ion 3S fully charged
const float BATT_MIN_V = 10.5;  // Li-ion 3S discharged


// ------------------------------------------------------------------------
// EEPROM Settings Storage (Using Struct + Checksum)
// ------------------------------------------------------------------------

const int EEPROM_SETTINGS_ADDR = 0;   // Start address for settings block

struct Settings {
  uint16_t moisture;      // Target moisture % (0-100)
  uint16_t hour;          // Scheduled hour (0-23)
  uint16_t minute;        // Scheduled minute (0-59)
  uint16_t pumpSeconds;   // Pump run time in seconds (1-300)
  uint16_t checksum;      // XOR of all previous fields
};

Settings settings;
const Settings DEFAULT_SETTINGS = {50, 16, 0, 30, 0};

// Compute simple XOR checksum
uint16_t computeChecksum(const Settings& s) {
  return s.moisture ^ s.hour ^ s.minute ^ s.pumpSeconds;
}

// Load settings from EEPROM; fallback to defaults if corrupted
void loadSettings() {
  EEPROM.get(EEPROM_SETTINGS_ADDR, settings);
  uint16_t calc = computeChecksum(settings);
  
  if (calc != settings.checksum) {
    settings = DEFAULT_SETTINGS;
    settings.checksum = computeChecksum(settings);
    saveSettings();
  }
}

// Save settings to EEPROM (updates checksum automatically)
void saveSettings() {
  settings.checksum = computeChecksum(settings);
  EEPROM.put(EEPROM_SETTINGS_ADDR, settings);
}


// ------------------------------------------------------------------------
// Global Variables
// ------------------------------------------------------------------------

int lastWateredDay = -1;          // For time-based irrigation (day stamp)
String inputBuffer = "";          // Numeric input buffer

bool pumpRunning = false;
unsigned long pumpStartTime = 0;
unsigned long lastMoistureWatering = 0;
const unsigned long MOISTURE_COOLDOWN = 300000;  // 5 min between moisture irrigations

// Screen saver
unsigned long lastInteractionTime = 0;
const unsigned long SCREEN_TIMEOUT = 60000;      // 60 seconds
bool screenOn = true;

// Message display for "Moisture OK" in time mode
bool timeMessageActive = false;
unsigned long timeMessageStart = 0;
const unsigned long TIME_MESSAGE_DURATION = 2000; // 2 seconds

// Menu states
enum SystemState {
  MAIN_MENU,
  MODE_TIME,
  MODE_MOIST,
  SET_MOIST,
  SET_HOUR,
  SET_MIN,
  VIEW_BATT,
  SET_PUMP
};

SystemState currentState = MAIN_MENU;


// ------------------------------------------------------------------------
// Helper Functions
// ------------------------------------------------------------------------

int getMoisture() {
  int raw = analogRead(sensorPin);
  // Calibration: 1023 (dry air) … 200 (in water)
  int m = map(raw, 1023, 200, 0, 100);
  return constrain(m, 0, 100);
}

void triggerPump() {
  pumpRunning = true;
  pumpStartTime = millis();

  // Wake screen to show pump active
  lcd.backlight();
  screenOn = true;
  lastInteractionTime = millis();

  lcd.clear();
  lcd.setCursor(4, 1); lcd.print("PUMP ACTIVE");
  lcd.setCursor(3, 2); lcd.print("TIME: ");
  lcd.print(settings.pumpSeconds);
  lcd.print("s");

  // Smooth start (optional)
  for (int i = 0; i <= 255; i += 51) {
    analogWrite(pumpPin, i);
    delay(40);
  }
}

void stopPump() {
  analogWrite(pumpPin, 0);
  digitalWrite(pumpPin, LOW);
  pumpRunning = false;

  lcd.clear();
  lcd.setCursor(1, 1); lcd.print("WATERING FINISHED!");
  delay(1500);
  goBack();
}

void printTime(int h, int m) {
  if (h < 10) lcd.print("0");
  lcd.print(h);
  lcd.print(":");
  if (m < 10) lcd.print("0");
  lcd.print(m);
}

void goBack() {
  currentState = MAIN_MENU;
  drawMainMenu();
}

void drawMainMenu() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("[A]TIME-M [B]MOIST-M");
  lcd.setCursor(0, 1); lcd.print("[C]SetTme [D]SetMst");
  lcd.setCursor(0, 2); lcd.print("[0]Pump Duration");
  lcd.setCursor(0, 3); lcd.print("[*]Battery [#]Exit");
}

void handleNumberInput(char key) {
  if (key >= '0' && key <= '9' && inputBuffer.length() < 3) {
    inputBuffer += key;
  } else if (key == 'D' && inputBuffer.length() > 0) {
    inputBuffer.remove(inputBuffer.length() - 1);   // BACKSPACE
  }
}

void confirmSave() {
  lcd.clear();
  lcd.setCursor(4, 1); lcd.print("DATA SAVED!");
  delay(1000);
}


// ------------------------------------------------------------------------
// Irrigation Checks
// ------------------------------------------------------------------------

// Time-based irrigation check (with message on moisture OK)
void checkTimeIrrigation() {
  DateTime now = rtc.now();
  
  // Only act if the current time matches the set time and we haven't processed today yet
  if (now.hour() == settings.hour && now.minute() == settings.minute && now.day() != lastWateredDay) {
    lastWateredDay = now.day();   // Mark as processed for today (whether we water or not)
    int moisture = getMoisture();
    
    if (moisture < settings.moisture) {
      // Moisture too low – water
      triggerPump();
    } else {
      // Moisture sufficient – show message
      timeMessageActive = true;
      timeMessageStart = millis();
    }
  }
}

// Moisture-based irrigation check (with cooldown)
void checkMoistIrrigation() {
  if (lastMoistureWatering != 0 && (millis() - lastMoistureWatering < MOISTURE_COOLDOWN)) return;
  
  if (getMoisture() < settings.moisture) {
    triggerPump();
    lastMoistureWatering = millis();
    if (lastMoistureWatering == 0) lastMoistureWatering = 1; // avoid zero ambiguity
  }
}


// ------------------------------------------------------------------------
// Display Functions (Each updates its respective screen)
// ------------------------------------------------------------------------

void runTimeModeDisplay() {
  static unsigned long lastU = 0;
  if (millis() - lastU < 1000) return;
  lastU = millis();

  DateTime now = rtc.now();
  lcd.setCursor(0, 0); lcd.print("--- TIME CONTROL ---");
  lcd.setCursor(0, 1); lcd.print("Live Time: "); printTime(now.hour(), now.minute());
  lcd.setCursor(0, 2); lcd.print("Set Time : "); printTime(settings.hour, settings.minute);

  // Check if we need to show the "Moisture OK" message
  if (timeMessageActive) {
    if (millis() - timeMessageStart < TIME_MESSAGE_DURATION) {
      lcd.setCursor(0, 3); lcd.print("  Moisture OK!     ");
    } else {
      timeMessageActive = false;   // message expires
      lcd.setCursor(0, 3); lcd.print("Waiting...   [#]Menu");
    }
  } else {
    lcd.setCursor(0, 3); lcd.print("Waiting...   [#]Menu");
  }
}

void runMoistModeDisplay() {
  static unsigned long lastU = 0;
  if (millis() - lastU < 500) return;
  lastU = millis();

  lcd.setCursor(0, 0); lcd.print("- MOISTURE CONTROL -");
  lcd.setCursor(0, 1); lcd.print("Live Moist: "); lcd.print(getMoisture()); lcd.print("%  ");
  lcd.setCursor(0, 2); lcd.print("Target    : "); lcd.print(settings.moisture); lcd.print("%  ");
  lcd.setCursor(0, 3); lcd.print("Monitoring.. [#]Menu");
}

void runBatteryDisplay() {
  static unsigned long lastU = 0;
  if (millis() - lastU < 1000) return;
  lastU = millis();

  float voltage = (analogRead(batteryPin) * V_REF / 1024.0) * ((R1 + R2) / R2);
  int percent = constrain(map(voltage * 10, BATT_MIN_V * 10, BATT_MAX_V * 10, 0, 100), 0, 100);

  lcd.setCursor(0, 0); lcd.print("-- BATTERY STATUS --");
  lcd.setCursor(0, 1); lcd.print("Voltage: "); lcd.print(voltage, 2); lcd.print(" V");
  lcd.setCursor(0, 2); lcd.print("Level  : "); lcd.print(percent); lcd.print("%");
  lcd.setCursor(0, 3); lcd.print("Press [#] to Exit");
}

// --- SET screens with DELETE only (D key) ---

void runSetMoistDisplay() {
  static unsigned long lastU = 0;
  if (millis() - lastU < 500) return;
  lastU = millis();

  lcd.setCursor(0, 0); lcd.print("- SET MOISTURE % -");
  lcd.setCursor(0, 1); lcd.print("Live Moist: "); lcd.print(getMoisture()); lcd.print("%  ");
  lcd.setCursor(0, 2); lcd.print("New Target: "); lcd.print(inputBuffer); lcd.print("_   ");
  lcd.setCursor(0, 3); lcd.print("[*]Save[#]Back[D]Del");
}

void runSetPumpDisplay() {
  lcd.setCursor(0, 0); lcd.print("- PUMP DURATION -");
  lcd.setCursor(0, 1); lcd.print("Current: "); lcd.print(settings.pumpSeconds); lcd.print("s");
  lcd.setCursor(0, 2); lcd.print("New Set: "); lcd.print(inputBuffer); lcd.print("_");
  lcd.setCursor(0, 3); lcd.print("[*]Save[#]Back[D]Del");
}

void runSetTimeDisplay(bool isHour) {
  DateTime now = rtc.now();
  lcd.setCursor(0, 0); lcd.print("- SET START TIME -");
  lcd.setCursor(0, 1); lcd.print("Live Time: "); printTime(now.hour(), now.minute());
  lcd.setCursor(0, 2); lcd.print(isHour ? " Set Hour: " : "Set Min : ");
  lcd.print(inputBuffer); lcd.print("_");
  lcd.setCursor(0, 3); lcd.print("[*]Save[#]Back[D]Del");
}


// ------------------------------------------------------------------------
// Main Setup
// ------------------------------------------------------------------------

void setup() {
  pinMode(pumpPin, OUTPUT);
  digitalWrite(pumpPin, LOW);

  lcd.init();
  lcd.backlight();

  if (!rtc.begin()) {
    lcd.print("RTC Error!");
    while (1);
  }

  // IMPORTANT: Only set the RTC once when first programming.
  // If you need to set the time, uncomment the line below, upload, then comment it again.
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  // Load settings from EEPROM (or defaults)
  loadSettings();

  // Splash screen
  lcd.clear();
  lcd.setCursor(3, 0); lcd.print("SAFE PLANT BY:");
  lcd.setCursor(1, 1); lcd.print("Jobaida  Rehnuma");
  lcd.setCursor(3, 2); lcd.print("Arman  Aditya");
  delay(2000);

  lastInteractionTime = millis();
  drawMainMenu();
}


// ------------------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------------------

void loop() {
  // If pump is running, just wait for it to finish (no other actions)
  if (pumpRunning) {
    if (millis() - pumpStartTime >= (settings.pumpSeconds * 1000UL)) {
      stopPump();
    }
    return;
  }

  char key = kp.getKey();

  // Screen timeout management
  if (key) {
    lastInteractionTime = millis();
    if (!screenOn) {
      lcd.backlight();
      screenOn = true;
      return;               // First key after sleep only wakes screen
    }
  }

  if (screenOn && (millis() - lastInteractionTime > SCREEN_TIMEOUT)) {
    lcd.noBacklight();
    screenOn = false;
  }

  if (!screenOn) return;    // Screen asleep, ignore further input

  // State machine
  switch (currentState) {
    
    case MAIN_MENU:
      if (key == 'A') { currentState = MODE_TIME;  lcd.clear(); }
      else if (key == 'B') { currentState = MODE_MOIST; lcd.clear(); }
      else if (key == 'C') { currentState = SET_HOUR;   inputBuffer = ""; lcd.clear(); }
      else if (key == 'D') { currentState = SET_MOIST;  inputBuffer = ""; lcd.clear(); }
      else if (key == '0') { currentState = SET_PUMP;   inputBuffer = ""; lcd.clear(); }
      else if (key == '*') { currentState = VIEW_BATT;  lcd.clear(); }
      break;

    case MODE_TIME:
      runTimeModeDisplay();
      checkTimeIrrigation();
      if (key == '#') goBack();
      break;

    case MODE_MOIST:
      runMoistModeDisplay();
      checkMoistIrrigation();
      if (key == '#') goBack();
      break;

    case VIEW_BATT:
      runBatteryDisplay();
      if (key == '#') goBack();
      break;

    case SET_MOIST:
      runSetMoistDisplay();
      if (key == '*') {
        if (inputBuffer.length() > 0) {
          int val = inputBuffer.toInt();
          settings.moisture = constrain(val, 0, 100);
          saveSettings();
          confirmSave();
        }
        goBack();
      } else if (key == '#') {
        goBack();
      } else if (key) {
        handleNumberInput(key);
      }
      break;

    case SET_PUMP:
      runSetPumpDisplay();
      if (key == '*') {
        if (inputBuffer.length() > 0) {
          int val = inputBuffer.toInt();
          settings.pumpSeconds = constrain(val, 1, 300); // Safe range
          saveSettings();
          confirmSave();
        }
        goBack();
      } else if (key == '#') {
        goBack();
      } else if (key) {
        handleNumberInput(key);
      }
      break;

    case SET_HOUR:
    case SET_MIN:
      runSetTimeDisplay(currentState == SET_HOUR);
      if (key == '*') {
        if (currentState == SET_HOUR) {
          if (inputBuffer.length() > 0) {
            int val = inputBuffer.toInt();
            settings.hour = constrain(val, 0, 23);
          }
          currentState = SET_MIN;
          inputBuffer = "";
          lcd.clear();
        } else { // SET_MIN
          if (inputBuffer.length() > 0) {
            int val = inputBuffer.toInt();
            settings.minute = constrain(val, 0, 59);
            saveSettings();
            confirmSave();
          }
          goBack();
        }
      } else if (key == '#') {
        goBack();
      } else if (key) {
        handleNumberInput(key);
      }
      break;
  }
}