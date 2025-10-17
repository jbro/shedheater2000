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

    async function poll() {
      try {
        const res = await fetch('/values', { cache: 'no-store' });
        if (!res.ok) return;
        const j = await res.json();

        if ($('temperature')) $('temperature').textContent =
          (typeof j.temperature === 'number') ? j.temperature.toFixed(1) : j.temperature;

        if ($('fan')) $('fan').textContent = j.fan ? 'On' : 'Off';
        if ($('heater1')) $('heater1').textContent = j.heater1 ? 'On' : 'Off';
        if ($('heater2')) $('heater2').textContent = j.heater2 ? 'On' : 'Off';
        if ($('fanRunEnd')) $('fanRunEnd').textContent = (j.fanRunEnd - j.now) / 1000 || -1;
        if ($('nextFanRun')) $('nextFanRun').textContent = (j.nextFanRun - j.now) / 1000 || -1;
        if ($('time')) $('time').textContent = j.time || '';
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

class IntParameter : public WiFiManagerParameter
{
public:
  IntParameter(const char *id, const char *placeholder, unsigned long value, const uint8_t length = 10)
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

// Custom Parameters
IntParameter setpointParam("setpointParam", "Temperature Setpoint (°C)", 6);
IntParameter hysteresisParam("hysteresisParam", "Hysteresis (°C)", 1);
IntParameter fanOverrunTimeParam("fanOverrunTimeParam", "Fan Overrun Time (seconds)", 60);
IntParameter fanTurnOnFrequencyParam("fanTurnOnFrequencyParam", "Fan Turn On Frequency (minutes)", 60);
IntParameter fanRunTimeParam("fanRunTimeParam", "Fan Run Time (minutes)", 5);
WiFiManagerParameter ntpServerParam("ntpServerParam", "NTP Server", "pool.ntp.org", 32);

// Periodic fan scheduling
unsigned long nextFanRun = 0;
unsigned long fanRunEnd = 0;

// now
unsigned long now = millis();

struct Config
{
  unsigned long setpoint;
  unsigned long hysteresis;
  unsigned long fanOverrunTime;
  unsigned long fanTurnOnFrequency;
  unsigned long fanRunTime;
  char ntpServer[32];
};

Thermistor *thermistor;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServerParam.getValue(), 0, 3600 * 1000); // Update every hour

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

  // Forget wifi credentials
  if (digitalRead(CLEAR_BTN_PIN) == LOW)
  {
    Serial.println("Button held on startup...");

    Serial.println("Clearing WiFi credentials");
    wm.resetSettings();

    Serial.println("Restore default values to EEPROM");
    EEPROM.put(0, config);
    EEPROM.commit();

    timeClient.forceUpdate();
  }

  // Setup Thermistors
  thermistor = new Thermistor(THERMISTOR_PIN, 3.3, 3.3, 1023, 10000, 10000, 27.8, 3380, 1, 0);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

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

  // Add custom routes to WiFiManager's web server
  wm.setWebServerCallback([&]()
                          {
     wm.server->on("/values", HTTP_GET, [=]()
                                          {
       float temp = thermistor->readTempC();
       bool fanState = digitalRead(FAN_PIN) == HIGH;
       bool heater1State = digitalRead(HEATER_1_PIN) == HIGH;
       bool heater2State = digitalRead(HEATER_2_PIN) == HIGH;
       String time = timeClient.getFormattedTime();

       String json;
       json += "{";
       json += "\"temperature\": " + String(temp) + ",";
       json += "\"fan\": " + String(fanState) + ",";
       json += "\"heater1\": " + String(heater1State) + ",";
       json += "\"heater2\": " + String(heater2State) + ",";
       json += "\"fanRunEnd\": " + String(fanRunEnd) + ",";
       json += "\"nextFanRun\": " + String(nextFanRun) + ",";
       json += "\"now\": " + String(now) + ",";
       json += "\"time\": \"" + time + "\"";
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
      page += "<p>Next Fan Run In: <span id=\"nextFanRun\"></span> seconds</p>";
      page += "<p>Fan Run End In: <span id=\"fanRunEnd\"></span> seconds</p>";
      page += "<p>Fan State: <span id=\"fan\"></span></p>";
      page += "<p>Heater 1 State: <span id=\"heater1\"></span></p>";
      page += "<p>Heater 2 State: <span id=\"heater2\"></span></p>";
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
}

bool fanRunning = false;
bool heatersRunning = false;
bool firstRun = true;

void turnOnFan()
{
  digitalWrite(FAN_PIN, HIGH);
  fanRunning = true;
}

void turnOffFan()
{
  digitalWrite(FAN_PIN, LOW);
  fanRunning = false;
}

void turnOnHeaters()
{
  turnOnFan(); // Ensure fan is on when heaters are on

  digitalWrite(HEATER_1_PIN, HIGH);
  digitalWrite(HEATER_2_PIN, HIGH);

  heatersRunning = true;
}

void turnOffHeaters(long now)
{
  digitalWrite(HEATER_1_PIN, LOW);
  digitalWrite(HEATER_2_PIN, LOW);

  // If the fan is going to turn off sooner than the overrun time, extend it
  if (now + (fanOverrunTimeParam.getValue() * 1000) > fanRunEnd)
  {
    fanRunEnd = now + (fanOverrunTimeParam.getValue() * 1000);
  }

  heatersRunning = false;
}

void loop()
{
  now = millis();

  // Update NTP Client
  timeClient.update();

  // Send mDNS update
  MDNS.update();

  // Make sure the web portal is running if requested
  if (!wm.getConfigPortalActive() && !wm.getWebPortalActive())
  {
    Serial.println("Webportal not running, starting");
    wm.startWebPortal();
  }

  // Handle WiFi Manager
  wm.process();

  float currentTemp = thermistor->readTempC();

  // Turn on the fan periodically independent of heating, skip first run
  if (now > nextFanRun && fanTurnOnFrequencyParam.getValue() > 0)
  {
    // Next run is now
    nextFanRun = now + (fanTurnOnFrequencyParam.getValue() * 60UL * 1000UL);

    // Skip first run
    if (firstRun)
    {
      firstRun = false;
    }
    else
    {
      turnOnFan();
      fanRunEnd = now + (fanRunTimeParam.getValue() * 60UL * 1000UL);
    }
  }

  // Turn on the heaters if the temperature is below the setpoint minus hysteresis
  if (currentTemp < setpointParam.getValue() - hysteresisParam.getValue() && !heatersRunning)
  {
    turnOnHeaters();
  }

  // Turn off the heaters if the temperature is at or above the setpoint
  if (currentTemp >= setpointParam.getValue() && heatersRunning)
  {
    turnOffHeaters(now);
  }

  // Turn off the fan if the heaters are off, and the fan run time has exceeded
  if (!heatersRunning && now > fanRunEnd && fanRunning)
  {
    turnOffFan();
  }

  delay(100);
}