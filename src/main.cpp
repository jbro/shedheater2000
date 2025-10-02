#include <Arduino.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <thermistor.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#define FAN_PIN D2
#define CLEAR_BTN_PIN D5
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
#define THERMISTOR_PIN A0

WiFiManager wm;

unsigned int wifiPortalTimeout = 180;

bool portalRunning = false;
bool startAP = false;

class IntParameter : public WiFiManagerParameter
{
public:
  IntParameter(const char *id, const char *placeholder, long value, const uint8_t length = 10)
      : WiFiManagerParameter("")
  {
    init(id, placeholder, String(value).c_str(), length, "", WFM_LABEL_BEFORE);
  }

  long getValue()
  {
    return String(WiFiManagerParameter::getValue()).toInt();
  }

  String getValueStr()
  {
    return String(WiFiManagerParameter::getValue());
  }
};

// Custom Parameters
IntParameter setpointParam("setpointParam", "Temperature Setpoint (C)", 5);
IntParameter hysteresisParam("hysteresisParam", "Hysteresis (C)", 1);
IntParameter fanRunOnParam("fanRunOnParam", "Fan Run On Time (seconds)", 60);
WiFiManagerParameter ntpServerParam("ntpServerParam", "NTP Server", "pool.ntp.org", 32);

Thermistor *thermistor;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServerParam.getValue(), 0, 3600); // Update every hour

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

  pinMode(CLEAR_BTN_PIN, INPUT_PULLUP);

  // Forget wifi credentials
  if (digitalRead(CLEAR_BTN_PIN) == LOW)
  {
    Serial.println("Button held on startup, clearing WiFi credentials");
    wm.resetSettings();
  }

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

  // Set up custom menu
  const char *menu[] = {"custom", "param", "info"};
  wm.setMenu(menu, sizeof(menu) / sizeof(menu[0]));

  // Add your custom menu HTML (e.g., a link to /status)
  wm.setCustomMenuHTML("<form action='/status' method='get'><button>Status</button></form><br />");

  // Add custom parameters
  wm.addParameter(&setpointParam);
  wm.addParameter(&hysteresisParam);
  wm.addParameter(&fanRunOnParam);
  wm.addParameter(&ntpServerParam);

  // TODO Restore parameter values from EEPROM
  // TODO register a callback to save the custom parameters to EEPROM
  // XXX don't save ntp server param to eeprom
  // TODO implement hysteresis and fan run on time

  // Add custom routes to WiFiManager's web server
  wm.setWebServerCallback([&]()
                          { wm.server->on("/status", HTTP_GET, [=]()
                                          {
      float temp = thermistor->readTempC();
      String setPoint = setpointParam.getValueStr();
      String hysteresis = hysteresisParam.getValueStr();
      String fanTimeout = fanRunOnParam.getValueStr();

      // XXX Move time and temperature into their own pages and update with JS
      String page;
      page += FPSTR(HTTP_HEAD_START);
      page.replace(FPSTR(T_v), "Status");
      page += FPSTR(HTTP_SCRIPT);
      page += FPSTR(HTTP_STYLE);
      page += FPSTR(HTTP_HEAD_END);
      page += FPSTR(HTTP_ROOT_MAIN);
      page.replace(FPSTR(T_t), "ShedHeater2000");
      page.replace(FPSTR(T_v), "Status");
      page += "<p>Current Time: " + timeClient.getFormattedTime() + " UTC</p>";
      page += "<p>Temperature: " + String(temp, 1) + " &deg;C</p>";
      page += "<p>Setpoint: " + setPoint + " &deg;C</p>";
      page += "<p>Hysteresis: " + hysteresis + " &deg;C</p>";
      page += "<p>Fan Run On Time: " + fanTimeout + " seconds</p>";
      page += FPSTR(HTTP_BR);
      page += FPSTR(HTTP_BACKBTN);
      page += FPSTR(HTTP_END);
      wm.server->send(200, "text/html", page); }); });
}

void loop()
{
  // Update NTP Client
  timeClient.update();

  // Send mDNS update
  MDNS.update();

  // Make sure the web portal is running if requested
  if (!wm.getConfigPortalActive() && !wm.getWebPortalActive())
  {
    wm.startWebPortal();
  }

  // Handle WiFi Manager
  wm.process();
}