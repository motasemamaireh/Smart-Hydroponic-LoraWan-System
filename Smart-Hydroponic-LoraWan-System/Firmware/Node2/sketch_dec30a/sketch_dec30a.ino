#include "LoRaWan_APP.h"
#include "HT_SSD1306Wire.h"
#include <Wire.h>

// ---------- LoRaWAN Config ----------
bool isTxConfirmed = false;
uint8_t confirmedNbTrials = 4;
bool overTheAirActivation = true;
DeviceClass_t loraWanClass = CLASS_A;
bool loraWanAdr = false;

// ---------- TTN Credentials (MSB, yours) ----------
uint8_t devEui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint8_t appEui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint8_t appKey[] = { 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t nwkSKey[16], appSKey[16];
uint32_t devAddr = 0x00000000;

uint16_t userChannelsMask[6] = { 0x0007, 0, 0, 0, 0, 0 };
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;

uint32_t appTxDutyCycle = 6000;
uint8_t appPort = 1;
static uint32_t counter = 0;
unsigned long lastSendTime = 0;

// ---------- TX Duty Cycle per DR (ms) ----------
const uint32_t sfDutyCycle[] = {
  300000, // DR0 = SF12
  148000, // DR1 = SF11
  74000,  // DR2 = SF10
  40000,  // DR3 = SF9
  22000,  // DR4 = SF8
  12000   // DR5 = SF7
};

// ---------- OLED ----------
SSD1306Wire oled(
  0x3c,
  500000,
  SDA_OLED,
  SCL_OLED,
  GEOMETRY_128_64,
  RST_OLED
);

// ---------- PRG Button ----------
#define PRG_BUTTON 0
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 300;
bool buttonPressed = false;

// ---------- SF / DR management ----------
uint8_t currentSF = 5; // DR5 = SF7
const uint8_t sfRange[] = { 0, 1, 2, 3, 4, 5 };
const int sfCount = sizeof(sfRange) / sizeof(sfRange[0]);

bool displaySF = false;
unsigned long sfDisplayStart = 0;
const unsigned long sfDisplayDuration = 2000;

// ---------- Built-in LED ----------
const int OnboardLED = 35;

// ---------- Countdown timer ----------
unsigned long lastDisplayUpdate = 0;
const unsigned long displayUpdateInterval = 1000;

// ---------- TDS sensor ----------
#define TDS_PIN 4
float adcVref = 3.30f;
float tdsFactor = 0.64f;
float tdsCal = 1.00f;

uint16_t lastTdsPpm = 0;
float lastVolts = 0.0f;

int16_t lastRssi = 32767;
int8_t lastSnr = 127;

// ---------- Helpers ----------
void showDisplay(String line1, String line2="", String line3="", String line4="") {
  oled.clear();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, line1);
  if (line2.length()) oled.drawString(0, 15, line2);
  if (line3.length()) oled.drawString(0, 30, line3);
  if (line4.length()) oled.drawString(0, 45, line4);
  oled.display();
}

int readADCavg(int pin, int samples = 32) {
  uint32_t acc = 0;
  for (int i = 0; i < samples; i++) {
    acc += analogRead(pin);
    delay(2);
  }
  return acc / samples;
}

void cycleSpreadingFactor() {
  int currentIndex = 0;
  for (int i = 0; i < sfCount; i++) {
    if (sfRange[i] == currentSF) {
      currentIndex = i;
      break;
    }
  }

  currentIndex = (currentIndex + 1) % sfCount;
  currentSF = sfRange[currentIndex];

  LoRaWAN.setDefaultDR(currentSF);
  appTxDutyCycle = sfDutyCycle[currentSF];

  oled.clear();
  oled.setFont(ArialMT_Plain_24);
  oled.drawString(35, 20, "SF" + String(12 - currentSF));
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(8, 50, "DR " + String(currentSF) + "  " + String(appTxDutyCycle / 1000) + "s");
  oled.display();

  displaySF = true;
  sfDisplayStart = millis();
}

static void prepareTxFrame(uint8_t port) {
  counter++;

  int raw = readADCavg(TDS_PIN, 32);
  float volts = (raw / 4095.0f) * adcVref;
  float tds = (volts * 1000.0f * tdsFactor) * tdsCal;
  if (tds < 0) tds = 0;

  lastTdsPpm = (uint16_t)(tds + 0.5f);
  lastVolts = volts;

  String msg = "TDS=" + String(lastTdsPpm);
  appDataSize = msg.length();
  memcpy(appData, msg.c_str(), appDataSize);
}

void updateCountdownDisplay() {
  if (displaySF) return;

  unsigned long timeElapsed = millis() - lastSendTime;
  unsigned long timeRemaining =
    (timeElapsed < appTxDutyCycle) ? (appTxDutyCycle - timeElapsed) / 1000 : 0;

  String rf = "RSSI/SNR: ";
  if (lastRssi == 32767 || lastSnr == 127) rf += "--/--";
  else rf += String(lastRssi) + "/" + String((int)lastSnr);

  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "TDS: " + String(lastTdsPpm) + " ppm");
  oled.drawString(0, 15, "V: " + String(lastVolts, 3));
  oled.drawString(0, 30, rf);
  oled.drawString(0, 45, "Next TX: " + String(timeRemaining) + "s");
  oled.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(OnboardLED, OUTPUT);
  pinMode(PRG_BUTTON, INPUT_PULLUP);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(50);

  oled.init();
  oled.flipScreenVertically();
  showDisplay("LoRa Transmitter", "Initializing...");

  pinMode(TDS_PIN, INPUT);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  LoRaWAN.init(loraWanClass, loraWanRegion);
  LoRaWAN.setDefaultDR(currentSF);

  appTxDutyCycle = sfDutyCycle[currentSF];
  showDisplay("Joining TTN...");
  LoRaWAN.join();
}

void loop() {
  bool pressed = (digitalRead(PRG_BUTTON) == LOW);

  if (pressed && !buttonPressed && millis() - lastButtonPress > debounceDelay) {
    cycleSpreadingFactor();
    lastButtonPress = millis();
    buttonPressed = true;
  } else if (!pressed) {
    buttonPressed = false;
  }

  if (displaySF && millis() - sfDisplayStart >= sfDisplayDuration) {
    displaySF = false;
    updateCountdownDisplay();
  }

  if (!displaySF && millis() - lastDisplayUpdate >= displayUpdateInterval) {
    updateCountdownDisplay();
    lastDisplayUpdate = millis();
  }

  if (millis() - lastSendTime >= appTxDutyCycle) {
    prepareTxFrame(appPort);
    LoRaWAN.send();
    lastSendTime = millis();
  }

  LoRaWAN.sleep(loraWanClass);
}

void onJoinSucceeded() {
  showDisplay("Joined TTN!");
  lastSendTime = millis();
}

void onJoinFailed() {
  showDisplay("Join Failed!");
}

void onTxDone() {
  updateCountdownDisplay();
}

void onTxConfirmed() {}
void onTxTimeout() {}

void downLinkDataHandle(McpsIndication_t *m) {
  lastRssi = m->Rssi;
  lastSnr = m->Snr;
}