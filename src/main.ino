/*
Important: This code is only for the DIY PRO / AirGradient ONE PCB Version 9 with the ESP-C3 MCU.

It is a high quality sensor showing PM2.5, CO2, TVOC, NOx, Temperature and Humidity on a small display and LEDbar and can send data over Wifi.

Build Instructions: https://www.airgradient.com/open-airgradient/instructions/

Kits (including a pre-soldered version) are available: https://www.airgradient.com/indoor/

The codes needs the following libraries installed:
“WifiManager by tzapu, tablatronix” tested with version 2.0.11-beta
“U8g2” by oliver tested with version 2.32.15
"Sensirion I2C SGP41" by Sensation Version 0.1.0
"Sensirion Gas Index Algorithm" by Sensation Version 3.2.1
“pms” by Markusz Kakl version 1.1.0
"S8_UART" by Josep Comas Version 1.0.1
"Arduino-SHT" by Johannes Winkelmann Version 1.2.2
"Adafruit NeoPixel" by Adafruit Version 1.11.0

Configuration:
Please set in the code below the configuration parameters.

If you have any questions please visit our forum at https://forum.airgradient.com/

If you are a school or university contact us for a free trial on the AirGradient platform.
https://www.airgradient.com/

CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License

*/

#include <HardwareSerial.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <SensirionI2CSgp41.h>
#include <NOxGasIndexAlgorithm.h>
#include <VOCGasIndexAlgorithm.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <MQTT.h>
#include <cstdint>

#include "PMS.h"
#include "SHTSensor.h"
#include "s8_uart.h"

#define DEBUG true

#define I2C_SDA 7
#define I2C_SCL 6

Adafruit_NeoPixel pixels(11, 10, NEO_GRB + NEO_KHZ800);
SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm voc_algorithm;
NOxGasIndexAlgorithm nox_algorithm;
SHTSensor sht;

PMS pms1(Serial0);

PMS::DATA data1;

S8_UART * sensor_S8;
S8_sensor sensor;

// time in seconds needed for NOx conditioning
uint16_t conditioning_s = 10;

// for peristent saving and loading
int addr = 4;
byte value;

// Display bottom right
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

String APIROOT = "http://airgradient.internal/";

// set to true to switch from Celcius to Fahrenheit
boolean inF = false;

// PM2.5 in US AQI (default ug/m3)
boolean inUSAQI = false;

// Display Position
boolean displayTop = true;

// use RGB LED Bar
boolean useRGBledBar = true;

// set to true if you want to connect to wifi. You have 60 seconds to connect. Then it will go into an offline mode.
boolean connectWIFI = true;

int loopCount = 0;

unsigned long currentMillis = 0;

const int oledInterval = 5000;
unsigned long previousOled = 0;

const int sendToServerInterval = 10000;
unsigned long previoussendToServer = 0;

const int tvocInterval = 1000;
unsigned long previousTVOC = 0;
int TVOC = -1;
int NOX = -1;

const int co2Interval = 5000;
unsigned long previousCo2 = 0;
int Co2 = 0;

const int pmInterval = 5000;
unsigned long previousPm = 0;
int pm25 = -1;
int pm01 = -1;
int pm10 = -1;
//int pm03PCount = -1;

const int tempHumInterval = 5000;
unsigned long previousTempHum = 0;
float temp;
int hum;

int buttonConfig = 0;
int lastState = LOW;
int currentState;
unsigned long pressedTime = 0;
unsigned long releasedTime = 0;

int remainingretries = 10;

MQTTClient mqttclient(256);
WiFiClient wificlient;

void setup() {
  if (DEBUG) {
    Serial.begin(115200);
    // see https://github.com/espressif/arduino-esp32/issues/6983
    Serial.setTxTimeoutMs(0); // <<<====== solves the delay issue
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  pixels.begin();
  pixels.clear();

  Serial1.begin(9600, SERIAL_8N1, 0, 1);
  Serial0.begin(9600);
  u8g2.begin();

  updateOLED2("Warming Up", "Serial Number:", String(getNormalizedMac()));
  sgp41.begin(Wire);
  delay(300);

  sht.init(Wire);
  //sht.setAccuracy(SHTSensor::SHT_ACCURACY_MEDIUM);
  delay(300);

  //init Watchdog
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  sensor_S8 = new S8_UART(Serial1);

  EEPROM.begin(512);
  delay(500);

  // push button
  pinMode(9, INPUT_PULLUP);

  buttonConfig = String(EEPROM.read(addr)).toInt();
  if (buttonConfig > 7) buttonConfig = 0;
  delay(400);
  setConfig();
  Serial.println("buttonConfig: " + String(buttonConfig));

  updateOLED2("Press Button", "for LED test &", "offline mode");
  delay(2000);
  currentState = digitalRead(9);
  if (currentState == LOW) {
    ledTest();
    return;
  }

  updateOLED2("Press Button", "Now for", "Config Menu");
  delay(2000);
  currentState = digitalRead(9);
  if (currentState == LOW) {
    updateOLED2("Entering", "Config Menu", "");
    delay(3000);
    lastState = HIGH;
    setConfig();
    inConf();
  }

  if (connectWIFI) connectToWifi();
  mqttclient.begin(IPAddress(192, 168, 1, 161), 1883, wificlient);
  mqttclient.onMessage(messageReceived);
  connect_mqtt();
  updateOLED2("Warming Up", "Serial Number:", String(getNormalizedMac()));
}

void messageReceived(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  // Note: Do not use the client in the callback to publish, subscribe or
  // unsubscribe as it may cause deadlocks when other things arrive while
  // sending and receiving acknowledgments. Instead, change a global variable,
  // or push to a queue and handle it in the loop after calling `client.loop()`.
}


void connect_mqtt() {
  Serial.print("checking wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.print("\nconnecting to mqtt broker...");
  while (remainingretries && !mqttclient.connect("arduino", "public", "public")) {
    Serial.print(".");
    delay(1000);
    remainingretries--;
  }
  if (!remainingretries) {
    Serial.println("mqtt connection failed");
    return;
  }
  Serial.println("\nmqtt client connected");
  //Serial.println("\nconnected!");
  mqttclient.subscribe("/hello");
  // client.unsubscribe("/hello");
}

void loop() {
  mqttclient.loop();
  if (!mqttclient.connected()) {
    connect_mqtt();
  }
  currentMillis = millis();
  updateTVOC();
  updateOLED();
  updateCo2();
  updatePm();
  updateTempHum();

  sendToServer();
}

void updateTVOC() {
  uint16_t error;
  char errorMessage[256];
  uint16_t defaultRh = 0x8000;
  uint16_t defaultT = 0x6666;
  uint16_t srawVoc = 0;
  uint16_t srawNox = 0;
  uint16_t defaultCompenstaionRh = 0x8000; // in ticks as defined by SGP41
  uint16_t defaultCompenstaionT = 0x6666; // in ticks as defined by SGP41
  uint16_t compensationRh = 0; // in ticks as defined by SGP41
  uint16_t compensationT = 0; // in ticks as defined by SGP41

  delay(1000);

  compensationT = static_cast < uint16_t > ((temp + 45) * 65535 / 175);
  compensationRh = static_cast < uint16_t > (hum * 65535 / 100);

  if (conditioning_s > 0) {
    error = sgp41.executeConditioning(compensationRh, compensationT, srawVoc);
    conditioning_s--;
  } else {
    error = sgp41.measureRawSignals(compensationRh, compensationT, srawVoc,
      srawNox);
  }

  if (currentMillis - previousTVOC >= tvocInterval) {
    previousTVOC += tvocInterval;
    if (error) {
      TVOC = -1;
      NOX = -1;
      Serial.println("Error in measureRawSignals()");
    } else {
      TVOC = voc_algorithm.process(srawVoc);
      NOX = nox_algorithm.process(srawNox);
      Serial.println(String(TVOC));
    }

  }
}

void updateCo2() {
  if (currentMillis - previousCo2 >= co2Interval) {
    previousCo2 += co2Interval;
    Co2 = sensor_S8 -> get_co2();
    Serial.println(String(Co2));
  }
}

void updatePm() {
  if (currentMillis - previousPm >= pmInterval) {
    previousPm += pmInterval;
    if (pms1.readUntil(data1, 2000)) {
      pm01 = data1.PM_AE_UG_1_0;
      pm25 = data1.PM_AE_UG_2_5;
      pm10 = data1.PM_AE_UG_10_0;
//      pm03PCount = data1.PM_RAW_0_3;
    } else {
      pm01 = -1;
      pm25 = -1;
      pm10 = -1;
//      pm03PCount = -1;
    }
  }
}

void updateTempHum() {
  if (currentMillis - previousTempHum >= tempHumInterval) {
    previousTempHum += tempHumInterval;

    if (sht.readSample()) {
      temp = sht.getTemperature();
      hum = sht.getHumidity();
    } else {
      Serial.print("Error in readSample()\n");
      temp = -10001;
      hum = -10001;
    }
  }
}

void updateOLED() {
  if (currentMillis - previousOled >= oledInterval) {
    previousOled += oledInterval;

    String ln3;
    String ln1;

    if (inUSAQI) {
      ln1 = "AQI:" + String(PM_TO_AQI_US(pm25)) + " CO2:" + String(Co2);
    } else {
      ln1 = "PM:" + String(pm25) + " CO2:" + String(Co2);
    }

    String ln2 = "TVOC:" + String(TVOC) + " NOX:" + String(NOX);

    if (inF) {
      ln3 = "F:" + String((temp * 9 / 5) + 32) + " H:" + String(hum) + "%";
    } else {
      ln3 = "C:" + String(temp) + " H:" + String(hum) + "%";
    }
    //updateOLED2(ln1, ln2, ln3);
    updateOLED3();
    setRGBledCO2color(Co2);
  }
}

void inConf() {
  setConfig();
  currentState = digitalRead(9);

  if (currentState) {
    Serial.println("currentState: high");
  } else {
    Serial.println("currentState: low");
  }

  if (lastState == HIGH && currentState == LOW) {
    pressedTime = millis();
  } else if (lastState == LOW && currentState == HIGH) {
    releasedTime = millis();
    long pressDuration = releasedTime - pressedTime;
    if (pressDuration < 1000) {
      buttonConfig = buttonConfig + 1;
      if (buttonConfig > 7) buttonConfig = 0;
    }
  }

  if (lastState == LOW && currentState == LOW) {
    long passedDuration = millis() - pressedTime;
    if (passedDuration > 4000) {
      updateOLED2("Saved", "Release", "Button Now");
      delay(1000);
      updateOLED2("Rebooting", "in", "5 seconds");
      delay(5000);
      EEPROM.write(addr, char(buttonConfig));
      EEPROM.commit();
      delay(1000);
      ESP.restart();
    }

  }
  lastState = currentState;
  delay(100);
  inConf();
}

void setConfig() {
  Serial.println("in setConfig");
  if (buttonConfig == 0) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:C, PM:ug/m3", "LED Bar: on", "Long Press Saves");
    inF = false;
    inUSAQI = false;
    useRGBledBar = true;
  } else if (buttonConfig == 1) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:C, PM:US AQI", "LED Bar: on", "Long Press Saves");
    inF = false;
    inUSAQI = true;
    useRGBledBar = true;
  } else if (buttonConfig == 2) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:F PM:ug/m3", "LED Bar: on", "Long Press Saves");
    inF = true;
    inUSAQI = false;
    useRGBledBar = true;
  } else if (buttonConfig == 3) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:F PM:US AQI", "LED Bar: on", "Long Press Saves");
    inF = true;
    inUSAQI = true;
    useRGBledBar = true;
  } else  if (buttonConfig == 4) {
    updateOLED2("T:C, PM:ug/m3", "LED Bar: off", "Long Press Saves");
    inF = false;
    inUSAQI = false;
    useRGBledBar = false;
  } else if (buttonConfig == 5) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:C, PM:US AQI", "LED Bar: off", "Long Press Saves");
    inF = false;
    inUSAQI = true;
    useRGBledBar = false;
  } else if (buttonConfig == 6) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:F PM:ug/m3", "LED Bar: off", "Long Press Saves");
    inF = true;
    inUSAQI = false;
    useRGBledBar = false;
  } else if (buttonConfig == 7) {
    u8g2.setDisplayRotation(U8G2_R0);
    updateOLED2("T:F PM:US AQI", "LED Bar: off", "Long Press Saves");
    inF = true;
    inUSAQI = true;
    useRGBledBar = false;
  }
}

void sendPing() {
  String payload = "{\"wifi\":" + String(WiFi.RSSI()) +
    ", \"boot\":" + loopCount +
    "}";
}

void updateOLED2(String ln1, String ln2, String ln3) {
  char buf[9];
  u8g2.firstPage();
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_t0_16_tf);
    u8g2.drawStr(1, 10, String(ln1).c_str());
    u8g2.drawStr(1, 30, String(ln2).c_str());
    u8g2.drawStr(1, 50, String(ln3).c_str());
  } while (u8g2.nextPage());
}

void updateOLED3() {
  char buf[9];
  u8g2.firstPage();
  u8g2.firstPage();
  do {

    u8g2.setFont(u8g2_font_t0_16_tf);

    if (inF) {
      if (temp > -10001) {
        float tempF = (temp * 9 / 5) + 32;
        sprintf(buf, "%.1f°F", tempF);
      } else {
        sprintf(buf, "-°F");
      }
      u8g2.drawUTF8(1, 10, buf);
    } else {
      if (temp > -10001) {
        sprintf(buf, "%.1f°C", temp);
      } else {
        sprintf(buf, "-°C");
      }
      u8g2.drawUTF8(1, 10, buf);
    }

    if (hum >= 0) {
      sprintf(buf, "%d%%", hum);
    } else {
      sprintf(buf, " -%%");
    }
    if (hum > 99) {
      u8g2.drawStr(97, 10, buf);
    } else {
      u8g2.drawStr(105, 10, buf);
      // there might also be single digits, not considered, sprintf might actually support a leading space
    }

    u8g2.drawLine(1, 13, 128, 13);
    u8g2.setFont(u8g2_font_t0_12_tf);
    u8g2.drawUTF8(1, 27, "CO2");
    u8g2.setFont(u8g2_font_t0_22b_tf);
    if (Co2 > 0) {
      sprintf(buf, "%d", Co2);
    } else {
      sprintf(buf, "%s", "-");
    }
    u8g2.drawStr(1, 48, buf);
    u8g2.setFont(u8g2_font_t0_12_tf);
    u8g2.drawStr(1, 61, "ppm");
    u8g2.drawLine(45, 15, 45, 64);
    u8g2.setFont(u8g2_font_t0_12_tf);
    u8g2.drawStr(48, 27, "PM2.5");
    u8g2.setFont(u8g2_font_t0_22b_tf);

    if (inUSAQI) {
      if (pm25 >= 0) {
        sprintf(buf, "%d", PM_TO_AQI_US(pm25));
      } else {
        sprintf(buf, "%s", "-");
      }
      u8g2.drawStr(48, 48, buf);
      u8g2.setFont(u8g2_font_t0_12_tf);
      u8g2.drawUTF8(48, 61, "AQI");
    } else {
      if (pm25 >= 0) {
        sprintf(buf, "%d", pm25);
      } else {
        sprintf(buf, "%s", "-");
      }
      u8g2.drawStr(48, 48, buf);
      u8g2.setFont(u8g2_font_t0_12_tf);
      u8g2.drawUTF8(48, 61, "ug/m³");
    }

    u8g2.drawLine(82, 15, 82, 64);
    u8g2.setFont(u8g2_font_t0_12_tf);
    u8g2.drawStr(85, 27, "TVOC:");
    if (TVOC >= 0) {
      sprintf(buf, "%d", TVOC);
    } else {
      sprintf(buf, "%s", "-");
    }
    u8g2.drawStr(85, 39, buf);
    u8g2.drawStr(85, 53, "NOx:");
    if (NOX >= 0) {
      sprintf(buf, "%d", NOX);
    } else {
      sprintf(buf, "%s", "-");
    }
    u8g2.drawStr(85, 63, buf);

  } while (u8g2.nextPage());
}

typedef struct {
  int32_t wifi; // wifi signal strength
  int32_t rco2; // CO2
  int32_t pm01; // PM1.0
  int32_t pm25; // PM2.5
  int32_t pm10; // PM10
  int32_t pm03PCount; // PM0.3
  int32_t tvoc_index; // TVOC
  int32_t nox_index; // NOx
  float atmp; // temperature
  int32_t rhum; // humidity
  int32_t boot; // loopCount
} payload;

char payloadbuffer[sizeof(payload)];

void sendToServer() {
  if (currentMillis - previoussendToServer >= sendToServerInterval) {
    previoussendToServer += sendToServerInterval;

    payload p = {
      WiFi.RSSI(),
      Co2,
      pm01,
      pm25,
      pm10,
      -1,
      TVOC,
      NOX,
      temp,
      hum,
      loopCount
    };

    Serial.println("Loopcount: " + String(loopCount) + "\n");
    memcpy(payloadbuffer, &p, sizeof(payload));

    if (WiFi.status() == WL_CONNECTED) {
      // send payload as-is
      bool res = mqttclient.publish("/office", payloadbuffer, sizeof(payload));
      if (!res) {
        Serial.println("mqtt publish failed");
      } else {
        Serial.println("published to mqtt");
      }
      resetWatchdog();
      loopCount++;
    } else {
      Serial.println("WiFi Disconnected");
    }
  }
}

void countdown(int from) {
  debug("\n");
  while (from > 0) {
    debug(String(from--));
    debug(" ");
    delay(1000);
  }
  debug("\n");
}

void resetWatchdog() {
  Serial.println("Watchdog reset");
  digitalWrite(2, HIGH);
  delay(20);
  digitalWrite(2, LOW);
}

// Wifi Manager
void connectToWifi() {
  WiFiManager wifiManager;
  //WiFi.disconnect(); //to delete previous saved hotspot
  String HOTSPOT = "AG-" + String(getNormalizedMac());
  updateOLED2("180s to connect", "to Wifi Hotspot", HOTSPOT);
  wifiManager.setTimeout(180);
  if (!wifiManager.autoConnect((const char * ) HOTSPOT.c_str())) {
    Serial.println("failed to connect and hit timeout");
    delay(6000);
  }
}

void debug(String msg) {
  if (DEBUG)
    Serial.print(msg);
}

void debug(int msg) {
  if (DEBUG)
    Serial.print(msg);
}

void debugln(String msg) {
  if (DEBUG)
    Serial.println(msg);
}

void debugln(int msg) {
  if (DEBUG)
    Serial.println(msg);
}

String getNormalizedMac() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac;
}

void setRGBledCO2color(int co2Value) { 
  if (co2Value <= 0) return; 
  co2Value = min(co2Value, 3000); // don't calculate hue beyond 3000 to not return green again 
  uint16_t hue = map(co2Value, 500, 1800, UINT16_MAX/3 /*green*/, 0 /*red*/); 
  uint32_t rgbcolor = pixels.ColorHSV(hue); 
  pixels.fill(rgbcolor); 
  pixels.show(); 
}

void setRGBledColor(char color) {
  if (useRGBledBar) {
    //pixels.clear();
    switch (color) {
    case 'g':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
        delay(30);
        pixels.show();
      }
      break;
    case 'y':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 255, 0));
        delay(30);
        pixels.show();
      }
      break;
    case 'o':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 128, 0));
        delay(30);
        pixels.show();
      }
      break;
    case 'r':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
        delay(30);
        pixels.show();
      }
      break;
    case 'b':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 255));
        delay(30);
        pixels.show();
      }
      break;
    case 'w':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 255, 255));
        delay(30);
        pixels.show();
      }
      break;
    case 'p':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(153, 0, 153));
        delay(30);
        pixels.show();
      }
      break;
    case 'z':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(102, 0, 0));
        delay(30);
        pixels.show();
      }
      break;
    case 'n':
      for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 0));
        delay(30);
        pixels.show();
      }
      break;
    default:
      // if nothing else matches, do the default
      // default is optional
      break;
    }
  }
}

void ledTest() {
  updateOLED2("LED Test", "running", ".....");
  for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
        delay(30);
        pixels.show();
      }
  delay(500);
  for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
        delay(30);
        pixels.show();
      }
  delay(500);
  for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 255));
        delay(30);
        pixels.show();
      }
  delay(500);
  for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 255, 255));
        delay(30);
        pixels.show();
      }
  delay(500);
  for (int i = 0; i < 11; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 0));
        delay(30);
        pixels.show();
      }
  delay(500);
}

// Calculate PM2.5 US AQI
int PM_TO_AQI_US(int pm02) {
  if (pm02 <= 12.0) return ((50 - 0) / (12.0 - .0) * (pm02 - .0) + 0);
  else if (pm02 <= 35.4) return ((100 - 50) / (35.4 - 12.0) * (pm02 - 12.0) + 50);
  else if (pm02 <= 55.4) return ((150 - 100) / (55.4 - 35.4) * (pm02 - 35.4) + 100);
  else if (pm02 <= 150.4) return ((200 - 150) / (150.4 - 55.4) * (pm02 - 55.4) + 150);
  else if (pm02 <= 250.4) return ((300 - 200) / (250.4 - 150.4) * (pm02 - 150.4) + 200);
  else if (pm02 <= 350.4) return ((400 - 300) / (350.4 - 250.4) * (pm02 - 250.4) + 300);
  else if (pm02 <= 500.4) return ((500 - 400) / (500.4 - 350.4) * (pm02 - 350.4) + 400);
  else return 500;
};
