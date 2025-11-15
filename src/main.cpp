#include <Arduino.h>
#include <WiFiManager.h>
#include <thermistor.h>
#include <DHT_Async.h>
#include <ESP8266mDNS.h>
// #include <NTPClient.h>
// #include <WiFiUdp.h>
// #include <EEPROM.h>
// #include <ArduinoMqttClient.h>

// Holds the current time in milliseconds
unsigned long now;

// Set up WifiManager
#define CLEAR_BTN_PIN D3
WiFiManager wm;
const unsigned long WIFI_CONFIG_TIMEOUT_S = 2ul * 60ul;

// Set up internal DHT22 sensor
void readInternalSensor();
#define DHT22_PIN D5
DHT_Async internalSensor(DHT22_PIN, DHT_TYPE_22);
const unsigned long DHT_READ_INTERVAL_MS = 2000ul;
unsigned long lastDHTRead;
float internalTemperature = NAN;
float internalHumidity = NAN;

// Set up the external thermistor sensor
void readExternalSensor();
#define THERMISTOR_PIN A0
Thermistor externalSensor(
    THERMISTOR_PIN, // pin
    3.3,            // vcc
    3.3,            // analogReference
    1023,           // adcMax
    10000,          // seriesResistor
    10000,          // thermistorNominal
    25,             // temperatureNominal
    3950,           // bCoefficient
    1,              // samples
    0               // sampleDelay
);
const unsigned long EXTERNAL_TEMP_READ_INTERVAL_MS = 100ul;
const size_t EXTERNAL_TEMP_SMOOTHING_COUNT = 10;
unsigned long lastExternalTempRead;
float externalTemperatureReadings[EXTERNAL_TEMP_SMOOTHING_COUNT];
size_t externalTempReadingIndex = 0;
float externalTemperature = NAN;

// Set up the heater and fan controllers
void controlFan();
void turnOnFan();
void turnOffFan();
#define FAN_PIN D2
const unsigned long FAN_OVERRUN_MS = 30ul * 1000ul;             // Fan overrun time after heater turns off
const unsigned long FAN_TURN_ON_FREQ_MS = 60ul * 60ul * 1000ul; // Fan turn on frequency for air circulation
const unsigned long FAN_RUN_TIME_MS = 5ul * 60ul * 1000ul;      // Fan run time for air circulation
bool fanState = false;
bool fanScheduledRun = false;
unsigned long lastFanOn;
void controlHeater();
void turnOnHeater();
void turnOffHeater();
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
const float HEATER_SETPOINT_TEMPERATURE = 5.0f; // Target temperature to maintain in Celsius
const float HEATER_HYSTERESIS = 0.5f;           // Hysteresis for temperature control
bool heaterState = false;
unsigned long lastHeaterOn;

// Set up status printer
void printStatus();
void printParameters();
const unsigned long STATUS_PRINT_INTERVAL_MS = 1000ul;
unsigned long lastStatusPrint;

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
  pinMode(CLEAR_BTN_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  // Start Serial for debug output
  Serial.begin(115200);
  delay(300);

  // Set timers to start values
  now = millis();
  lastDHTRead = now - DHT_READ_INTERVAL_MS;                    // Force immediate DHT read
  lastExternalTempRead = now - EXTERNAL_TEMP_READ_INTERVAL_MS; // Force immediate external temp read
  lastFanOn = now;                                             // Pretend the fan was just turned on on startup, so we run for the first time after FAN_TURN_ON_FREQ_MS
  lastHeaterOn = now - FAN_OVERRUN_MS;                         // The heater has never been on
  lastStatusPrint = now;                                       // We are okay to wait STATUS_PRINT_INTERVAL_MS before first print

  // Initialize external temperature readings to NAN
  for (size_t i = 0; i < EXTERNAL_TEMP_SMOOTHING_COUNT; ++i)
  {
    externalTemperatureReadings[i] = NAN;
  }

  // Turn off heater and fan to start
  turnOffHeater();
  turnOffFan();

  // Print the configuration parameters
  printParameters();

  // Erase settings if button is held during boot
  if (digitalRead(CLEAR_BTN_PIN) == LOW)
  {
    Serial.println("Clearing WiFi settings...");
    wm.resetSettings();
  }

  // Setup wifi manager
  wm.setConfigPortalTimeout(WIFI_CONFIG_TIMEOUT_S);
  wm.setWiFiAutoReconnect(true);
  wm.setConfigPortalBlocking(false);

  // Attempt to connect to saved WiFi
  wm.autoConnect("ShedHeater2000_Config");

  // Start MDNS
  MDNS.begin("shedheater2000");
}

void loop()
{
  wm.process();

  // Update current time
  now = millis();

  // Read sensors, control heater and fan
  readInternalSensor();
  readExternalSensor();
  controlHeater();
  controlFan();

  // If wifi is connected
  if (WiFi.status() == WL_CONNECTED)
  {
    // Publish MDNS
    MDNS.update();

    // If web portal is not active, enable it
    if (!wm.getWebPortalActive())
    {
      wm.startWebPortal();
    }
  }

  // Print status periodically
  printStatus();

  yield();
}

void readInternalSensor()
{
  float temperature;
  float humidity;

  if (now - lastDHTRead >= DHT_READ_INTERVAL_MS)
  {
    if (internalSensor.measure(&temperature, &humidity))
    {
      internalTemperature = temperature;
      internalHumidity = humidity;

      lastDHTRead = now;
    }
  }
}

void readExternalSensor()
{
  if (now - lastExternalTempRead >= EXTERNAL_TEMP_READ_INTERVAL_MS)
  {
    float tempC = externalSensor.readTempC();
    if (!isnan(tempC))
    {
      externalTemperatureReadings[externalTempReadingIndex] = tempC;
      externalTempReadingIndex = (externalTempReadingIndex + 1) % EXTERNAL_TEMP_SMOOTHING_COUNT;

      // Compute smoothed temperature
      float sum = 0.0f;
      size_t count = 0;
      for (size_t i = 0; i < EXTERNAL_TEMP_SMOOTHING_COUNT; i++)
      {
        if (!isnan(externalTemperatureReadings[i]))
        {
          sum += externalTemperatureReadings[i];
          count++;
        }
      }
      if (count > 0)
      {
        externalTemperature = sum / count;
      }
    }

    lastExternalTempRead = now;
  }
}

void turnOnFan()
{
  digitalWrite(FAN_PIN, HIGH);
  fanState = true;
}

void turnOffFan()
{
  digitalWrite(FAN_PIN, LOW);
  fanState = false;
}

void turnOnHeater()
{
  digitalWrite(HEATER_1_PIN, HIGH);
  digitalWrite(HEATER_2_PIN, HIGH);
  heaterState = true;
}

void turnOffHeater()
{
  digitalWrite(HEATER_1_PIN, LOW);
  digitalWrite(HEATER_2_PIN, LOW);
  heaterState = false;
}

void controlFan()
{
  // Check if it's time to turn on the fan for air circulation
  // but don't start or stop the fan until later
  if (now - lastFanOn >= FAN_TURN_ON_FREQ_MS)
  {
    fanScheduledRun = true;
    lastFanOn = now;
  }

  // Check if the scheduled fan run time has elapsed
  if (fanScheduledRun && (now - lastFanOn >= FAN_RUN_TIME_MS))
  {
    fanScheduledRun = false;
  }

  // If the heater is on, ensure the fan is on
  if (heaterState)
  {
    turnOnFan();
    return;
  }

  // If we are in the fan overrun period, keep the fan on
  if (now - lastHeaterOn < FAN_OVERRUN_MS)
  {
    turnOnFan();
    return;
  }

  // We are past the safety fan controls, so handle scheduled runs
  if (fanScheduledRun)
  {
    turnOnFan();
    return;
  }

  // Otherwise, turn off the fan
  turnOffFan();
}

void controlHeater()
{
  if (externalTemperature < HEATER_SETPOINT_TEMPERATURE - HEATER_HYSTERESIS)
  {
    if (!heaterState)
    {
      turnOnHeater();
      lastHeaterOn = now;

      return;
    }
  }

  if (externalTemperature > HEATER_SETPOINT_TEMPERATURE + HEATER_HYSTERESIS)
  {
    if (heaterState)
    {
      turnOffHeater();
    }
  }
}

void printParameters()
{
  Serial.println("Shed Heater 2000 Initialized");
  Serial.print("Heater Setpoint Temperature: ");
  Serial.print(HEATER_SETPOINT_TEMPERATURE);
  Serial.println(" C");
  Serial.print("Heater Hysteresis: ");
  Serial.print(HEATER_HYSTERESIS);
  Serial.println(" C");
  Serial.print("Fan Overrun Time: ");
  Serial.print(FAN_OVERRUN_MS / 1000);
  Serial.println(" s");
  Serial.print("Fan Turn On Frequency: ");
  Serial.print(FAN_TURN_ON_FREQ_MS / 1000);
  Serial.println(" s");
  Serial.print("Fan Run Time: ");
  Serial.print(FAN_RUN_TIME_MS / 1000);
  Serial.print(" s");
  Serial.println();
}

void printStatus()
{
  if (now - lastStatusPrint >= STATUS_PRINT_INTERVAL_MS)
  {
    Serial.print("Time: ");
    Serial.print(now / 1000);
    Serial.print(" s | ");
    Serial.print("Internal Temp: ");
    Serial.print(internalTemperature);
    Serial.print(" C, Humidity: ");
    Serial.print(internalHumidity);
    Serial.print(" % | External Temp: ");
    Serial.print(externalTemperature);
    Serial.print(" C");
    Serial.print(" | Heater: ");
    Serial.print(heaterState ? "ON" : "OFF");
    Serial.print(" | Fan: ");
    Serial.print(fanState ? "ON" : "OFF");
    Serial.print(" | Scheduled Fan Run: ");
    Serial.print(fanScheduledRun ? "YES" : "NO");
    Serial.print(" | WiFi Connected: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "YES" : "NO");
    Serial.print(" | WiFi SSID: ");
    Serial.print(WiFi.SSID());
    Serial.print(" | WiFi RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.print(" dBm");
    Serial.println();

    lastStatusPrint = now;
  }
}
