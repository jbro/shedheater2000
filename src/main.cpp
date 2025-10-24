#include <Arduino.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <thermistor.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

#define FAN_PIN D2
#define CLEAR_BTN_PIN D3
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
#define THERMISTOR_PIN A0

// Javascript to poll for updated values
static const char SCRIPT_POLL[] PROGMEM = R"JS(
<script>
  (function () {
    const $ = id => document.getElementById(id);

    function formatUTCDate(timestamp) {
      if (!timestamp) return 'Never';
      const date = new Date(timestamp * 1000);
      return date.toUTCString();
    }

    async function poll() {
      try {
        const res = await fetch('/values', { cache: 'no-store' });
        if (!res.ok) return;
        const j = await res.json();

        // Update DOM elements with the received values
        $('temperature').textContent = j.temperature;
        $('fanState').textContent = j.fanState;
        $('heater1State').textContent = j.heater1State;
        $('heater2State').textContent = j.heater2State;
        $('time').textContent = j.time;
        $('timeToNextFanRun').textContent = j.timeToNextFanRun;
        $('timeToNextFanOff').textContent = j.timeToNextFanOff;
        $('timeLeftofOverrun').textContent = j.timeLeftofOverrun;

        // Format heaterLastOn as a readable UTC date if available
        $('heaterLastOn').textContent = formatUTCDate(j.heaterLastOn);
      } catch (e) {
        // ignore errors
      }
    }

    poll();
    setInterval(poll, 1000);
  })();
</script>
)JS";

WiFiManager wm;

unsigned int wifiPortalTimeout = 180;

bool portalRunning = false;
bool startAP = false;

class UnsignedLongParameter : public WiFiManagerParameter
{
public:
  UnsignedLongParameter(const char *id, const char *placeholder, unsigned long value, const uint8_t length = 10)
      : WiFiManagerParameter("")
  {
    init(id, placeholder, String(value).c_str(), length, "", WFM_LABEL_BEFORE);
  }

  unsigned long getValue()
  {
    return String(WiFiManagerParameter::getValue()).toInt();
  }

  String getValueStr()
  {
    return String(WiFiManagerParameter::getValue());
  }
};

class LongParameter : public WiFiManagerParameter
{
public:
  LongParameter(const char *id, const char *placeholder, signed long value, const uint8_t length = 11)
      : WiFiManagerParameter("")
  {
    init(id, placeholder, String(value).c_str(), length, "", WFM_LABEL_BEFORE);
  }

  signed long getValue()
  {
    return String(WiFiManagerParameter::getValue()).toInt();
  }

  String getValueStr()
  {
    return String(WiFiManagerParameter::getValue());
  }
};

// Custom Parameters
LongParameter setpointParam("setpointParam", "Temperature Setpoint (°C)", 6);
LongParameter hysteresisParam("hysteresisParam", "Hysteresis (°C)", 1);
UnsignedLongParameter fanOverrunTimeParam("fanOverrunTimeParam", "Fan Overrun Time (seconds)", 60);
UnsignedLongParameter fanTurnOnFrequencyParam("fanTurnOnFrequencyParam", "Fan Turn On Frequency (minutes)", 60);
UnsignedLongParameter fanRunTimeParam("fanRunTimeParam", "Fan Run Time (minutes)", 5);
WiFiManagerParameter ntpServerParam("ntpServerParam", "NTP Server", "pool.ntp.org", 32);
// TODO introduce mqtt support starting where, with a new parameter

// Timers
unsigned long now = millis();
unsigned long FanScheduler = now;
unsigned long HeatersLastOn = now;

// State
bool fanRunning = false;
bool heatersRunning = false;
bool inFanScheduledRun = false;
float currentTemperature = 0.0;

// Fan control timings in ms
unsigned long fanOverrunTimeMs;
unsigned long fanRunTimeMs;
unsigned long fanTurnOnFrequencyMs;

struct Config
{
  long setpoint;
  long hysteresis;
  unsigned long fanOverrunTime;
  unsigned long fanTurnOnFrequency;
  unsigned long fanRunTime;
  char ntpServer[32];
};

Thermistor *thermistor;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "", 0, 3600UL * 1000UL); // Update every hour

void onSaveParams()
{
  Serial.println("Saving config");

  Config config = {
      .setpoint = setpointParam.getValue(),
      .hysteresis = hysteresisParam.getValue(),
      .fanOverrunTime = fanOverrunTimeParam.getValue(),
      .fanTurnOnFrequency = fanTurnOnFrequencyParam.getValue(),
      .fanRunTime = fanRunTimeParam.getValue(),
  };
  const char *ntpServer = ntpServerParam.getValue();
  strncpy(config.ntpServer, ntpServer, sizeof(config.ntpServer));
  EEPROM.put(0, config);
  EEPROM.commit();

  // Convert parameter values to ms where applicable
  fanOverrunTimeMs = fanOverrunTimeParam.getValue() * 1000;              // Convert seconds to ms
  fanRunTimeMs = fanRunTimeParam.getValue() * 60 * 1000;                 // Convert minutes to ms
  fanTurnOnFrequencyMs = fanTurnOnFrequencyParam.getValue() * 60 * 1000; // Convert minutes to ms

  timeClient.setPoolServerName(ntpServer);
}

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

  // Start Serial for debug output
  Serial.begin(115200);
  delay(300);

  EEPROM.begin(sizeof(Config));

  // Config default values
  Config config = {
      .setpoint = setpointParam.getValue(),
      .hysteresis = hysteresisParam.getValue(),
      .fanOverrunTime = fanOverrunTimeParam.getValue(),
      .fanTurnOnFrequency = fanTurnOnFrequencyParam.getValue(),
      .fanRunTime = fanRunTimeParam.getValue(),
  };
  const char *defaultNtpServer = ntpServerParam.getValue();
  strncpy(config.ntpServer, defaultNtpServer, sizeof(config.ntpServer));

  // Forget wifi credentials and reset config if button held on startup
  if (digitalRead(CLEAR_BTN_PIN) == LOW)
  {
    Serial.println("Button held on startup...");

    Serial.println("Clearing WiFi credentials");
    wm.resetSettings();

    Serial.println("Restore default values to EEPROM");
    EEPROM.put(0, config);
    EEPROM.commit();
  }

  // Setup Thermistors
  thermistor = new Thermistor(THERMISTOR_PIN, 3.3, 3.3, 1023, 10000, 10000, 27.8, 3380, 10, 10);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // Set host name and start mDNS
  wm.setHostname("shedheater2000");
  MDNS.begin("shedheater2000");

  // Set up custom menu
  const char *menu[] = {"custom", "param", "info"};
  wm.setMenu(menu, sizeof(menu) / sizeof(menu[0]));

  // Add your custom menu HTML (e.g., a link to /status)
  wm.setCustomMenuHTML("<form action='/status' method='get'><button>Status</button></form><br />");

  // Register the save callback
  wm.setSaveParamsCallback(onSaveParams);

  // Restore parameters from EEPROM
  EEPROM.get(0, config);
  setpointParam.setValue(String(config.setpoint).c_str(), 10);
  hysteresisParam.setValue(String(config.hysteresis).c_str(), 10);
  fanOverrunTimeParam.setValue(String(config.fanOverrunTime).c_str(), 10);
  fanTurnOnFrequencyParam.setValue(String(config.fanTurnOnFrequency).c_str(), 10);
  fanRunTimeParam.setValue(String(config.fanRunTime).c_str(), 10);
  ntpServerParam.setValue(config.ntpServer, 32);

  // Add custom parameters
  wm.addParameter(&setpointParam);
  wm.addParameter(&hysteresisParam);
  wm.addParameter(&fanOverrunTimeParam);
  wm.addParameter(&fanTurnOnFrequencyParam);
  wm.addParameter(&fanRunTimeParam);
  wm.addParameter(&ntpServerParam);

  // Convert parameter values to ms where applicable
  fanOverrunTimeMs = fanOverrunTimeParam.getValue() * 1000;              // Convert seconds to ms
  fanRunTimeMs = fanRunTimeParam.getValue() * 60 * 1000;                 // Convert minutes to ms
  fanTurnOnFrequencyMs = fanTurnOnFrequencyParam.getValue() * 60 * 1000; // Convert minutes to ms

  // Initialize fan overrun timer in the past so fan doesn't turn on immediately on boot
  HeatersLastOn = now - fanOverrunTimeMs;

  // Set NTP server from config
  timeClient.setPoolServerName(ntpServerParam.getValue());

  Serial.println("Config Restored:");
  Serial.printf(" Setpoint: %ld °C\n", config.setpoint);
  Serial.printf(" Hysteresis: %ld °C\n", config.hysteresis);
  Serial.printf(" Fan Overrun Time: %ld s\n", config.fanOverrunTime);
  Serial.printf(" Fan Turn On Frequency: %ld s\n", config.fanTurnOnFrequency);
  Serial.printf(" Fan Run Time: %ld min\n", config.fanRunTime);
  Serial.printf(" NTP Server: %s\n", config.ntpServer);

  // Add custom routes to WiFiManager's web server
  wm.setWebServerCallback([&]()
                          {
     wm.server->on("/values", HTTP_GET, [=]()
                                          {
       String fanState = digitalRead(FAN_PIN) == HIGH ? "ON" : "OFF";
       String heater1State = digitalRead(HEATER_1_PIN) == HIGH ? "ON" : "OFF";
       String heater2State = digitalRead(HEATER_2_PIN) == HIGH ? "ON" : "OFF";
       String time = timeClient.getFormattedTime();

       long timeToNextFanRun = (long(fanTurnOnFrequencyMs) - long(now - FanScheduler)) / 1000;
       long timeToNextFanOff = (long(fanRunTimeMs) - long(now - FanScheduler)) / 1000;
       long timeLeftofOverrun = (long(fanOverrunTimeMs) - long(now - HeatersLastOn)) / 1000;
       time_t heaterLastOn = timeClient.getEpochTime() - ((now - HeatersLastOn) / 1000);

       String json;
       json += "{";
       json += "\"temperature\": \"" + String(currentTemperature) + "\",";
       json += "\"fanState\": \"" + fanState + "\",";
       json += "\"heater1State\": \"" + heater1State + "\",";
       json += "\"heater2State\": \"" + heater2State + "\",";
       json += "\"time\": \"" + time + "\"" + ",";
       json += "\"timeToNextFanRun\": \"" + (fanTurnOnFrequencyMs > 0 && !heatersRunning? String(timeToNextFanRun) : "∞") + "\",";
       json += "\"timeToNextFanRunRaw\":  \"" + String(timeToNextFanRun) + "\",";
       json += "\"timeToNextFanOff\": \"" + (fanTurnOnFrequencyMs > 0 && inFanScheduledRun && !heatersRunning && timeToNextFanOff > 0 ? String(timeToNextFanOff) : "∞") + "\",";
       json += "\"timeToNextFanOffRaw\":  \"" + String(timeToNextFanOff) + "\",";
       json += "\"timeLeftofOverrun\": \"" + (timeLeftofOverrun < 0 || heatersRunning ? "0" :  String(timeLeftofOverrun)) + "\",";
       json += "\"timeLeftofOverrunRaw\":  \"" + String(timeLeftofOverrun) + "\",";
       json += "\"heaterLastOn\": \"" + String(heaterLastOn) + "\"";
       json += "}";

       wm.server->send(200, "application/json", json);
     });
     wm.server->on("/status", HTTP_GET, [=]()
                                          {
      String setPoint = setpointParam.getValueStr();
      String hysteresis = hysteresisParam.getValueStr();
      String fanTimeout = fanOverrunTimeParam.getValueStr();
      String fanTurnOnFrequency = fanTurnOnFrequencyParam.getValueStr();
      String fanRunTime = fanRunTimeParam.getValueStr();
      String ntpServer = ntpServerParam.getValue();

      String page;
      page += FPSTR(HTTP_HEAD_START);
      page.replace(FPSTR(T_v), "Status");
      page += FPSTR(HTTP_SCRIPT);
      page += FPSTR(SCRIPT_POLL);
      page += FPSTR(HTTP_STYLE);
      page += FPSTR(HTTP_HEAD_END);
      page += FPSTR(HTTP_ROOT_MAIN);
      page.replace(FPSTR(T_t), "ShedHeater2000");
      page.replace(FPSTR(T_v), "Time");
      page += "<p><span id=\"time\"></span> UTC</p>";
      page += "<p>In sync? " + String(timeClient.isTimeSet() ? "Yes" : "No") + "</p>";
      page += "<p>NTP Server: " + ntpServer + "</p>";
      page += "<h3>Current Status</h3>";
      page += "<p>Temperature: <span id=\"temperature\"></span> &deg;C</p>";
      page += "<p>Heater Last On: <span id=\"heaterLastOn\"></span></p>";
      page += "<p>Fan Overrun Time: <span id=\"timeLeftofOverrun\"></span> seconds</p>";
      page += "<p>Next Fan Run In: <span id=\"timeToNextFanRun\"></span> seconds</p>";
      page += "<p>Next Fan Off In: <span id=\"timeToNextFanOff\"></span> seconds</p>";
      page += "<p>Fan State: <span id=\"fanState\"></span></p>";
      page += "<p>Heater 1 State: <span id=\"heater1State\"></span></p>";
      page += "<p>Heater 2 State: <span id=\"heater2State\"></span></p>";
      page += "<h3>Parameters</h3>";
      page += "<p>Setpoint: " + setPoint + " &deg;C</p>";
      page += "<p>Hysteresis: " + hysteresis + " &deg;C</p>";
      page += "<p>Fan Overrun Time: " + fanTimeout + " seconds</p>";
      page += "<p>Fan Turn On Frequency: " + fanTurnOnFrequency + " minutes</p>";
      page += "<p>Fan Run Time: " + fanRunTime + " minutes</p>";
      page += FPSTR(HTTP_BR);
      page += FPSTR(HTTP_BACKBTN);
      page += FPSTR(HTTP_END);

      wm.server->send(200, "text/html", page); }); });

  // Start config portal if wifi isn't connecting
  wm.setConfigPortalTimeout(wifiPortalTimeout);
  wm.autoConnect("shedheater2000-Setup");
}

void setFan(bool on)
{
  if (on)
  {
    digitalWrite(FAN_PIN, HIGH);
    fanRunning = true;
  }
  else
  {
    digitalWrite(FAN_PIN, LOW);
    fanRunning = false;
  }
}
void setHeaters(bool on)
{
  if (on)
  {
    digitalWrite(HEATER_1_PIN, HIGH);
    digitalWrite(HEATER_2_PIN, HIGH);
    heatersRunning = true;
  }
  else
  {
    digitalWrite(HEATER_1_PIN, LOW);
    digitalWrite(HEATER_2_PIN, LOW);
    heatersRunning = false;
  }
}

void controlHeaters()
{
  // If temperature is below setpoint - hysteresis, turn on heaters
  if (!heatersRunning && currentTemperature < (setpointParam.getValue() - hysteresisParam.getValue()))
  {
    setHeaters(true);
    HeatersLastOn = now;
    return;
  }

  // If heaters are running and temperature is above setpoint, turn off heaters
  if (heatersRunning)
  {
    HeatersLastOn = now;

    if (currentTemperature >= setpointParam.getValue())
    {
      setHeaters(false);
      return;
    }
  }
}

void controlFan()
{
  // If fan is running due to heaters, do nothing
  if (heatersRunning)
  {
    setFan(true); // Safety! Ensure fan is on when heaters are on
    return;
  }

  // If it's less than fan overrun time since heaters turned off, keep fan on
  if ((now - HeatersLastOn) < fanOverrunTimeMs)
  {
    setFan(true);
    return;
  }

  if (fanRunning && !inFanScheduledRun && (now - HeatersLastOn) >= fanOverrunTimeMs)
  {
    setFan(false);
    return;
  }

  // If fan schedule frequency is 0, do not run scheduled fan runs
  if (fanTurnOnFrequencyMs <= 0)
  {
    FanScheduler = now;
    setFan(false);
    return;
  }

  // If we are not in a scheduled fan run, check if it's time to start one
  if (!inFanScheduledRun && (now - FanScheduler) > fanTurnOnFrequencyMs)
  {
    inFanScheduledRun = true;
    FanScheduler = now;
    setFan(true);
    return;
  }

  // If we are in a scheduled fan run, check if it's time to stop
  if (inFanScheduledRun && (now - FanScheduler) > fanRunTimeMs)
  {
    inFanScheduledRun = false;
    setFan(false);
    return;
  }
}

void loop()
{
  // This blocks for about 10*10ms in between sampling the thermistor,
  // which is ok because it lets the wifi stack do its thing
  currentTemperature = thermistor->readTempC();

  // Update current time
  now = millis();

  // Update NTP Client if connected to WiFi
  if (WiFi.status() == WL_CONNECTED)
  {
    timeClient.update();
  }

  // Send mDNS update if connected to WiFi
  if (WiFi.status() == WL_CONNECTED)
  {
    MDNS.update();
  }

  // Make sure the web portal is running even if someone closed it in the web UI
  if (!wm.getConfigPortalActive() && !wm.getWebPortalActive())
  {
    Serial.println("Web portal not running, starting");
    wm.startWebPortal();
  }

  // Handle WiFi Manager
  wm.process();

  // Control Fan and Heaters
  controlHeaters();
  controlFan();
}