// Wemos D1 Mini pin reference
// Pin  Function                        ESP-8266 Pin
// TX   TXD                             TXD
// RX   RXD                             RXD
// A0   Analog input, max 3.3V input    A0
// D0   IO                              GPIO16
// D1   IO, SCL                         GPIO5
// D2   IO, SDA                         GPIO4
// D3   IO, 10k Pull-up                 GPIO0
// D4   IO, 10k Pull-up, BUILTIN_LED    GPIO2
// D5   IO, SCK                         GPIO14
// D6   IO, MISO                        GPIO12
// D7   IO, MOSI                        GPIO13
// D8   IO, 10k Pull-down, SS           GPIO15
// G    Ground                          GND
// 5V   5V                              -
// 3V3  3.3V                            3.3V
// RST  Reset                           RST

#include <ESP8266WiFi.h>          // See https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/readme.html
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <FastLED.h>              // See http://fastled.io/
#include <ezTime.h>               // See https://github.com/ropg/ezTime
#include <FS.h>                   // See https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html
#include <ArduinoJson.h>          // See https://arduinojson.org/
#include "DHT.h"                  // See https://github.com/adafruit/DHT-sensor-library

// -------------- LED's ---------------
#define NR_LEDS       (6)
#define PIN_LED_DATA  (12)
CRGB leds[NR_LEDS];
byte previousLedBrightness;

// -------------- Thermometer / Hygrometer ---------------
#define DHTTYPE DHT22
#define DHTPIN  14        // Digital pin we're connected to, on Wemos D1 it's pin D5
DHT dht(DHTPIN, DHTTYPE); // Initialize DHT sensor.
float temperature;
float humidity;

// -------------- HV5530 ---------------
#define PIN_HV5530_CLOCK     (4)
#define PIN_HV5530_LATCH     (0)
#define PIN_HV5530_DATA      (5)
#define PIN_HV5530_BLANKING  (2)
#define NR_HV5530s           (2)
#define NR_HV5530_PINS       (32 * NR_HV5530s)
#define NIXIE_ON             (true)
#define NIXIE_OFF            (false)

// -------------- Accesspoint ---------------
IPAddress accesspointIp(192, 168, 1, 1);
DNSServer dnsServer;

// -------------- Webserver ---------------
const char* hostname = "luminixie";
#define WEBSERVER_PORT (80)
ESP8266WebServer webserver(WEBSERVER_PORT);

// -------------- Connection / WiFi --------------
#define CONNECTION_MODE_AP   (0) 
#define CONNECTION_MODE_STA  (1)
boolean connectedToWifiNetwork = false;
WiFiEventHandler connectedEventHandler, gotIpEventHandler, disconnectedEventHandler;

// -------------- Date/time ---------------
Timezone dateAndTime = nullptr;

// -------------- Config ---------------
struct Config {
  byte connectionMode; // 0 (CONNECTION_MODE_AP), 1 (CONNECTION_MODE_STA)
  String wifiSsid;
  String wifiPassword;

  String timezoneLocation; // See https://en.wikipedia.org/wiki/List_of_tz_database_time_zones (TZ database name)
  boolean showDate; // When set to true, the current date is shown every minute on the nixies on seconds 10, 11 and 12

  boolean showTemperature; // When set to true, the current temperature is shown every minute on the nixies on seconds 30, 31 and 32
  boolean showHumidity; // When set to true, the current humidity is shown every minute on the nixies on seconds 50, 51 and 52

  byte ledBrightness; // Range 0-255
  byte ledColorR;
  byte ledColorG;
  byte ledColorB;
};

const char* configFileFullPath = "/config.json";
Config config; // <- global configuration values

// -------------- HV5530 / Nixie tubes ---------------
byte pinIndex[6][10] = { // Mapping table to find the HV5530 pinnumber for a digit on a tube
  // Tube 0 (0 - 9) (most right tube)
  { 0, 9, 8, 7, 6, 5, 4, 3, 2, 1 },
  // Tube 1 (0 - 9)
  { 10, 19, 18, 17, 16, 15, 14, 13, 12, 11 },
  // Tube 2 (0 - 9)
  { 21, 30, 29, 28, 27, 26, 25, 24, 23, 22 },
  // Tube 3 (0 - 9)
  { 32, 41, 40, 39, 38, 37, 36, 35, 34, 33 },
  // Tube 4 (0 - 9)
  { 43, 52, 51, 50, 49, 48, 47, 46, 45, 44 },
  // Tube 5 (0 - 9) (most left tube)
  { 53, 62, 61, 60, 59, 58, 57, 56, 55, 54 }
};

#define RIGHT_DOT_PIN (20)
#define LEFT_DOT_PIN  (42)

boolean turnOffTimeDots = false;
unsigned long turnOffTimeDotsAt;

boolean restart = false;
unsigned long restartAt;

struct bcmPin_t {
  bool value = NIXIE_OFF;
};
bcmPin_t bcmPins[NR_HV5530_PINS];

void setup() {
  setupSerial();
  setupFileSystem();
  loadConfigurationFromFile();
  setupLeds();
  setupNixies();
  setupTemperatureAndHumiditySensor();
  setupNetwork();
  setupDateTimeSync();
}

void setupSerial() {
  Serial.begin(115200);
  while (!Serial) continue;
}

void setupFileSystem() {
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
  }
}

void setupLeds() {
  FastLED.addLeds<APA106, PIN_LED_DATA>(leds, NR_LEDS);
  applyLedConfiguration();
}

void setupNixies() {
  pinMode(PIN_HV5530_CLOCK, OUTPUT);
  pinMode(PIN_HV5530_DATA, OUTPUT);
  pinMode(PIN_HV5530_LATCH, OUTPUT);
  pinMode(PIN_HV5530_BLANKING, OUTPUT);

  digitalWrite(PIN_HV5530_BLANKING, HIGH);
  allTubesOff();
  digitalWrite(PIN_HV5530_BLANKING, HIGH);
  writerHV5530();  
}

void setupTemperatureAndHumiditySensor() {
  dht.begin();
}

void setupNetwork() {
  connectedEventHandler = WiFi.onStationModeConnected(onStationModeConnected);
  disconnectedEventHandler = WiFi.onStationModeDisconnected(onStationModeDisconnected);
  gotIpEventHandler = WiFi.onStationModeGotIP(onStationModeGotIP);

  if (config.connectionMode == CONNECTION_MODE_STA) {
    connectToWifi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Failed to connect in STA mode. Switch to AP mode");
      config.connectionMode = CONNECTION_MODE_AP;
      saveConfigurationToFile();
      setupAccessPoint();
    }
  } else {
    setupAccessPoint();
  }
  startWebserver();
  
  Serial.print("hostname: ");
  Serial.println(hostname);
}

void setupDateTimeSync() {
  dateAndTime.setLocation(config.timezoneLocation);

  if (config.connectionMode == CONNECTION_MODE_AP) {
    Serial.println("Setting fixed time and date");
    setInterval(); // Instruct eztime to do not query NTP
    setTime(0, 0, 0, 1, 1, 2000);
    
  } else if (connectedToWifiNetwork) {
    Serial.println("Syncing time and date from NTP");
    updateNTP();
    waitForSync();
  }  
}

void loop() {
  if (restart && millis() >= restartAt) {
    ESP.restart();
  }
  if (turnOffTimeDots && millis() >= turnOffTimeDotsAt) {
    turnOffTimeDots = false;
    bcmPins[RIGHT_DOT_PIN].value = NIXIE_OFF;
    bcmPins[LEFT_DOT_PIN].value = NIXIE_OFF;
    writerHV5530();
  }

  events(); // Process date/time events

  if (secondChanged()) {
    printCurrentTimeAndDate();

    if (dateAndTime.second() == 0 && dateAndTime.minute() % 5 == 0) {
      runAntiCathodePoisoningSequence2(1);
    }

    if (config.showTemperature) {
      temperature = dht.readTemperature();
      printTemperature();
    }
    if (config.showHumidity) {
      humidity = dht.readHumidity();
      printHumidity();
    }

    if (config.showDate && (dateAndTime.second() == 10 || dateAndTime.second() == 11 || dateAndTime.second() == 12)) {
      setDateOnTubes(dateAndTime.day(), dateAndTime.month(), dateAndTime.year() % 1000);
    } else if (config.showTemperature && !isnan(temperature) && (dateAndTime.second() == 30 || dateAndTime.second() == 31 || dateAndTime.second() == 32)) {
      setTemperatureOnTubes(temperature);
    } else if (config.showHumidity && !isnan(humidity) && (dateAndTime.second() == 50 || dateAndTime.second() == 51 || dateAndTime.second() == 52)) {
      setHumidityOnTubes(humidity);
    } else {
      setTimeOnTubes(dateAndTime.hour(), dateAndTime.minute(), dateAndTime.second());
    }
  }
    
  dnsServer.processNextRequest();
  webserver.handleClient();
}

void printTemperature() {
  if (isnan(temperature)) {
    Serial.println("Failed to read temperature from DHT sensor!");
  } else {
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" *C ");    
  }
}

void printHumidity() {
  if (isnan(humidity)) {
    Serial.println("Failed to read humidity from DHT sensor!");
  } else {
    Serial.print("Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");
  }
}

void printCurrentTimeAndDate() {
  Serial.println(dateAndTime.dateTime("d-m-y H:i:s.v T (e)"));  
}

// Assumes that parameter "value" is less than 100.00.
// Temperature is shown on different tubes than Humidity to have more "even" wear on the tubes
void setTemperatureOnTubes(float value) {
  int integerPartOfValue = (int)(value);
  int decimalPartOfValue = getDecimals(value, 1);
  
  setTubes(
    -1,
    -1,
    false,
    integerPartOfValue >= 10 ? extractDigit(integerPartOfValue, 2) : -1,
    extractDigit(integerPartOfValue, 1),
    true,
    extractDigit(decimalPartOfValue, 1),
    -1    
  );
}

// Assumes that parameter "value" is less than 100.00.
// Temperature is shown on different tubes than Humidity to have more "even" wear on the tubes
void setHumidityOnTubes(float value) {
  int integerPartOfValue = (int)(value);
  int decimalPartOfValue = getDecimals(value, 1);
  
  setTubes(
    integerPartOfValue >= 10 ? extractDigit(integerPartOfValue, 2) : -1,
    extractDigit(integerPartOfValue, 1),
    true,
    extractDigit(decimalPartOfValue, 1),
    -1,
    false,
    -1,
    -1
  );
}

int getDecimals(float value, int numberOfDecimals) {
  String valueString = String(value, numberOfDecimals);
  int indexOfDot = valueString.lastIndexOf(".");
  String decimalsString = valueString.substring(indexOfDot + 1);
  return decimalsString.toInt();
}

// Each "item" will be displayed with leading zero's.
// This method assumes the given parameter values are less than 100.
void setTimeOnTubes(int hours, int minutes, int seconds) {
  setTubes(
    hours < 10 ? 0 : extractDigit(hours, 2),
    extractDigit(hours, 1),
    false,
    minutes < 10 ? 0 : extractDigit(minutes, 2),
    extractDigit(minutes, 1),
    false,
    seconds < 10 ? 0 : extractDigit(seconds, 2),
    extractDigit(seconds, 1)
  );

  turnOffTimeDotsAt = millis() + 500;
  turnOffTimeDots = true;
}

// Each "item" will be displayed with leading zero's.
// This method assumes the given parameter values are less than 100.
void setDateOnTubes(int day, int month, int year) {
  setTubes(
    day < 10 ? 0 : extractDigit(day, 2),
    extractDigit(day, 1),
    false,
    month < 10 ? 0 : extractDigit(month, 2),
    extractDigit(month, 1),
    false,
    year < 10 ? 0 : extractDigit(year, 2),
    extractDigit(year, 1)
  );
}

// Returns a single digit in a given number.
// Examples:
// extractDigit(8135, 1) -> returns 5
// extractDigit(8135, 2) -> returns 3
// extractDigit(8135, 3) -> returns 1
// extractDigit(8135, 4) -> returns 8
int extractDigit(int number, int pos) {
  return int(number/(pow(10,pos-1))) - int(number/(pow(10,pos)))*10;
}

// Set the given tube to the given digit.
// The parameter "tube" van have the folowing values: 0, 1, 2, 3, 4, 5.
// 0 is the most right tube, 5 is the most left tube (as seen from the front).
void setTube(int tube, int digit) {
  byte pin = pinIndex[tube][digit];
  if (digit >= 0) bcmPins[pin].value = NIXIE_ON;
}

void allTubesOff() {
  bcmPin_t *p = &bcmPins[0];
  for (uint8_t i = 0; i < NR_HV5530_PINS; i++) {
    p++->value = NIXIE_OFF;   // all pins off.
  }
}

// 00 00 00
// 11 11 11
// 22 22 22
// 33 33 33
// etc.
void runAntiCathodePoisoningSequence1() {
  for (int i=0; i<=9; i++) {
    setTubes(i, i, false, i, i, false, i, i);
    delay(100);
  }
}

void runAntiCathodePoisoningSequence1(int numberOfRuns) {
  for (int i=0; i<numberOfRuns; i++) {
    runAntiCathodePoisoningSequence1();
  }
}

// 12 34 56
// 23 45 67
// 34 56 78
// etc.
void runAntiCathodePoisoningSequence2() {
  int n[6] = {
    1, 2, 3, 4, 5, 6
  };
  
  for (int i=0; i<=9; i++) {
    setTubes(n[0], n[1], false, n[2], n[3], false, n[4], n[5]);
    delay(100);
    
    for (int j=0; j<6; j++) {
      int next = n[j] + 1;
      if (next >= 10) next -= 10;
      n[j] = next;
    }
  }
}

void runAntiCathodePoisoningSequence2(int numberOfRuns) {
  for (int i=0; i<numberOfRuns; i++) {
    runAntiCathodePoisoningSequence2();
  }
}

void setTubes(int n5, int n4, boolean leftDot, int n3, int n2, boolean rightDot, int n1, int n0) {
  allTubesOff();
  setTube(5, n5);
  setTube(4, n4);
  if (leftDot) bcmPins[LEFT_DOT_PIN].value = NIXIE_ON;
  setTube(3, n3);
  setTube(2, n2);
  if (rightDot) bcmPins[RIGHT_DOT_PIN].value = NIXIE_ON;
  setTube(1, n1);
  setTube(0, n0);
  writerHV5530();
}

void writerHV5530(void) {
  bcmPin_t *p = &bcmPins[NR_HV5530_PINS-1];  // MSB first
  
  digitalWrite(PIN_HV5530_LATCH, LOW);  // load data
  for (uint8_t i = 0; i < NR_HV5530_PINS; i++) {
    delayMicroseconds(1);
    digitalWrite(PIN_HV5530_DATA, (p->value == NIXIE_ON) ? HIGH : LOW);
    digitalWrite(PIN_HV5530_CLOCK, HIGH);
    // try go for a lower delay or remove it all together
    delayMicroseconds(10);
    digitalWrite(PIN_HV5530_CLOCK, LOW);
    p--;
  }
  digitalWrite(PIN_HV5530_LATCH, HIGH);  // store data     
}

void setAllLedsToSameColor(CRGB color) {
  fill_solid(leds, NR_LEDS, color);
  FastLED.show();
}

void onStationModeConnected(const WiFiEventStationModeConnected& event) {
  Serial.println("Connected to accesspoint, wating for ip");
}

void onStationModeGotIP(const WiFiEventStationModeGotIP& event) {
  Serial.print("Sucessfully obtained ip from ");
  Serial.println(WiFi.SSID());
  Serial.print("local-ip: ");
  Serial.println(WiFi.localIP());  

  // startWebserver();
  connectedToWifiNetwork = true;
}

void onStationModeDisconnected(const WiFiEventStationModeDisconnected& event) {
  Serial.println("Lost WiFi connection!");
  connectedToWifiNetwork = false;
  // TODO: reconnect / error mode / ...
}

void processConfigurationRequest() {
  if (webserver.method() == HTTP_GET) {
    sendConfigurationFromWebserver();
  } else if (webserver.method() == HTTP_POST) {
    saveConfigurationSentToWebserver();
    applyLedConfiguration();
  }
}

void sendConfigurationFromWebserver() {
  DynamicJsonDocument jsonDoc = getConfigObjectAsJsonDocument();
  webserver.send(200, "application/json", jsonDoc.as<String>());
}

void saveConfigurationSentToWebserver() {
  Serial.println("Save configuration sent to webserver");
  
  String previousWifiSsid = config.wifiSsid;
  String previousWifiPwd = config.wifiPassword;
  byte previousConnectionMode = config.connectionMode;

  DynamicJsonDocument configurationJson(1024);
  
  DeserializationError error = deserializeJson(configurationJson, webserver.arg("plain"));
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    webserver.send(500, "application/json", "{ \"message\": \"Failed to save settings\" }");
    return;
  }

  writeConfigurationJsonToFile(configurationJson);

  loadConfigurationFromFile();
 
  if (previousWifiSsid != config.wifiSsid || previousWifiPwd != config.wifiPassword || previousConnectionMode != config.connectionMode) {
    webserver.send(200, "application/json", "{ \"message\": \"Restarting to apply the updated connection settings\" }");

    restart = true;
    restartAt = millis() + 500;
    return;
  }
  
  sendConfigurationFromWebserver();

  int yearPresent = configurationJson["year"] | -1;
  if (config.connectionMode == CONNECTION_MODE_AP && yearPresent != -1) {
    Serial.println("set date/time");
    int year = configurationJson["year"];
    int month = configurationJson["month"];
    int day = configurationJson["day"];
    int hour = configurationJson["hour"];
    int minute = configurationJson["minute"];
    int second = configurationJson["second"];
    setTime(hour, minute, second, day, month, year);
  }
}

void applyLedConfiguration() {
  FastLED.setBrightness(config.ledBrightness);
  setAllLedsToSameColor(CRGB(config.ledColorR, config.ledColorG, config.ledColorB));  
}

void startWebserver() {
  webserver.on("/", processRootRequest);
  webserver.on("/configuration", processConfigurationRequest);
  webserver.onNotFound(handleNotFound);
  webserver.begin();
  Serial.println("Webserver started");
}

void processRootRequest() {
  Serial.println("handleRoot");
  redirect("/index.html");
}

void redirect(String toUrl) {
  webserver.sendHeader("Location", toUrl, true);
  webserver.send(302, "text/plain", "");
}

void handleNotFound() {
  if (webserver.method() == HTTP_GET) {
    if (handleFileRead(webserver.uri())) {
      return;
    }
  }
  String message = "Not Found\n\n";
  message += "URI: ";
  message += webserver.uri();
  message += "\nMethod: ";
  message += (webserver.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += webserver.args();
  message += "\n";
  for (int i=0; i<webserver.args(); i++){
    message += " " + webserver.argName(i) + ": " + webserver.arg(i) + "\n";
  }
  webserver.send(404, "text/plain", message);
}

bool handleFileRead(String path) {
  Serial.println("handleFileRead: " + path);

  webserver.sendHeader("Cache-Control","public, max-age=2628000");
  
  String contentType = getContentType(path);
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    size_t sent = webserver.streamFile(file, contentType);
    file.close();
    return true;
  }
  Serial.println("File Not Found!");
  return false;
}

String getContentType(String filename){
  if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".js")) return "text/javascript";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

void setupAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(accesspointIp, accesspointIp, IPAddress(255, 255, 255, 0));

  // Please note that when the password is too short (less than 8 characters) the WiFi.softAP(ssid, password) function doesn't work.
  // There is no warning during compilation for this.
  const char* accesspointPassword = "luminixie";

  Serial.println();

  String ssid = "luminixie-" + String(ESP.getChipId());
  const char* accesspointSsid = ssid.c_str();
  
  boolean result = WiFi.softAP(accesspointSsid, accesspointPassword);
  result ? Serial.println("AccessPoint ready") : Serial.println("Failed to start AccessPoint");
  
  // if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request
  const byte dnsPort = 53;
  dnsServer.start(dnsPort, "*", accesspointIp);

  Serial.print("Accesspoint SSID: ");
  Serial.println(accesspointSsid);

  Serial.print("Accesspoint IP: ");
  Serial.println(WiFi.softAPIP());
}

void connectToWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname);
  WiFi.begin(config.wifiSsid, config.wifiPassword);

  // Wait at most 15 seconds to connect
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    Serial.write('.');
    runAntiCathodePoisoningSequence2(1);
  }
  setTubes(-1, -1, false, -1, -1, false, -1, -1);
}

DynamicJsonDocument readConfigurationFileToJsonDocument() {
  File file = SPIFFS.open(configFileFullPath, "r");

  DynamicJsonDocument doc(1024);

  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("Failed to read file");
  }
  file.close();

  return doc;
}

void loadConfigurationFromFile() {
  DynamicJsonDocument jsonDoc = readConfigurationFileToJsonDocument();

  config.connectionMode = jsonDoc["connectionMode"] | CONNECTION_MODE_STA;
  config.wifiSsid = jsonDoc["wifiSsid"] | "default";
  config.wifiPassword = jsonDoc["wifiPassword"] | "default";
  config.ledBrightness = jsonDoc["ledBrightness"] | 25;
  config.ledColorR = jsonDoc["ledColorR"] | 0;
  config.ledColorG = jsonDoc["ledColorG"] | 0;
  config.ledColorB = jsonDoc["ledColorB"] | 255;
  config.timezoneLocation = jsonDoc["timezoneLocation"] | "Europe/Amsterdam";
  config.showDate = jsonDoc["showDate"] | true;
  config.showTemperature = jsonDoc["showTemperature"] | true;
  config.showHumidity = jsonDoc["showHumidity"] | true;

  Serial.println("Current configuration: ");
  serializeJsonPretty(jsonDoc, Serial);
}

DynamicJsonDocument getConfigObjectAsJsonDocument() {
  DynamicJsonDocument jsonDoc(1024);

  jsonDoc["connectionMode"] = config.connectionMode;
  jsonDoc["wifiSsid"] = config.wifiSsid;
  jsonDoc["wifiPassword"] = config.wifiPassword;
  jsonDoc["ledBrightness"] = config.ledBrightness;
  jsonDoc["ledColorR"] = config.ledColorR;
  jsonDoc["ledColorG"] = config.ledColorG;
  jsonDoc["ledColorB"] = config.ledColorB;
  jsonDoc["timezoneLocation"] = config.timezoneLocation;
  jsonDoc["showDate"] = config.showDate;
  jsonDoc["showTemperature"] = config.showTemperature;
  jsonDoc["showHumidity"] = config.showHumidity;

  return jsonDoc;
}

void saveConfigurationToFile() {
  DynamicJsonDocument jsonDoc = getConfigObjectAsJsonDocument();
  writeConfigurationJsonToFile(jsonDoc);  
}

void writeConfigurationJsonToFile(DynamicJsonDocument configurationJson) {
  Serial.println("Saving configuration to file");
  
  File configFile = SPIFFS.open(configFileFullPath, "w");
  if (!configFile) {
    Serial.print("Failed to open ");
    Serial.print(configFileFullPath);
    Serial.println(" for writing");
  }

  if (serializeJson(configurationJson, configFile) == 0) {
    Serial.println("Failed to write configuration to file");
  }
  configFile.close();

  Serial.println("Configuration written to file: ");
  serializeJsonPretty(configurationJson, Serial);
}
