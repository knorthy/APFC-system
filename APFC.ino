// 1. PLACE YOUR UNIQUE BLYNK INFO AT THE VERY TOP
#define BLYNK_TEMPLATE_ID ""
#define BLYNK_TEMPLATE_NAME ""
#define BLYNK_AUTH_TOKEN ""

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <PZEM004Tv30.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>  

// --- Network & Auth ---
char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = ""; 
char pass[] = "";

// --- Pin Mapping ---
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define RELAY_1 18
#define RELAY_2 19
#define RELAY_3 25
#define RELAY_4 26

// --- Objects ---
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
LiquidCrystal_I2C lcd(0x27, 20, 4); 
BlynkTimer timer; 

// --- Variables ---
float voltage = 0, current = 0, pf = 1.0, energy = 0;
int activeSteps = 0;
String systemStatus = "STARTING...";
bool isManual = false;
int manR1, manR2, manR3, manR4;

unsigned long lastSwitchTime = 0;
const unsigned long switchDelay = 3000;

// --- Blynk Listeners ---
BLYNK_WRITE(V6) { isManual = param.asInt(); } 
BLYNK_WRITE(V7) { manR1 = param.asInt(); }
BLYNK_WRITE(V8) { manR2 = param.asInt(); }
BLYNK_WRITE(V9) { manR3 = param.asInt(); }
BLYNK_WRITE(V10) { manR4 = param.asInt(); }

// --- Core Functions ---

void updateData() {
  // 1. Read raw data from the PZEM sensor
  float v = pzem.voltage();
  float i = pzem.current();
  float p = pzem.pf();
  float e = pzem.energy();

  // 2. Validate data and update global variables
  if (!isnan(v)) voltage = v;
  if (!isnan(i)) current = i;
  if (!isnan(p)) pf = p;
  if (!isnan(e)) energy = e;

  // 3. CALCULATE REACTIVE POWER (VAR)
  // This math helps explain why the PF "slides" or changes on its own
  float apparentPower = voltage * current;
  float activePower = apparentPower * pf;
  
  float reactivePower = 0;
  if (apparentPower > activePower) {
    // Math: Q = sqrt(S^2 - P^2)
    reactivePower = sqrt(pow(apparentPower, 2) - pow(activePower, 2));
  }

  // 4. DEBUG LOGGING
  // This will show up in your Serial Monitor so you can watch the motor's "lag"
  Serial.print("V: "); Serial.print(voltage);
  Serial.print("V | I: "); Serial.print(current);
  Serial.print("A | PF: "); Serial.print(pf);
  Serial.print(" | VAR (Reactive): "); Serial.println(reactivePower);

  // 5. UPDATE OTHER SYSTEMS
  processCorrection();
  updateDisplays();
}

// --- NEW IMPROVED LOGIC ---
void processCorrection() {
  unsigned long currentTime = millis();

  // 1. MANUAL MODE BYPASS
  if (isManual) {

    Serial.print("MANUAL ACTIVE - Relays are: ");
    Serial.print(manR1); Serial.print(" ");
    Serial.print(manR2); Serial.print(" ");
    Serial.print(manR3); Serial.print(" ");
    Serial.println(manR4);

    systemStatus = "MANUAL MODE";
    digitalWrite(RELAY_1, manR1 ? LOW : HIGH);
    digitalWrite(RELAY_2, manR2 ? LOW : HIGH);
    digitalWrite(RELAY_3, manR3 ? LOW : HIGH);
    digitalWrite(RELAY_4, manR4 ? LOW : HIGH);
    activeSteps = manR1 + manR2 + manR3 + manR4;
    return; 
  }

  // 2. MINIMAL LOAD PROTECTION (Lowered from 0.20 to 0.05)
  // PZEM sensors struggle below 0.05A. If we try to correct 
  // at 0.01A, the PF reading is usually garbage.
  if (current < 0.05) { 
    if (activeSteps != 0) {
        activeSteps = 0;
        updateRelays();
    }
    systemStatus = "NO LOAD DETECTED";
    return;
  }

  // 3. TARGET SETTINGS
  float targetLimit = 0.97; 

  // 4. AUTOMATIC STEPPING LOGIC
  if (currentTime - lastSwitchTime > switchDelay) {
    
    // If PF is low, ADD a capacitor
    if (pf < targetLimit && activeSteps < 4) {
      activeSteps++;
      lastSwitchTime = currentTime;
      systemStatus = "ADDING CAP...";
    } 
    // If PF is 1.0 or leading (which some sensors show as decreasing), 
    // or if current is high and PF is perfect, we stay put.
    // Only remove if we are SURE we are over-correcting.
    else if (pf > 0.99 && activeSteps > 0) { 
      // Note: In some setups, adding too much cap makes PF 
      // go from 0.8 (lagging) -> 1.0 -> 0.8 (leading).
      activeSteps--;
      lastSwitchTime = currentTime;
      systemStatus = "REMOVING (OVER)";
    }
    else {
      systemStatus = (pf >= targetLimit) ? "OPTIMIZED" : "MAX STEPS";
    }
    
    updateRelays();
  }
}

// --- HELPER TO SET RELAYS ---
void updateRelays() {
  digitalWrite(RELAY_1, (activeSteps >= 1) ? LOW : HIGH);
  digitalWrite(RELAY_2, (activeSteps >= 2) ? LOW : HIGH);
  digitalWrite(RELAY_3, (activeSteps >= 3) ? LOW : HIGH);
  digitalWrite(RELAY_4, (activeSteps >= 4) ? LOW : HIGH);
}

void updateDisplays() {
  lcd.init(); 
  lcd.backlight(); 

  float displayPF = pf; 

  lcd.setCursor(0, 0);
  lcd.print("Energy: " + String(energy, 3) + " kWh"); 
  lcd.setCursor(0, 1);
  lcd.print("PF: " + String(displayPF, 2) + "        ");
  lcd.setCursor(0, 2);
  lcd.print("Steps: " + String(activeSteps) + " / 4   ");
  lcd.setCursor(0, 3);
  lcd.print("Stat: " + systemStatus + "    ");

  Blynk.virtualWrite(V0, voltage);
  Blynk.virtualWrite(V1, current);
  Blynk.virtualWrite(V2, displayPF); 
  Blynk.virtualWrite(V3, activeSteps);
  Blynk.virtualWrite(V4, energy);
  Blynk.virtualWrite(V5, systemStatus);
  Blynk.virtualWrite(V11, energy);
}

void setup() {
  Serial.begin(115200);
  
  // RELAYS INITIAL STATE
  digitalWrite(RELAY_1, HIGH); 
  digitalWrite(RELAY_2, HIGH);
  digitalWrite(RELAY_3, HIGH);
  digitalWrite(RELAY_4, HIGH);

  pinMode(RELAY_1, OUTPUT); 
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT); 
  pinMode(RELAY_4, OUTPUT);

  Wire.begin(21, 22); 
  lcd.init();
  lcd.backlight();

  Blynk.begin(auth, ssid, pass);
  Blynk.syncVirtual(V6, V7, V8, V9, V10);

  timer.setInterval(1000L, updateData);
}

BLYNK_WRITE(V12) {
  if (param.asInt() == 1) {
    pzem.resetEnergy();
    Blynk.virtualWrite(V11, 0);
  }
}

void loop() {
  Blynk.run();
  timer.run();
}
