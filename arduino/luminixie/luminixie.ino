#include <ESP8266WiFi.h>          // See https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/readme.html
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <FastLED.h>              // See http://fastled.io/
#include <ezTime.h>               // See https://github.com/ropg/ezTime
#include <FS.h>                   // See https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html
#include <ArduinoJson.h>          // See https://arduinojson.org/

// -------------- LED's ---------------
#define NR_LEDS       (6)
#define PIN_LED_DATA  (12)
CRGB leds[NR_LEDS];
byte previousLedBrightness;

// -------------- HV5530 ---------------
#define PIN_HV5530_CLOCK     (4)
#define PIN_HV5530_LATCH     (0)
#define PIN_HV5530_DATA      (5)
#define PIN_HV5530_BLANKING  (2)
#define NR_HV5530s           (2)
#define NR_HV5530_PINS       (32 * NR_HV5530s)
#define DIGIT_ON             (true)
#define DIGIT_OFF            (false)

// -------------- Accesspoint ---------------
IPAddress accesspointIp(192, 168, 1, 1);
DNSServer dnsServer;

// -------------- Webserver ---------------
const char* hostname = "luminixie";
#define WEBSERVER_PORT (80)
ESP8266WebServer webserver(WEBSERVER_PORT);

// -------------- Connection / WiFi --------------
#define CONNECTION_MODE_AP 0 
#define CONNECTION_MODE_STA 1
boolean connectedToWifiNetwork = false;
WiFiEventHandler connectedEventHandler, gotIpEventHandler, disconnectedEventHandler;

// -------------- Date/time ---------------
Timezone dateAndTime = nullptr;

// -------------- Config ---------------
struct Config {
  byte ledBrightness; // Range 0-255

  byte connectionMode; // 0 (CONNECTION_MODE_AP), 1 (CONNECTION_MODE_STA)
  String wifiSsid;
  String wifiPassword;

  String timezoneLocation;
  
  String ledColorHex;
  byte ledColorR;
  byte ledColorG;
  byte ledColorB;  
};

const char* configFileFullPath = "/config.json";
Config config; // <- global configuration values

// -------------- HV5530 / Nixies ---------------
byte pinIndex[6][10] = {
  // Tube 1 (0 - 9) (most right tube)
  { 0, 9, 8, 7, 6, 5, 4, 3, 2, 1 },
  // Tube 2 (0 - 9)
  { 10, 19, 18, 17, 16, 15, 14, 13, 12, 11 },
  // Tube 3 (0 - 9)
  { 21, 30, 29, 28, 27, 26, 25, 24, 23, 22 },
  // Tube 4 (0 - 9)
  { 32, 41, 40, 39, 38, 37, 36, 35, 34, 33 },
  // Tube 5 (0 - 9)
  { 43, 52, 51, 50, 49, 48, 47, 46, 45, 44 },
  // Tube 6 (0 - 9) (most left tube)
  { 53, 62, 61, 60, 59, 58, 57, 56, 55, 54 }
};

struct bcmPin_t {
  bool value = DIGIT_OFF;
};
bcmPin_t bcmPins[NR_HV5530_PINS];

void setup() {
  setupSerial();
  setupFileSystem();
  loadConfigurationFromFile();
  setupLeds();
  setupNixies(); 
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
    setTime(0, 0, 0, 1, 1, 2000);
    
  } else if (connectedToWifiNetwork) {
    Serial.println("Syncing time and date from NTP");
    updateNTP();
    waitForSync();
  }  
}

void loop() {
  events(); // Process date/time events

  if (secondChanged()) {
    if (dateAndTime.second() == 0 && dateAndTime.minute() % 5 == 0) {
      runAntiCathodePoisoningSequence(1);
    }
    setTimeOnTubes(dateAndTime.hour(), dateAndTime.minute(), dateAndTime.second());
    printCurrentTimeAndDate();
  }

  dnsServer.processNextRequest();
  webserver.handleClient();
}

void printCurrentTimeAndDate() {
  Serial.print(dateAndTime.hour());
  Serial.print(":");
  Serial.print(dateAndTime.minute());
  Serial.print(":");
  Serial.print(dateAndTime.second());
  Serial.print(" ");
  Serial.print(dateAndTime.day());
  Serial.print("-");
  Serial.print(dateAndTime.month());
  Serial.print("-");
  Serial.println(dateAndTime.year());
}

void setTimeOnTubes(byte hours, byte minutes, byte seconds) {
  allTubesOff();
  setHoursTubes(hours);
  setMinutesTubes(minutes);
  setSecondsTubes(seconds);
  writerHV5530();
}

void setSecondsTubes(byte seconds) {
  setTube(0, extractDigit(seconds, 1));
  setTube(1, seconds < 10 ? 0 : extractDigit(seconds, 2)); 
}

void setMinutesTubes(byte seconds) {
  setTube(2, extractDigit(seconds, 1));
  setTube(3, seconds < 10 ? 0 : extractDigit(seconds, 2)); 
}

void setHoursTubes(byte seconds) {
  setTube(4, extractDigit(seconds, 1));
  setTube(5, seconds < 10 ? 0 : extractDigit(seconds, 2)); 
}

int extractDigit(int number, int pos) {
  return int(number/(pow(10,pos-1))) - int(number/(pow(10,pos)))*10;
}

void setTube(byte tube, byte digit) {
  byte pin = pinIndex[tube][digit];
  bcmPins[pin].value = DIGIT_ON;
}

void allTubesOff() {
  bcmPin_t *p = &bcmPins[0];
  for (uint8_t i = 0; i < NR_HV5530_PINS; i++) {
    p++->value = DIGIT_OFF;   // all pins off.
  }
}

void runAntiCathodePoisoningSequence(int numberOfRuns) {
  for (int i=0; i<numberOfRuns; i++) {
    for (int j=0; j<=9; j++) {
      setAllTubes(j);
      delay(100);
    }
  }
}

void setAllTubes(byte digit) {
  allTubesOff();
  for (int i=0; i<=5; i++) {
    setTube(i, digit);
  }
  writerHV5530();
}

void writerHV5530(void) {
  bcmPin_t *p = &bcmPins[NR_HV5530_PINS-1];  // MSB first
  
  digitalWrite(PIN_HV5530_LATCH, LOW);  // load data
  for (uint8_t i = 0; i < NR_HV5530_PINS; i++) {
    delayMicroseconds(1);
    digitalWrite(PIN_HV5530_DATA, (p->value == DIGIT_ON) ? HIGH : LOW);
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
    return;
  }

  writeConfigurationJsonToFile(configurationJson);

  loadConfigurationFromFile();
 
  if (previousWifiSsid != config.wifiSsid || previousWifiPwd != config.wifiPassword || previousConnectionMode != config.connectionMode) {
    webserver.send(200, "application/json", "Restarting to apply new connection settings");
    ESP.restart();
  } else {
    webserver.send(200, "application/json", "Saved successfully");
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
  redirect("/configuration");
}

void handleNotFound() {
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

void redirect(String toUrl) {
  webserver.sendHeader("Location", toUrl, true);
  webserver.send(302, "text/plain", "");
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
    runAntiCathodePoisoningSequence(1);
  }
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
  config.ledColorHex = jsonDoc["ledColorHex"] | "#0000FF";
  config.ledColorR = jsonDoc["ledColorR"] | 0;
  config.ledColorG = jsonDoc["ledColorG"] | 0;
  config.ledColorB = jsonDoc["ledColorB"] | 255;
  config.timezoneLocation = jsonDoc["timezoneLocation"] | "Europe/Amsterdam";

  Serial.println("Current configuration: ");
  serializeJsonPretty(jsonDoc, Serial);
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
    Serial.println("Failed to write to file");
  }
  configFile.close();

  Serial.println("Configuration written to file: ");
  serializeJsonPretty(configurationJson, Serial);
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
  jsonDoc["ledColorHex"] = config.ledColorHex;
  jsonDoc["timezoneLocation"] = config.timezoneLocation;

  return jsonDoc;
}
