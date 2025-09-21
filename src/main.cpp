#include <Arduino.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <thermistor.h>

#define FAN_PIN D2
#define CONFIG_BTN_PIN D5
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
#define THERMISTOR_PIN A0

WiFiManager wm;

unsigned int wifiPortalTimeout = 180;
unsigned long wifistartTime = millis();

bool portalRunning = false;
bool startAP = false;

// Function prototype
void doWiFiManager();

Thermistor *thermistor;

void readThermistor();
unsigned long thermistorStartTime = millis();

void setup()
{
  // Setup PIN modes and states as early as possible
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  pinMode(HEATER_1_PIN, OUTPUT);
  digitalWrite(HEATER_1_PIN, LOW);

  pinMode(HEATER_2_PIN, OUTPUT);
  digitalWrite(HEATER_2_PIN, LOW);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(THERMISTOR_PIN, INPUT);

  pinMode(CONFIG_BTN_PIN, INPUT_PULLUP);

  // Setup Thermistors
  thermistor = new Thermistor(THERMISTOR_PIN, 3.3, 3.3, 1023, 10000, 10000, 27.8, 3380, 1, 0);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // Start Serial for debug output
  Serial.begin(115200);
  delay(300);
  Serial.println("Ready");

  wm.setHostname("shedheater2000");
  MDNS.begin("shedheater2000");
  wm.setConfigPortalTimeout(wifiPortalTimeout);
  wm.autoConnect("shedheater2000-Setup");

  for (int i = 0; i < 3; i++)
  {
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
  }
}

void loop()
{
  // Send mDNS update
  MDNS.update();

  // Handle WiFi Manager
  doWiFiManager();

  readThermistor();
}

void readThermistor()
{
  if (millis() - thermistorStartTime > 2000)
  {
    float temperature = thermistor->readTempC();
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" °C");
    thermistorStartTime = millis();
  }
}

void doWiFiManager()
{
  // is auto timeout portal running
  if (portalRunning)
  {
    wm.process();

    // check for timeout
    if ((millis() - wifistartTime) > (wifiPortalTimeout * 1000))
    {
      Serial.println("portal timed out");
      portalRunning = false;
      digitalWrite(LED_BUILTIN, HIGH);
      if (startAP)
      {
        wm.stopConfigPortal();
      }
      else
      {
        wm.stopWebPortal();
      }
    }
  }

  // is configuration portal requested?
  if (digitalRead(CONFIG_BTN_PIN) == LOW && (!portalRunning))
  {
    if (startAP)
    {
      Serial.println("Button Pressed, Starting Config Portal");
      wm.setConfigPortalBlocking(false);
      wm.startConfigPortal();
    }
    else
    {
      Serial.println("Button Pressed, Starting Web Portal");
      wm.startWebPortal();
    }
    portalRunning = true;
    digitalWrite(LED_BUILTIN, LOW);
    wifistartTime = millis();
  }
}
