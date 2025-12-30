#include "LoRaWan_APP.h"
#include "Arduino.h"
#include "HT_SSD1306Wire.h"
#include <Wire.h>
#include <Adafruit_BMP085.h>

// ===== LoRaWAN Settings =====
bool isTxConfirmed = true;
uint8_t confirmedNbTrials = 4;
bool overTheAirActivation = true;
DeviceClass_t loraWanClass = CLASS_A;
bool loraWanAdr = true;

uint8_t devEui[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t appEui[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t appKey[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t nwkSKey[16] = {0};
uint8_t appSKey[16] = {0};
uint32_t devAddr = 0;
uint16_t userChannelsMask[6] = {0x00FF,0,0,0,0,0};

LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;
uint32_t appTxDutyCycle = 30000;
uint8_t appPort = 2;

// ===== Pins =====
const int GAS_PIN  = 4;
const int SOIL_PIN = 5;
const int BUZZER_PIN = 36;   // keep same pin

// ===== Calibrations =====
int SOIL_AIR_ADC=4095, SOIL_WATER_ADC=1500;
int MQ2_AIR_ADC=1500, MQ2_GAS_ADC=3000;

// ===== OLED =====
#define SDA_OLED 17
#define SCL_OLED 18
#define RST_OLED 21
SSD1306Wire myDisplay(0x3c,500000,SDA_OLED,SCL_OLED,GEOMETRY_128_64,RST_OLED);

// ===== I2C sensors =====
#define SDA_SENSORS 38
#define SCL_SENSORS 39
TwoWire I2C_SENSORS = TwoWire(1);
Adafruit_BMP085 bmp;

// ===== Globals =====
bool buzzerState=false;
bool joined=false;
float seaLevel_hPa = 1013.25;
uint8_t BH1750_ADDR=0x23;
bool bh1750_ok=false;

// ===== BH1750 helper =====
bool bh1750_setMode(uint8_t mode){
  I2C_SENSORS.beginTransmission(BH1750_ADDR);
  I2C_SENSORS.write(mode);
  return I2C_SENSORS.endTransmission()==0;
}
bool bh1750_begin_auto(){
  BH1750_ADDR=0x23;
  if(bh1750_setMode(0x01)&&bh1750_setMode(0x10)){delay(180);return true;}
  BH1750_ADDR=0x5C;
  if(bh1750_setMode(0x01)&&bh1750_setMode(0x10)){delay(180);return true;}
  return false;
}
float bh1750_read_lux(){
  I2C_SENSORS.requestFrom((int)BH1750_ADDR,2);
  if(I2C_SENSORS.available()<2)return -1;
  uint16_t hi=I2C_SENSORS.read(),lo=I2C_SENSORS.read();
  return ((hi<<8)|lo)/1.2f;
}

// ===== Helper functions =====
void oledMsg(const String &l1,const String &l2="",const String &l3="",const String &l4=""){
  myDisplay.clear();
  myDisplay.setTextAlignment(TEXT_ALIGN_LEFT);
  myDisplay.setFont(ArialMT_Plain_10);
  myDisplay.drawString(0,0,l1);
  if(l2.length())myDisplay.drawString(0,16,l2);
  if(l3.length())myDisplay.drawString(0,32,l3);
  if(l4.length())myDisplay.drawString(0,48,l4);
  myDisplay.display();
}
int readADC(int pin,int samples=16){
  uint32_t acc=0; for(int i=0;i<samples;i++){acc+=analogRead(pin);delay(2);} return acc/samples;
}
int soilPercentFromRaw(int raw){return constrain(map(raw,SOIL_AIR_ADC,SOIL_WATER_ADC,0,100),0,100);}
int gasPercentFromRaw(int raw){return constrain(map(raw,MQ2_AIR_ADC,MQ2_GAS_ADC,0,100),0,100);}

void calibrateSeaLevel(float knownAlt){
  float p=0; for(int i=0;i<10;i++){p+=bmp.readPressure()/100.0f;delay(50);} p/=10.0f;
  seaLevel_hPa=p/pow(1.0-(knownAlt/44330.0),5.255);
}

// ===== Uplink frame =====
static void prepareTxFrame(uint8_t port){
  int soilRaw=readADC(SOIL_PIN);
  int gasRaw =readADC(GAS_PIN);
  int soilPct=soilPercentFromRaw(soilRaw);
  int gasPct =gasPercentFromRaw(gasRaw);

  // Auto buzzer rule
  if(gasPct>50) buzzerState=true;
  else buzzerState=false;
  digitalWrite(BUZZER_PIN,buzzerState?HIGH:LOW);

  float t=bmp.readTemperature();
  float p=bmp.readPressure()/100.0f;
  float alt=44330.0*(1.0-pow(p/seaLevel_hPa,0.1903));
  float lux=bh1750_ok?bh1750_read_lux():0;
  if(lux<0)lux=0;

  uint16_t t10=(uint16_t)roundf(t*10);
  uint16_t lx=(uint16_t)min(65535.0f,roundf(lux));
  uint16_t p10=(uint16_t)roundf(p*10);
  uint16_t a10=(uint16_t)roundf(alt*10);

  appData[0]=soilPct;
  appData[1]=gasPct;
  appData[2]=(t10>>8)&0xFF; appData[3]=t10&0xFF;
  appData[4]=(lx>>8)&0xFF;  appData[5]=lx&0xFF;
  appData[6]=(p10>>8)&0xFF; appData[7]=p10&0xFF;
  appData[8]=(a10>>8)&0xFF; appData[9]=a10&0xFF;
  appData[10]=buzzerState?1:0;
  appDataSize=11;

  oledMsg("Soil "+String(soilPct)+"% Gas "+String(gasPct)+"%",
          "T "+String(t,1)+"C Lux "+String(lux,0),
          "P "+String(p,1)+"hPa Alt "+String(alt,1),
          "Buzzer "+String(buzzerState?"ON":"OFF"));
  Serial.printf("Soil=%d Gas=%d T=%.1f Lx=%.0f P=%.1f Alt=%.1f Buzzer=%s\n",
                soilPct,gasPct,t,lux,p,alt,buzzerState?"ON":"OFF");
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);delay(100);
  Serial.println("Heltec LoRa node start");

  pinMode(Vext,OUTPUT);digitalWrite(Vext,LOW);delay(100);
  Wire.begin(SDA_OLED,SCL_OLED);myDisplay.init();
  I2C_SENSORS.begin(SDA_SENSORS,SCL_SENSORS);
  pinMode(SOIL_PIN,INPUT); pinMode(GAS_PIN,INPUT);
  analogSetPinAttenuation(SOIL_PIN,ADC_11db);
  analogSetPinAttenuation(GAS_PIN,ADC_11db);
  pinMode(BUZZER_PIN,OUTPUT); digitalWrite(BUZZER_PIN,LOW);

  oledMsg("Booting...","Init sensors");
  if(bmp.begin(BMP085_ULTRAHIGHRES,&I2C_SENSORS)) calibrateSeaLevel(933.0);
  bh1750_ok=bh1750_begin_auto();

  Mcu.begin(HELTEC_BOARD,SLOW_CLK_TPYE);
  LoRaWAN.init(loraWanClass,loraWanRegion);
  LoRaWAN.setDefaultDR(3);
  oledMsg("Init OK","Joining TTN...");
  Serial.println("Joining TTN...");
  deviceState=DEVICE_STATE_JOIN;
}

// ===== Loop =====
void loop(){
  switch(deviceState){
    case DEVICE_STATE_JOIN:
      oledMsg("Joining TTN...");
      LoRaWAN.join();
      break;
    case DEVICE_STATE_SEND:
      if(!joined){oledMsg("Joined TTN!"); joined=true;}
      prepareTxFrame(appPort);
      LoRaWAN.send();
      deviceState=DEVICE_STATE_CYCLE;
      break;
    case DEVICE_STATE_CYCLE:
      txDutyCycleTime=appTxDutyCycle+randr(-APP_TX_DUTYCYCLE_RND,APP_TX_DUTYCYCLE_RND);
      LoRaWAN.cycle(txDutyCycleTime);
      deviceState=DEVICE_STATE_SLEEP;
      break;
    case DEVICE_STATE_SLEEP:
      LoRaWAN.sleep(loraWanClass);
      break;
    default: deviceState=DEVICE_STATE_INIT; break;
  }
}

// ===== Downlink =====
void downLinkDataHandle(McpsIndication_t *m){
  if(m->BufferSize>0){
    uint8_t cmd=m->Buffer[0];
    if(cmd==0x00)buzzerState=false;
    else if(cmd==0x01)buzzerState=true;
    else if(cmd==0x02)buzzerState=!buzzerState;

    digitalWrite(BUZZER_PIN,buzzerState?HIGH:LOW);
    oledMsg("Downlink RX","Buzzer "+String(buzzerState?"ON":"OFF"));
    Serial.printf("Downlink buzzer: %s\n",buzzerState?"ON":"OFF");
  }
}