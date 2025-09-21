#include <Arduino.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>

#define FAN_PIN D2
#define CONFIG_BTN_PIN D5
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
#define THERMISTOR_PIN A0

WiFiManager wm;

unsigned int wifiPortalTimeout = 180;
unsigned int startTime = millis();

bool portalRunning = false;
bool startAP = false;

// Function prototype
void doWiFiManager();

void setup()
{
  // Setup PIN modes and states as early as possible
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  pinMode(HEATER_1_PIN, OUTPUT);
  digitalWrite(HEATER_1_PIN, LOW);

  pinMode(HEATER_2_PIN, OUTPUT);
  digitalWrite(HEATER_2_PIN, LOW);

  pinMode(THERMISTOR_PIN, INPUT);

  pinMode(CONFIG_BTN_PIN, INPUT_PULLUP);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // Start Serial for debug output
  Serial.begin(115200);
  delay(1000);

  Serial.println("Ready");

  wm.setHostname("shedheater2000");
  MDNS.begin("shedheater2000");
  wm.setConfigPortalTimeout(wifiPortalTimeout);
  wm.autoConnect("shedheater2000-Setup");
}

void loop()
{
  // Send mDNS update
  MDNS.update();

  // Handle WiFi Manager
  doWiFiManager();
}

void doWiFiManager()
{
  // is auto timeout portal running
  if (portalRunning)
  {
    wm.process();

    // check for timeout
    if ((millis() - startTime) > (wifiPortalTimeout * 1000))
    {
      Serial.println("portal timed out");
      portalRunning = false;
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
    startTime = millis();
  }
}
