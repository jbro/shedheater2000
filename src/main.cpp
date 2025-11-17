#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "secrets.h"
#include <thermistor.h>
#include <DHT_Async.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoMqttClient.h>

// Holds the current time in milliseconds
unsigned long now;

// Set up Wifi
const unsigned long WIFI_CONNECT_RETRY_INTERVAL_MS = 10ul * 1000ul;
unsigned long lastWiFiConnectAttempt;

// Set up NTP Client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "dk.pool.ntp.org", 0, 3600 * 1000); // Update every hour

// Set up mqtt
void publishMqttData();
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char MQTT_BROKER[] = "10.1.1.1";
const int MQTT_PORT = 1883;
const char MQTT_CLIENT_ID[] = "shedheater2000";
const char MQTT_TOPIC[] = "shed/heater2000/status";
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 1ul * 1000ul;
unsigned long lastMqttPublish;

// Set up internal DHT22 sensor
void readInternalSensor();
#define DHT22_PIN D5
DHT_Async internalSensor(DHT22_PIN, DHT_TYPE_22);
const unsigned long DHT_READ_INTERVAL_MS = 2ul * 1000ul;
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
unsigned long fanRunTimeAccumulated = 0; // Total fan run time accumulated in current schedule
unsigned long fanRunTimeStart;           // Time when the fan was turned on for fan run
void controlHeater();
void turnOnHeater();
void turnOffHeater();
#define HEATER_1_PIN D6
#define HEATER_2_PIN D7
const float HEATER_SETPOINT_TEMPERATURE = 5.0f; // Target temperature to maintain in Celsius
const float HEATER_HYSTERESIS = 0.5f;           // Hysteresis for temperature control
bool heaterState = false;
unsigned long lastHeaterOff;

// Set up status printer
void printStatus();
void printParameters();
const unsigned long STATUS_PRINT_INTERVAL_MS = 1ul * 1000ul;
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

  WiFi.mode(WIFI_STA);
  WiFi.hostname(MQTT_CLIENT_ID);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // Start Serial for debug output
  Serial.begin(115200);
  delay(300);

  // Set timers to start values
  now = millis();
  lastWiFiConnectAttempt = now - WIFI_CONNECT_RETRY_INTERVAL_MS; // Force immediate WiFi connect attempt
  lastDHTRead = now - DHT_READ_INTERVAL_MS;                      // Force immediate DHT read
  lastExternalTempRead = now - EXTERNAL_TEMP_READ_INTERVAL_MS;   // Force immediate external temp read
  lastFanOn = now;                                               // Pretend the fan was just turned on on startup, so we run for the first time after FAN_TURN_ON_FREQ_MS
  fanRunTimeStart = now;                                         // Initialize fan run time
  lastHeaterOff = now - FAN_OVERRUN_MS;                          // The heater has never been on
  lastStatusPrint = now;                                         // We are okay to wait STATUS_PRINT_INTERVAL_MS before first print

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

  // Set mqtt client id
  mqttClient.setId(MQTT_CLIENT_ID);

  // Start NTP client
  timeClient.begin();
}

void loop()
{
  // Update current time
  now = millis();

  // If WiFi is not connected, attempt to connect
  if (WiFi.status() != WL_CONNECTED)
  {
    if (now - lastWiFiConnectAttempt >= WIFI_CONNECT_RETRY_INTERVAL_MS)
    {
      WiFi.begin(ssid, password);
      lastWiFiConnectAttempt = now;
    }
  }

  // Read sensors, control heater and fan
  readInternalSensor();
  readExternalSensor();
  controlHeater();
  controlFan();

  // If wifi is connected
  if (WiFi.status() == WL_CONNECTED)
  {
    // Update NTP client
    timeClient.update();

    // Keep MQTT client alive
    mqttClient.poll();

    // Send MQTT data periodically
    publishMqttData();
  }

  // Print status periodically
  printStatus();

  yield();
}

void publishMqttData()
{
  if (now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL_MS)
  {
    if (!mqttClient.connected())
    {
      mqttClient.connect(MQTT_BROKER, MQTT_PORT);
    }

    if (mqttClient.connected())
    {
      // Send status message as CSV
      char payload[192];
      snprintf(payload, sizeof(payload), "%lu,%d,%lu,%d,%.2f,%.2f,%.2f,%d,%d,%d,%lu",
               timeClient.getEpochTime(),      // Current epoch time
               timeClient.isTimeSet() ? 1 : 0, // Time synced state
               now / 1000ul,                   // Uptime in seconds
               WiFi.RSSI(),                    // WiFi RSSI
               internalTemperature,            // Internal temperature
               internalHumidity,               // Internal humidity
               externalTemperature,            // External temperature
               heaterState ? 1 : 0,            // Heater state
               fanState ? 1 : 0,               // Fan state
               fanScheduledRun ? 1 : 0,        // Fan scheduled run
               fanRunTimeAccumulated);         // Fan run time accumulated

      mqttClient.beginMessage(MQTT_TOPIC);
      mqttClient.print(payload);
      mqttClient.endMessage();
    }

    lastMqttPublish = now;
  }
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
    }
    lastDHTRead = now;
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
  if (!fanState)
  {
    digitalWrite(FAN_PIN, HIGH);
    fanState = true;

    // Record the time the fan was turned on
    fanRunTimeStart = now;
  }

  fanRunTimeAccumulated += now - fanRunTimeStart;
  fanRunTimeStart = now;
}

void turnOffFan()
{
  if (fanState)
  {

    digitalWrite(FAN_PIN, LOW);
    fanState = false;

    // Update accumulated fan run time
    fanRunTimeAccumulated += now - fanRunTimeStart;
    fanRunTimeStart = now;
  }
}

void turnOnHeater()
{
  if (!heaterState)
  {
    digitalWrite(HEATER_1_PIN, HIGH);
    digitalWrite(HEATER_2_PIN, HIGH);
    heaterState = true;
  }
}

void turnOffHeater()
{
  if (heaterState)
  {

    digitalWrite(HEATER_1_PIN, LOW);
    digitalWrite(HEATER_2_PIN, LOW);

    lastHeaterOff = now;

    heaterState = false;
  }
}

void controlFan()
{
  // Check if it's time to turn on the fan for air circulation
  // but don't start or stop the fan until later
  if (now - lastFanOn >= FAN_TURN_ON_FREQ_MS)
  {
    // Check if we need to run the fan this schedule
    if (fanRunTimeAccumulated < FAN_RUN_TIME_MS)
    {
      fanScheduledRun = true;
    }

    lastFanOn = now;
  }

  // Check if the scheduled fan run time has elapsed
  if (fanScheduledRun && (now - lastFanOn >= FAN_RUN_TIME_MS))
  {
    fanScheduledRun = false;

    fanRunTimeAccumulated = 0;
  }

  // If the heater is on, ensure the fan is on
  if (heaterState)
  {
    turnOnFan();
    return;
  }

  // If we are in the fan overrun period, keep the fan on
  if (now - lastHeaterOff < FAN_OVERRUN_MS)
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
  // Safety check: if temperature is invalid, turn off heater
  if (isnan(internalTemperature))
  {
    turnOffHeater();

    return;
  }

  if (internalTemperature < HEATER_SETPOINT_TEMPERATURE - HEATER_HYSTERESIS)
  {
    if (!heaterState)
    {
      turnOnHeater();
      return;
    }
  }

  if (internalTemperature > HEATER_SETPOINT_TEMPERATURE + HEATER_HYSTERESIS)
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
    Serial.print(timeClient.getFormattedTime());
    Serial.print(" | ");
    Serial.print("Time synced: ");
    Serial.print(timeClient.isTimeSet() ? "YES" : "NO");
    Serial.print(" | ");
    Serial.print("Uptime: ");
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
    Serial.print(" | Fan Run Time Accumulated: ");
    Serial.print(fanRunTimeAccumulated / 1000);
    Serial.print(" s");
    Serial.print(" | WiFi Connected: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "YES" : "NO");
    Serial.print(" | WiFi SSID: ");
    Serial.print(WiFi.SSID());
    Serial.print(" | WiFi RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.print(" dBm");
    Serial.print(" | MQTT Connected: ");
    Serial.print(mqttClient.connected() ? "YES" : "NO");
    Serial.print(" | Free Heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" bytes");
    Serial.println();

    lastStatusPrint = now;
  }
}
