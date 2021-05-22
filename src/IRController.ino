#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>                                      // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>

#include <ArduinoJson.h>
#include "sha256.h"

#include <Ticker.h>                                           // For LED status
#include <TimeLib.h>

#include <LittleFS.h>

// User settings are below here
//+=============================================================================
#define enabledMQTT 1                                           // Enable MQTT; this disables lots of other code as the MQTT client is very memory intensive

const uint16 packetSize = 2048;

const bool getExternalIP = true;                               // Set to false to disable querying external IP

const bool getTime = true;                                     // Set to false to disable querying for the time
const int timeZone = -5;                                       // Timezone (-5 is EST)

const bool enableMDNSServices = true;                          // Use mDNS services, must be enabled for ArduinoOTA

const bool bypassLocalAuth = true;                             // Allow local traffic to bypass HMAC check

const unsigned int captureBufSize = 1024;                      // Size of the IR capture buffer.

const bool toggleRC = true;                                    // Toggle RC signals every other transmission

#if defined(ARDUINO_ESP8266_WEMOS_D1R1) || defined(ARDUINO_ESP8266_WEMOS_D1MINI) || defined(ARDUINO_ESP8266_WEMOS_D1MINIPRO) || defined(ARDUINO_ESP8266_WEMOS_D1MINILITE)
const uint16_t  pinr1 = D5;                                          // Receiving pin (GPIO14)
const uint16_t  pins1 = D6;                                          // Transmitting preset 1 (GPIO12)
const uint16_t  configpin = D2;                                      // Reset Pin (GPIO4)
const uint16_t  pins2 = 5;                                           // Transmitting preset 2
const uint16_t  pins3 = 12;                                          // Transmitting preset 3
const uint16_t  pins4 = 13;                                          // Transmitting preset 4
#else
const uint16_t  pinr1 = 14;                                          // Receiving pin
const uint16_t  pins1 = 4;                                           // Transmitting preset 1
const uint16_t  configpin = 10;                                      // Reset Pin
const uint16_t  pins2 = 5;                                           // Transmitting preset 2
const uint16_t  pins3 = 12;                                          // Transmitting preset 3
const uint16_t  pins4 = 13;                                          // Transmitting preset 4
#endif
//+=============================================================================
// User settings are above here

#if enabledMQTT == 1
#include <PubSubClient.h>
WiFiClientSecure secureClient;
PubSubClient mqtt_client(secureClient);
#else
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#endif

const int ledpin = LED_BUILTIN;                                // Built in LED defined for WEMOS people
const char *wifi_config_name = "IR Controller Configuration";
const char serverName[] = "checkip.dyndns.org";
int port = 80;
int mqtt_port = 8883;
char passcode[20] = "";
char host_name[20] = "";
char port_str[6] = "80";
char user_id[60] = "";

// MQTT settings
char mqtt_host[100] = "b-397770a4-fa3e-4e01-9951-cc3a556005aa-1.mq.us-east-1.amazonaws.com";
char mqtt_port_str[6] = "8883";
char mqtt_user[20] = "public";
char mqtt_pass[20] = "publicaccess";

// Do not modify these values with your own, they are placeholder values that WiFiManager will overwrite
char static_ip[16] = "10.0.1.10";
char static_gw[16] = "10.0.1.1";
char static_sn[16] = "255.255.255.0";
char static_dns[16] = "10.0.1.1";

Ticker ticker;
HTTPClient http;
WiFiClient client;
ESP8266WebServer *server = NULL;

long mqtt_lastReconnectAttempt = 0;

DynamicJsonDocument deviceState(256);

bool shouldSaveConfig = false;                                 // Flag for saving data
bool holdReceive = false;                                      // Flag to prevent IR receiving while transmitting

IRrecv irrecv(pinr1, captureBufSize, 35);
IRsend irsend1(pins1);
IRsend irsend2(pins2);
IRsend irsend3(pins3);
IRsend irsend4(pins4);

const unsigned long resetfrequency = 259200000;                // 72 hours in milliseconds for external IP reset
static const char ntpServerName[] = "time.google.com";
unsigned int localPort = 8888;                                 // Local port to listen for UDP packets
WiFiUDP ntpUDP;

bool _rc5toggle = false;
bool _rc6toggle = false;

char _ip[16] = "";

unsigned long lastupdate = 0;

bool authError = false;
time_t timeAuthError = 0;
bool externalIPError = false;
bool userIDError = false;
bool ntpError = false;

class Code {
  public:
    char encoding[14] = "";
    char address[20] = "";
    char command[40] = "";
    char data[40] = "";
    String raw = "";
    int bits = 0;
    time_t timestamp = 0;
    bool valid = false;
};

// Declare prototypes
void sendCodePage(Code selCode);
void sendCodePage(Code selCode, int httpcode);
void cvrtCode(Code& codeData, decode_results *results);
void copyCode (Code& c1, Code& c2);

Code last_recv;
Code last_recv_2;
Code last_recv_3;
Code last_recv_4;
Code last_recv_5;
Code last_send;
Code last_send_2;
Code last_send_3;
Code last_send_4;
Code last_send_5;

//+=============================================================================
// Callback notifying us of the need to save config
//
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}


//+=============================================================================
// Reenable IR receiving
//
void resetReceive() {
  if (holdReceive) {
    Serial.println("Reenabling receiving");
    irrecv.resume();
    holdReceive = false;
  }
}


//+=============================================================================
// Valid user_id formatting
//
bool validUID(char* user_id) {
  if (!String(user_id).startsWith("amzn1.account.")) {
      Serial.println("Warning, user_id appears to be in the wrong format, security check will most likely fail. Should start with amzn1.account.***");
      return false;
    }
    return true;
}


//+=============================================================================
// Valid EPOCH time retrieval
//
bool validEPOCH(time_t timenow) {
  if (timenow < 922838400) {
    Serial.println("Epoch time from timeServer is unexpectedly old, probably failed connection to the time server. Check your network settings");
    Serial.println(timenow);
    return false;
  }
  return true;
}


//+=============================================================================
// EPOCH time to String
//
String epochToString(time_t timenow) {
  unsigned long hours = (timenow % 86400L) / 3600;
  String hourStr = hours < 10 ? "0" + String(hours) : String(hours);

  unsigned long minutes = (timenow % 3600) / 60;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  unsigned long seconds = (timenow % 60);
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);
  return hourStr + ":" + minuteStr + ":" + secondStr;
}

//+=============================================================================
// Valid command request using HMAC
//
bool validateHMAC(String epid, String mid, String timestamp, String signature, IPAddress clientIP) {
    userIDError = false;
    authError = false;
    ntpError = false;
    timeAuthError = 0;

    if (allowLocalBypass(clientIP)) {
      Serial.println("Bypassing HMAC security as this is a local network request");
      return true;
    }

    userIDError = !(validUID(user_id));

    time_t timethen = timestamp.toInt();
    time_t timenow = now() - (timeZone * SECS_PER_HOUR);
    time_t timediff = abs(timethen - timenow);
    if (timediff > 30) {
      Serial.println("Failed security check, signature is too old");
      Serial.print("Server: ");
      Serial.println(timethen);
      Serial.print("Local: ");
      Serial.println(timenow);
      Serial.print("MID: ");
      Serial.println(mid);
      timeAuthError = timediff;
      validEPOCH(timenow);
      return false;
    }

    uint8_t *hash;
    String key = String(user_id);
    Sha256.initHmac((uint8_t*)key.c_str(), key.length()); // key, and length of key in bytes
    Sha256.print(epid);
    Sha256.print(mid);
    Sha256.print(timestamp);
    hash = Sha256.resultHmac();
    String computedSignature = bin2hex(hash, HASH_LENGTH);

    if (computedSignature != signature) {
      Serial.println("Failed security check, signatures do not match");
      Serial.print("1: ");
      Serial.println(signature);
      Serial.print("2: ");
      Serial.println(computedSignature);
      Serial.print("MID: ");
      Serial.println(mid);
      authError = true;
      return false;
    }

    Serial.println("Passed security check");
    Serial.print("MID: ");
    Serial.println(mid);
    return true;
}

//+=============================================================================
// Check if client request is coming from a local IP address
//
bool isInSubnet(IPAddress address) {
    Serial.print("Client IP: ");
    Serial.println(address.toString());

    uint32_t mask = uint32_t(WiFi.subnetMask());

    /*
    uint32_t net_lower = (uint32_t(WiFi.localIP()) & mask);
    uint32_t net_upper = (net_lower | (~mask));
    Serial.println(IPAddress(net_lower).toString());
    Serial.println(IPAddress(net_upper).toString());
    */

    return ((uint32_t(address) & mask) == (uint32_t(WiFi.localIP()) & mask));
}

//+=============================================================================
// Allow local traffic to bypass security
//
bool allowLocalBypass(IPAddress clientIP) {
  return (bypassLocalAuth && isInSubnet(clientIP));
}

//+=============================================================================
// Passcode valid check
//
bool isPasscodeValid(String pass) {
  return ((strlen(passcode) == 0) || (pass == passcode));
}


//+=============================================================================
// Toggle state
//
void tick()
{
  int state = digitalRead(ledpin);  // get the current state of GPIO1 pin
  digitalWrite(ledpin, !state);     // set pin to the opposite state
}


//+=============================================================================
// Get External IP Address
//
String externalIP()
{
  if (!getExternalIP) {
    return "0.0.0.0"; // User doesn't want the external IP
  }

  if (strlen(_ip) > 0) {
    unsigned long delta = millis() - lastupdate;
    if (delta > resetfrequency || lastupdate == 0) {
      Serial.println("Reseting cached external IP address");
      strncpy(_ip, "", 16); // Reset the cached external IP every 72 hours
    } else {
      return String(_ip); // Return the cached external IP
    }
  }

  externalIPError = false;
  unsigned long start = millis();
  http.setTimeout(5000);
  http.begin(client, serverName, 8245);
  int httpCode = http.GET();

  if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int pos_start = payload.indexOf("IP Address") + 12; // add 10 for "IP Address" and 2 for ":" + "space"
    int pos_end = payload.indexOf("</body>", pos_start); // add nothing
    strncpy(_ip, payload.substring(pos_start, pos_end).c_str(), 16);
    Serial.print(F("External IP: "));
    Serial.println(_ip);
    lastupdate = millis();
  } else {
    Serial.println("Error retrieving external IP");
    Serial.print("HTTP Code: ");
    Serial.println(httpCode);
    Serial.println(http.errorToString(httpCode));
    externalIPError = true;
  }

  http.end();
  Serial.print("External IP address request took ");
  Serial.print(millis() - start);
  Serial.println(" ms");

  return _ip;
}


//+=============================================================================
// Turn off the Led after timeout
//
void disableLed()
{
  digitalWrite(ledpin, HIGH);                           // Shut down the LED
  ticker.detach();                                      // Stopping the ticker
}


//+=============================================================================
// Gets called when WiFiManager enters configuration mode
//
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}


//+=============================================================================
// Gets called when device loses connection to the accesspoint
//
void lostWifiCallback (const WiFiEventStationModeDisconnected& evt) {
  Serial.println("Lost Wifi");
  // reset and try again, or maybe put it to deep sleep
  ESP.reset();
  delay(1000);
}


//+=============================================================================
// First setup of the Wifi.
// If return true, the Wifi is well connected.
// Should not return false if Wifi cannot be connected, it will loop
//
bool setupWifi(bool resetConf) {
  // start ticker with 0.5 because we start in AP mode and try to connect
  ticker.attach(0.5, tick);
  
  WiFi.mode(WIFI_STA); // To make sure STA mode is preserved by WiFiManager and resets it after config is done.
  
  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);
  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Reset device if on config portal for greater than 3 minutes
  wifiManager.setConfigPortalTimeout(180);

  if (LittleFS.begin()) {
    Serial.println("mounted file system");
    if (LittleFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument json(512);
        DeserializationError error = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if (!error) {
          Serial.println("\nparsed json");

          if (json.containsKey("hostname")) strncpy(host_name, json["hostname"], 20);
          if (json.containsKey("passcode")) strncpy(passcode, json["passcode"], 20);
          if (json.containsKey("mqtt_host")) strncpy(mqtt_host, json["mqtt_host"], 100);
          if (json.containsKey("mqtt_port_str")) {
            strncpy(mqtt_port_str, json["mqtt_port_str"], 6);
            port = atoi(json["mqtt_port_str"]);
          }
          if (json.containsKey("mqtt_user")) strncpy(mqtt_user, json["mqtt_user"], 20);
          if (json.containsKey("mqtt_pass")) strncpy(mqtt_pass, json["mqtt_pass"], 20);
          if (json.containsKey("user_id")) strncpy(user_id, json["user_id"], 60);
          if (json.containsKey("port_str")) {
            strncpy(port_str, json["port_str"], 6);
            port = atoi(json["port_str"]);
          }
          if (json.containsKey("ip")) strncpy(static_ip, json["ip"], 16);
          if (json.containsKey("gw")) strncpy(static_gw, json["gw"], 16);
          if (json.containsKey("sn")) strncpy(static_sn, json["sn"], 16);
          if (json.containsKey("dns")) strncpy(static_dns, json["dns"], 16);
        } else {
          Serial.println("failed to load json config");
        }
        json.clear();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  WiFiManagerParameter custom_hostname("hostname", "Choose a hostname to this IR Controller", host_name, 20);
  wifiManager.addParameter(&custom_hostname);
  WiFiManagerParameter custom_passcode("passcode", "Choose a passcode", passcode, 20);
  wifiManager.addParameter(&custom_passcode);
  WiFiManagerParameter custom_mqtt_host("mqtt_host", "Choose a MQTT host", mqtt_host, 100);
  wifiManager.addParameter(&custom_mqtt_host);
  WiFiManagerParameter custom_mqtt_port("mqtt_port_str", "Choose MQTT server port", mqtt_port_str, 6);
  wifiManager.addParameter(&custom_mqtt_port);
  WiFiManagerParameter custom_mqtt_user("mqtt_user", "Choose a MQTT user", mqtt_user, 20);
  wifiManager.addParameter(&custom_mqtt_user);
  WiFiManagerParameter custom_mqtt_pass("mqtt_pass", "Choose a MQTT password", mqtt_pass, 20);
  wifiManager.addParameter(&custom_mqtt_pass);
  WiFiManagerParameter custom_userid("user_id", "Enter your Amazon user_id", user_id, 60);
  wifiManager.addParameter(&custom_userid);
  WiFiManagerParameter custom_port("port_str", "Choose a port", port_str, 6);
  wifiManager.addParameter(&custom_port);

  wifiManager.setShowStaticFields(true);
  wifiManager.setShowDnsFields(true);

  IPAddress sip, sgw, ssn, dns;
  sip.fromString(static_ip);
  sgw.fromString(static_gw);
  ssn.fromString(static_sn);
  dns.fromString(static_dns);

  if (resetConf) {
    Serial.println("Reset triggered, launching in AP mode");
    wifiManager.startConfigPortal(wifi_config_name);
  } else {
    Serial.println("Setting static WiFi data from config");
    wifiManager.setSTAStaticIPConfig(sip, sgw, ssn, dns);
  }

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(wifi_config_name)) {
    Serial.println("Failed to connect and hit timeout");
    // reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  // if you get here you have connected to the WiFi
  strncpy(host_name, custom_hostname.getValue(), 20);
  strncpy(passcode, custom_passcode.getValue(), 20);
  strncpy(mqtt_host, custom_mqtt_host.getValue(), 100);
  strncpy(mqtt_port_str, custom_mqtt_port.getValue(), 6);
  strncpy(mqtt_user, custom_mqtt_user.getValue(), 20);
  strncpy(mqtt_pass, custom_mqtt_pass.getValue(), 20);
  strncpy(user_id, custom_userid.getValue(), 60);
  strncpy(port_str, custom_port.getValue(), 6);
  port = atoi(port_str);
  mqtt_port = atoi(mqtt_port_str);

  // Reset device if lost wifi Connection
  WiFi.onStationModeDisconnected(&lostWifiCallback);

  Serial.println("WiFi connected! User chose hostname '" + String(host_name) + String("' passcode '") + String(passcode) + "' and port '" + String(port_str) + "'");

  // Save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println(" config...");
    DynamicJsonDocument json(512);
    json["hostname"] = host_name;
    json["passcode"] = passcode;
    json["mqtt_host"] = mqtt_host;
    json["mqtt_port_str"] = mqtt_port_str;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_pass"] = mqtt_pass;
    json["user_id"] = user_id;
    json["port_str"] = port_str;
    json["ip"] = WiFi.localIP().toString();
    json["gw"] = WiFi.gatewayIP().toString();
    json["sn"] = WiFi.subnetMask().toString();
    json["dns"] = WiFi.dnsIP().toString();

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    serializeJson(json, Serial);
    Serial.println("");
    Serial.println("Writing config file");
    serializeJson(json, configFile);
    configFile.close();
    json.clear();
    Serial.println("Config written successfully");
  }
  ticker.detach();

  // keep LED on
  digitalWrite(ledpin, LOW);
  return true;
}


//+=============================================================================
// Setup ESP8266WebServer/MQTT and IR receiver/blaster
//
void setup() {
  // Initialize serial
  Serial.begin(115200);

  // set led pin as output
  pinMode(ledpin, OUTPUT);

  Serial.println("");
  Serial.println("ESP8266 IR Controller");
  pinMode(configpin, INPUT_PULLUP);
  Serial.print("Config pin GPIO");
  Serial.print(configpin);
  Serial.print(" set to: ");
  Serial.println(digitalRead(configpin));
  if (!setupWifi(digitalRead(configpin) == LOW))
    return;

  Serial.println("WiFi configuration complete");

  if (strlen(host_name) > 0) {
    WiFi.hostname(host_name);
  } else {
    WiFi.hostname().toCharArray(host_name, 20);
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  wifi_set_sleep_type(LIGHT_SLEEP_T);
  digitalWrite(ledpin, LOW);
  // Turn off the led in 2s
  ticker.attach(2, disableLed);

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP().toString());
  Serial.print("DNS IP: ");
  Serial.println(WiFi.dnsIP().toString());
  Serial.println("URL to send commands: http://" + String(host_name) + ".local:" + port_str);

  if (server != NULL) {
    delete server;
  }
  server = new ESP8266WebServer(port);

#if enabledMQTT == 1
  // MQTT
  const char fingerprint[] = "4F 75 27 A0 9B E2 23 85 5E B0 63 DA 40 73 51 D8 0E 7B 70 2E";

  if (mqtt_enabled()) {
    secureClient.setFingerprint(fingerprint);
    IPAddress mqtt_ip;
    if (mqtt_ip.fromString(mqtt_host)) {
      Serial.println("MQTT IP: " + String(mqtt_host));
      mqtt_client.setServer(mqtt_ip, mqtt_port);
    } else {
      Serial.println("MQTT host: " + String(mqtt_host));
      mqtt_client.setServer(mqtt_host, mqtt_port);
    }
    mqtt_client.setCallback(mqtt_callback);
  } else {
    Serial.println("MQTT not enabled, host not set");
  }
#else
  if (enableMDNSServices) {
    // Configure OTA Update
    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname(host_name);
    ArduinoOTA.onStart([]() {
      Serial.println("Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("ArduinoOTA started");

    // Configure mDNS
    MDNS.addService("http", "tcp", port); // Announce the ESP as an HTTP service
    Serial.println("MDNS http service added. Hostname is set to " + String(host_name) + ".local:" + String(port));
  }

  Serial.println("Starting UDP");
  ntpUDP.begin(localPort);
  Serial.print("Local port: ");
  Serial.println(ntpUDP.localPort());
  Serial.println("Waiting for sync");
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  // Configure the server
  server->on("/json", []() { // JSON handler for more complicated IR blaster routines
    Serial.println("Connection received endpoint '/json'");

    int simple = 0;
    if (server->hasArg("simple")) simple = server->arg("simple").toInt();
    String signature = server->arg("auth");
    String epid = server->arg("epid");
    String mid = server->arg("mid");
    String timestamp = server->arg("time");

    if (!allowLocalBypass(server->client().remoteIP()) && !isPasscodeValid(server->arg("pass"))) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, invalid passcode");
    } else if (strlen(user_id) != 0 && !validateHMAC(epid, mid, timestamp, signature, server->client().remoteIP())) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, HMAC security authentication failed");
    } else {
      DynamicJsonDocument root(packetSize);
      DeserializationError error = deserializeJson(root, server->arg("plain"));
      int out = (server->hasArg("out")) ? server->arg("out").toInt() : 1;
      if (error) {
        Serial.println("JSON parsing failed");
        Serial.println(error.c_str());
        if (simple) {
          sendCorsHeaders();
          server->send(400, "text/plain", "JSON parsing failed, " + String(error.c_str()));
        } else {
          sendHomePage("JSON parsing failed", "Error", 3, 400); // 400
        }
        root.clear();
      } else {
        digitalWrite(ledpin, LOW);
        ticker.attach(0.5, disableLed);

        // Handle device state limitations for the global JSON command request
        if (server->hasArg("device")) {
          String device = server->arg("device");
          Serial.println("Device name detected " + device);
          int state = (server->hasArg("state")) ? server->arg("state").toInt() : 0;
          if (deviceState.containsKey(device)) {
            Serial.println("Contains the key!");
            Serial.println(state);
            int currentState = deviceState[device];
            Serial.println(currentState);
            if (state == currentState) {
              if (simple) {
                sendCorsHeaders();
                server->send(200, "text/html", "Not sending command to " + device + ", already in state " + state);
              } else {
                sendHomePage("Not sending command to " + device + ", already in state " + state, "Warning", 2); // 200
              }
              Serial.println("Not sending command to " + device + ", already in state " + state);
              return;
            } else {
              Serial.println("Setting device " + device + " to state " + state);
              deviceState[device] = state;
            }
          } else {
            Serial.println("Setting device " + device + " to state " + state);
            deviceState[device] = state;
          }
        }

        if (simple) {
          sendCorsHeaders();
          server->send(200, "text/html", "Success, code sent");
        }

        String message = processJson(root, out);

        if (!simple) {
          Serial.println("Sending home page");
          sendHomePage(message, "Success", 1); // 200
        }

        root.clear();
      }
    }
  });

  // Setup simple msg server to mirror version 1.0 functionality
  server->on("/msg", []() {
    Serial.println("Connection received endpoint '/msg'");

    int simple = 0;
    if (server->hasArg("simple")) simple = server->arg("simple").toInt();
    String signature = server->arg("auth");
    String epid = server->arg("epid");
    String mid = server->arg("mid");
    String timestamp = server->arg("time");

    if (!allowLocalBypass(server->client().remoteIP()) && !isPasscodeValid(server->arg("pass"))) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, invalid passcode");
    } else if (strlen(user_id) != 0 && !validateHMAC(epid, mid, timestamp, signature, server->client().remoteIP())) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, HMAC security authentication");
    } else {
      digitalWrite(ledpin, LOW);
      ticker.attach(0.5, disableLed);
      String type = server->arg("type");
      String data = server->arg("data");
      String ip = server->arg("ip");

      // Handle device state limitations
      if (server->hasArg("device")) {
        String device = server->arg("device");
        Serial.println("Device name detected " + device);
        int state = (server->hasArg("state")) ? server->arg("state").toInt() : 0;
        if (deviceState.containsKey(device)) {
          Serial.println("Contains the key!");
          Serial.println(state);
          int currentState = deviceState[device];
          Serial.println(currentState);
          if (state == currentState) {
            if (simple) {
              sendCorsHeaders();
              server->send(200, "text/html", "Not sending command to " + device + ", already in state " + state);
            } else {
              sendHomePage("Not sending command to " + device + ", already in state " + state, "Warning", 2); // 200
            }
            Serial.println("Not sending command to " + device + ", already in state " + state);
            return;
          } else {
            Serial.println("Setting device " + device + " to state " + state);
            deviceState[device] = state;
          }
        } else {
          Serial.println("Setting device " + device + " to state " + state);
          deviceState[device] = state;
        }
      }

      int len = server->arg("length").toInt();
      long address = 0;
      if (server->hasArg("address")) {
        String addressString = server->arg("address");
        address = strtoul(addressString.c_str(), 0, 0);
      }

      int rdelay = (server->hasArg("rdelay")) ? server->arg("rdelay").toInt() : 1000;
      int pulse = (server->hasArg("pulse")) ? server->arg("pulse").toInt() : 1;
      int pdelay = (server->hasArg("pdelay")) ? server->arg("pdelay").toInt() : 100;
      int repeat = (server->hasArg("repeat")) ? server->arg("repeat").toInt() : 1;
      int out = (server->hasArg("out")) ? server->arg("out").toInt() : 1;
      if (server->hasArg("code")) {
        String code = server->arg("code");
        char separator = ':';
        data = getValue(code, separator, 0);
        type = getValue(code, separator, 1);
        len = getValue(code, separator, 2).toInt();
      }

      if (simple) {
        sendCorsHeaders();
        server->send(200, "text/html", "Success, code sent");
      }

      if (type == "roku") {
        rokuCommand(ip, data, repeat, rdelay);
      } else {
        irblast(type, data, len, rdelay, pulse, pdelay, repeat, address, pickIRsend(out));
      }

      if (!simple) {
        sendHomePage("Code Sent", "Success", 1); // 200
      }
    }
  });
#endif

  server->on("/received", []() {
    Serial.println("Connection received endpoint '/received'");
    String signature = server->arg("auth");
    String epid = server->arg("epid");
    String mid = server->arg("mid");
    String timestamp = server->arg("time");
    
    if (!allowLocalBypass(server->client().remoteIP()) && !isPasscodeValid(server->arg("pass"))) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, invalid passcode");
    } else if (strlen(user_id) != 0 && !validateHMAC(epid, mid, timestamp, signature, server->client().remoteIP())) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, HMAC security authentication");
    } else {
      int id = server->arg("id").toInt();
      String output;
      if (id == 1 && last_recv.valid) {
        sendCodePage(last_recv);
      } else if (id == 2 && last_recv_2.valid) {
        sendCodePage(last_recv_2);
      } else if (id == 3 && last_recv_3.valid) {
        sendCodePage(last_recv_3);
      } else if (id == 4 && last_recv_4.valid) {
        sendCodePage(last_recv_4);
      } else if (id == 5 && last_recv_5.valid) {
        sendCodePage(last_recv_5);
      } else {
        sendHomePage("Code does not exist", "Alert", 2, 404); // 404
      }
    }
  });

  server->on("/", []() {
    Serial.println("Connection received endpoint '/'");
    String signature = server->arg("auth");
    String epid = server->arg("epid");
    String mid = server->arg("mid");
    String timestamp = server->arg("time");
    
    if (!allowLocalBypass(server->client().remoteIP()) && !isPasscodeValid(server->arg("pass"))) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, invalid passcode");
    } else if (strlen(user_id) != 0 && !validateHMAC(epid, mid, timestamp, signature, server->client().remoteIP())) {
      Serial.println("Unauthorized access");
      sendCorsHeaders();
      server->send(401, "text/plain", "Unauthorized, HMAC security authentication");
    } else {
      sendHomePage(); // 200
    }
  });

  server->begin();

  Serial.println("HTTP Server started on port " + String(port));

  externalIP();

  if (strlen(user_id) > 0) {
    userIDError = !validUID(user_id);
    if (!userIDError) {
      Serial.println("No errors detected with security configuration");
    }

    // Validation check time
    time_t timenow = now() - (timeZone * SECS_PER_HOUR);
    bool validEpoch = validEPOCH(timenow);
    if (validEpoch) {
      Serial.println("EPOCH time obtained for security checks");
    } else {
      Serial.println("Invalid EPOCH time, security checks may fail if unable to sync with NTP server");
    }
  }

  irsend1.begin();
  irsend2.begin();
  irsend3.begin();
  irsend4.begin();
  irrecv.enableIRIn();
  Serial.println("Ready to send and receive IR signals");
}

String processJson(DynamicJsonDocument& root, int out) {
  String message = "Code sent";
  for (size_t x = 0; x < root.size(); x++) {
    String type = root[x]["type"];
    String ip = root[x]["ip"];
    int rdelay = root[x]["rdelay"];
    int pulse = root[x]["pulse"];
    int pdelay = root[x]["pdelay"];
    int repeat = root[x]["repeat"];
    int xout = root[x]["out"];
    if (xout == 0) {
      xout = out;
    }
    int duty = root[x]["duty"];

    if (pulse <= 0) pulse = 1; // Make sure pulse isn't 0
    if (repeat <= 0) repeat = 1; // Make sure repeat isn't 0
    if (pdelay <= 0) pdelay = 100; // Default pdelay
    if (rdelay <= 0) rdelay = 1000; // Default rdelay
    if (duty <= 0) duty = 50; // Default duty

    // Handle device state limitations on a per JSON object basis
    String device = root[x]["device"];
    if (device != "null") {
      int state = root[x]["state"];
      if (deviceState.containsKey(device)) {
        int currentState = deviceState[device];
        if (state == currentState) {
          Serial.println("Not sending command to " + device + ", already in state " + state);
          message = "Code sent. Some components of the code were held because device was already in appropriate state";
          continue;
        } else {
          Serial.println("Setting device " + device + " to state " + state);
          deviceState[device] = state;
        }
      } else {
        Serial.println("Setting device " + device + " to state " + state);
        deviceState[device] = state;
      }
    }

    if (type == "delay") {
      delay(rdelay);
    } else if (type == "raw") {
      JsonArray raw = root[x]["data"]; // Array of unsigned int values for the raw signal
      int khz = root[x]["khz"];
      if (khz <= 0) khz = 38; // Default to 38khz if not set
      rawblast(raw, khz, rdelay, pulse, pdelay, repeat, pickIRsend(xout),duty);
    } else if (type == "pronto") {
      JsonArray pdata = root[x]["data"]; // Array of values for pronto
      pronto(pdata, rdelay, pulse, pdelay, repeat, pickIRsend(xout));
    } else if (type == "roku") {
      String data = root[x]["data"];
      rokuCommand(ip, data, repeat, rdelay);
    } else {
      String data = root[x]["data"];
      String addressString = root[x]["address"];
      long address = strtoul(addressString.c_str(), 0, 0);
      int len = root[x]["length"];
      irblast(type, data, len, rdelay, pulse, pdelay, repeat, address, pickIRsend(xout));
    }
  }
  return message;
}

#if enabledMQTT == 1
//+=============================================================================
// MQTT Enabled
//
boolean mqtt_enabled() {
  return (String(mqtt_host).length() > 0) && (String(user_id).length() > 0) && (String(host_name).length() > 0);
}


//+=============================================================================
// MQTT Reconnect
//
boolean mqtt_reconnect() {
  // Create a random client ID
  String clientId = String(host_name);
  clientId += "_" + String(random(0xffff), HEX);

  Serial.print("Attempting MQTT connection... ");
  // Attempt to connect
  if ((String(mqtt_user).length() == 0 && mqtt_client.connect(clientId.c_str())) || mqtt_client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
    String sub = String(user_id) + "/" + String(host_name);
    mqtt_client.subscribe(sub.c_str());
    Serial.println("MQTT connected");
    Serial.print("Subscribed to ");
    Serial.println(sub);
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqtt_client.state());
  }
  return mqtt_client.connected();
}


//+=============================================================================
// MQTT Callback
//
void mqtt_callback(char* topic, byte * payload, unsigned int length) {
  Serial.println("MQTT message received");
  DynamicJsonDocument root(packetSize);
  DeserializationError error = deserializeJson(root, payload);
  if (error) {
    Serial.println("JSON parsing failed");
    Serial.println(error.c_str());
  } else {
    // serializeJson(root, Serial);
    processJson(root, 1);
    digitalWrite(ledpin, LOW);
    ticker.attach(0.5, disableLed);
  }
  root.clear();
}
#else


//+=============================================================================
// NTP Code
//
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (ntpUDP.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = ntpUDP.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      ntpUDP.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}


//+=============================================================================
// Send an NTP request to the time server at the given address
//
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  ntpUDP.beginPacket(address, 123); //NTP requests are to port 123
  ntpUDP.write(packetBuffer, NTP_PACKET_SIZE);
  ntpUDP.endPacket();
}
#endif


//+=============================================================================
// Send CORS HTTP headers
//
void sendCorsHeaders() {
  server->sendHeader("Access-Control-Allow-Origin", "*");
  server->sendHeader("Access-Control-Allow-Methods", "GET, POST");
}


//+=============================================================================
// Send header HTML
//
void sendHeader() {
  sendHeader(200);
}

void sendHeader(int httpcode) {
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(httpcode, "text/html; charset=utf-8", "");
  server->sendContent_P("<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Strict//EN' 'http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd'>\n");
  server->sendContent_P("<html xmlns='http://www.w3.org/1999/xhtml' xml:lang='en'>\n");
  server->sendContent_P("  <head>\n");
  server->sendContent_P("    <meta name='viewport' content='width=device-width, initial-scale=.75' />\n");
  server->sendContent_P("    <link rel='stylesheet' href='https://stackpath.bootstrapcdn.com/bootstrap/3.4.1/css/bootstrap.min.css' />\n");
  server->sendContent_P("    <style>@media (max-width: 991px) {.nav-pills>li {float: none; margin-left: 0; margin-top: 5px; text-align: center;}}</style>\n");
  server->sendContent_P(("    <title>ESP8266 IR Controller (" + String(host_name) + ")</title>\n").c_str());
  server->sendContent_P("  </head>\n");
  server->sendContent_P("  <body>\n");
  server->sendContent_P("    <div class='container'>\n");
  server->sendContent_P("      <h1><a href='https://github.com/mdhiggins/ESP8266-HTTP-IR-Blaster'>ESP8266 IR Controller</a></h1>\n");
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <ul class='nav nav-pills'>\n");
  server->sendContent_P("            <li class='active'>\n");
  server->sendContent_P(("              <a href='http://" + String(host_name) + ".local" + ":" + String(port) + "'>Hostname <span class='badge'>" + String(host_name) + ".local" + ":" + String(port) + "</span></a></li>\n").c_str());
#if enabledMQTT == 0
  server->sendContent_P("            <li class='active'>\n");
  server->sendContent_P(("              <a href='http://" + WiFi.localIP().toString() + ":" + String(port) + "'>Local <span class='badge'>" + WiFi.localIP().toString() + ":" + String(port) + "</span></a></li>\n").c_str());
#endif
  server->sendContent_P("            <li class='active'>\n");
  server->sendContent_P(("              <a href='http://" + WiFi.dnsIP().toString() + "'>DNS <span class='badge'>" + WiFi.dnsIP().toString() + "</span></a></li>\n").c_str());
  server->sendContent_P("            <li class='active'>\n");
  server->sendContent_P(("              <a href='http://" + externalIP() + ":" + String(port) + "'>External <span class='badge'>" + externalIP() + ":" + String(port) + "</span></a></li>\n").c_str());
  server->sendContent_P("            <li class='active'>\n");
  server->sendContent_P(("              <a>MAC <span class='badge'>" + String(WiFi.macAddress()) + "</span></a></li>\n").c_str());
  server->sendContent_P("          </ul>\n");
  server->sendContent_P("        </div>\n");
  server->sendContent_P("      </div><hr />\n");
}


//+=============================================================================
// Send footer HTML
//
void sendFooter() {
#if enabledMQTT == 1
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><em>" + String(millis()) + "ms uptime</em></div></div>\n").c_str());
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Device in MQTT mode, HTTP commands and SHA256 HTTP authentincation disabled to save memory, TLS based MQTT SSL enabled</em></div></div>");
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>mDNS services, NTP, and ArduinoOTA disabled, not compatible with MQTT</em></div></div>");
#else
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><em>" + String(millis()) + "ms uptime; EPOCH " + String(now() - (timeZone * SECS_PER_HOUR)) + "</em> / <em id='jepoch'></em> ( <em id='jdiff'></em> )</div></div>\n").c_str());
  server->sendContent_P("      <script>document.getElementById('jepoch').innerHTML = Math.round((new Date()).getTime() / 1000)</script>");
  server->sendContent_P(("      <script>document.getElementById('jdiff').innerHTML = Math.abs(Math.round((new Date()).getTime() / 1000) - " + String(now() - (timeZone * SECS_PER_HOUR)) + ")</script>").c_str());
  if (strlen(user_id) != 0)
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Device secured with SHA256 authentication. Only commands sent and verified with Amazon Alexa and the IR Controller Skill will be processed</em></div></div>");
  if (authError)
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Error - last authentication failed because HMAC signatures did not match, see serial output for debugging details</em></div></div>");
  if (timeAuthError > 0)
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><em>Error - last authentication failed because your timestamps are out of sync, see serial output for debugging details. Timediff: " + String(timeAuthError) + "</em></div></div>").c_str());
  if (externalIPError)
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Error - unable to retrieve external IP address, this may be due to bad network settings.</em></div></div>");
  time_t timenow = now() - (timeZone * SECS_PER_HOUR);
  if (!validEPOCH(timenow))
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Error - EPOCH time is inappropriately low, likely connection to external time server has failed, check your network settings</em></div></div>");
  if (userIDError)
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Error - your userID is in the wrong format and authentication will not work</em></div></div>");
  if (ntpError)
  server->sendContent_P("      <div class='row'><div class='col-md-12'><em>Error - last attempt to connect to the NTP server failed, check NTP settings and networking settings</em></div></div>");
#endif
  server->sendContent_P("    </div>\n");
  server->sendContent_P("  </body>\n");
  server->sendContent_P("</html>\n");
  server->client().stop();
}


//+=============================================================================
// Stream home page HTML
//
void sendHomePage() {
  sendHomePage("", "");
}

void sendHomePage(String message, String header) {
  sendHomePage(message, header, 0);
}

void sendHomePage(String message, String header, int type) {
  sendHomePage(message, header, type, 200);
}

void sendHomePage(String message, String header, int type, int httpcode) {
  sendHeader(httpcode);
  if (type == 1)
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><div class='alert alert-success'><strong>" + header + "!</strong> " + message + "</div></div></div>\n").c_str());
  if (type == 2)
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><div class='alert alert-warning'><strong>" + header + "!</strong> " + message + "</div></div></div>\n").c_str());
  if (type == 3)
  server->sendContent_P(("      <div class='row'><div class='col-md-12'><div class='alert alert-danger'><strong>" + header + "!</strong> " + message + "</div></div></div>\n").c_str());
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <h3>Codes Transmitted</h3>\n");
  server->sendContent_P("          <table class='table table-striped' style='table-layout: fixed;'>\n");
  server->sendContent_P("            <thead><tr><th>Sent</th><th>Command</th><th>Type</th><th>Length</th><th>Address</th></tr></thead>\n"); //Title
  server->sendContent_P("            <tbody>\n");
  if (last_send.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td>" + epochToString(last_send.timestamp) + "</td><td><code>" + String(last_send.data) + "</code></td><td><code>" + String(last_send.encoding) + "</code></td><td><code>" + String(last_send.bits) + "</code></td><td><code>" + String(last_send.address) + "</code></td></tr>\n").c_str());
  if (last_send_2.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td>" + epochToString(last_send_2.timestamp) + "</td><td><code>" + String(last_send_2.data) + "</code></td><td><code>" + String(last_send_2.encoding) + "</code></td><td><code>" + String(last_send_2.bits) + "</code></td><td><code>" + String(last_send_2.address) + "</code></td></tr>\n").c_str());
  if (last_send_3.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td>" + epochToString(last_send_3.timestamp) + "</td><td><code>" + String(last_send_3.data) + "</code></td><td><code>" + String(last_send_3.encoding) + "</code></td><td><code>" + String(last_send_3.bits) + "</code></td><td><code>" + String(last_send_3.address) + "</code></td></tr>\n").c_str());
  if (last_send_4.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td>" + epochToString(last_send_4.timestamp) + "</td><td><code>" + String(last_send_4.data) + "</code></td><td><code>" + String(last_send_4.encoding) + "</code></td><td><code>" + String(last_send_4.bits) + "</code></td><td><code>" + String(last_send_4.address) + "</code></td></tr>\n").c_str());
  if (last_send_5.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td>" + epochToString(last_send_5.timestamp) + "</td><td><code>" + String(last_send_5.data) + "</code></td><td><code>" + String(last_send_5.encoding) + "</code></td><td><code>" + String(last_send_5.bits) + "</code></td><td><code>" + String(last_send_5.address) + "</code></td></tr>\n").c_str());
  if (!last_send.valid && !last_send_2.valid && !last_send_3.valid && !last_send_4.valid && !last_send_5.valid)
  server->sendContent_P("              <tr><td colspan='5' class='text-center'><em>No codes sent</em></td></tr>");
  server->sendContent_P("            </tbody></table>\n");
  server->sendContent_P("          </div></div>\n");
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <h3>Codes Received</h3>\n");
  server->sendContent_P("          <table class='table table-striped' style='table-layout: fixed;'>\n");
  server->sendContent_P("            <thead><tr><th>Received</th><th>Command</th><th>Type</th><th>Length</th><th>Address</th></tr></thead>\n"); //Title
  server->sendContent_P("            <tbody>\n");
  if (last_recv.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td><a href='/received?id=1'>" + epochToString(last_recv.timestamp) + "</a></td><td><code>" + String(last_recv.data) + "</code></td><td><code>" + String(last_recv.encoding) + "</code></td><td><code>" + String(last_recv.bits) + "</code></td><td><code>" + String(last_recv.address) + "</code></td></tr>\n").c_str());
  if (last_recv_2.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td><a href='/received?id=2'>" + epochToString(last_recv_2.timestamp) + "</a></td><td><code>" + String(last_recv_2.data) + "</code></td><td><code>" + String(last_recv_2.encoding) + "</code></td><td><code>" + String(last_recv_2.bits) + "</code></td><td><code>" + String(last_recv_2.address) + "</code></td></tr>\n").c_str());
  if (last_recv_3.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td><a href='/received?id=3'>" + epochToString(last_recv_3.timestamp) + "</a></td><td><code>" + String(last_recv_3.data) + "</code></td><td><code>" + String(last_recv_3.encoding) + "</code></td><td><code>" + String(last_recv_3.bits) + "</code></td><td><code>" + String(last_recv_3.address) + "</code></td></tr>\n").c_str());
  if (last_recv_4.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td><a href='/received?id=4'>" + epochToString(last_recv_4.timestamp) + "</a></td><td><code>" + String(last_recv_4.data) + "</code></td><td><code>" + String(last_recv_4.encoding) + "</code></td><td><code>" + String(last_recv_4.bits) + "</code></td><td><code>" + String(last_recv_4.address) + "</code></td></tr>\n").c_str());
  if (last_recv_5.valid)
  server->sendContent_P(("              <tr class='text-uppercase'><td><a href='/received?id=5'>" + epochToString(last_recv_5.timestamp) + "</a></td><td><code>" + String(last_recv_5.data) + "</code></td><td><code>" + String(last_recv_5.encoding) + "</code></td><td><code>" + String(last_recv_5.bits) + "</code></td><td><code>" + String(last_recv_5.address) + "</code></td></tr>\n").c_str());
  if (!last_recv.valid && !last_recv_2.valid && !last_recv_3.valid && !last_recv_4.valid && !last_recv_5.valid)
  server->sendContent_P("              <tr><td colspan='5' class='text-center'><em>No codes received</em></td></tr>");
  server->sendContent_P("            </tbody></table>\n");
  server->sendContent_P("          </div></div>\n");
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P(("            <li><span class='badge'>GPIO " + String(pinr1) + "</span> Receiving </li>\n").c_str());
  server->sendContent_P(("            <li><span class='badge'>GPIO " + String(pins1) + "</span> Transmitter 1 </li>\n").c_str());
  server->sendContent_P(("            <li><span class='badge'>GPIO " + String(pins2) + "</span> Transmitter 2 </li>\n").c_str());
  server->sendContent_P(("            <li><span class='badge'>GPIO " + String(pins3) + "</span> Transmitter 3 </li>\n").c_str());
  server->sendContent_P(("            <li><span class='badge'>GPIO " + String(pins4) + "</span> Transmitter 4 </li></ul>\n").c_str());
  server->sendContent_P("        </div>\n");
  server->sendContent_P("      </div>\n");
  sendFooter();
}

//+=============================================================================
// Stream code page HTML
//
void sendCodePage(Code selCode) {
  sendCodePage(selCode, 200);
}

void sendCodePage(Code selCode, int httpcode){
  sendHeader(httpcode);
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P(("          <h2><span class='label label-success'>" + String(selCode.data) + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "</span></h2><br/>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Data</dt>\n");
  server->sendContent_P(("            <dd><code>" + String(selCode.data)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Type</dt>\n");
  server->sendContent_P(("            <dd><code>" + String(selCode.encoding)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Length</dt>\n");
  server->sendContent_P(("            <dd><code>" + String(selCode.bits)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Address</dt>\n");
  server->sendContent_P(("            <dd><code>" + String(selCode.address)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Raw</dt>\n");
  server->sendContent_P(("            <dd><code>" + String(selCode.raw)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("          <dl class='dl-horizontal'>\n");
  server->sendContent_P("            <dt>Timestamp</dt>\n");
  server->sendContent_P(("            <dd><code>" + epochToString(selCode.timestamp)  + "</code></dd></dl>\n").c_str());
  server->sendContent_P("        </div></div>\n");
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <div class='alert alert-warning'>Don't forget to add your passcode to the URLs below if you set one</div>\n");
  server->sendContent_P("      </div></div>\n");
  if (String(selCode.encoding) == "UNKNOWN") {
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P("            <li>Hostname <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + String(host_name) + ".local:" + String(port) + "/json?plain=[{data:[" + String(selCode.raw) + "],type:'raw',khz:38}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>Local IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + WiFi.localIP().toString() + ":" + String(port) + "/json?plain=[{data:[" + String(selCode.raw) + "],type:'raw',khz:38}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>External IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + externalIP() + ":" + String(port) + "/json?plain=[{data:[" + String(selCode.raw) + "],type:'raw',khz:38}]</pre></li></ul>\n").c_str());
  } else if (String(selCode.encoding) == "PANASONIC" || String(selCode.encoding) == "NEC") {
  //} else if (strtoul(selCode.address, 0, 0) > 0) {
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P("            <li>Hostname <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + String(host_name) + ".local:" + String(port) + "/msg?code=" + String(selCode.data) + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "&address=" + String(selCode.address) + "</pre></li>\n").c_str());
  server->sendContent_P("            <li>Local IP <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + WiFi.localIP().toString() + ":" + String(port) + "/msg?code=" + String(selCode.data) + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "&address=" + String(selCode.address) + "</pre></li>\n").c_str());
  server->sendContent_P("            <li>External IP <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + externalIP() + ":" + String(port) + "/msg?code=" + selCode.data + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "&address=" + String(selCode.address) + "</pre></li></ul>\n").c_str());
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P("            <li>Hostname <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + String(host_name) + ".local:" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + ",address:'" + String(selCode.address) + "'}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>Local IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + WiFi.localIP().toString() + ":" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + ",address:'" + String(selCode.address) + "'}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>External IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + externalIP() + ":" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + ",address:'" + String(selCode.address) + "'}]</pre></li></ul>\n").c_str());
  } else {
  server->sendContent_P("      <div class='row'>\n");
  server->sendContent_P("        <div class='col-md-12'>\n");
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P("            <li>Hostname <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + String(host_name) + ".local:" + String(port) + "/msg?code=" + String(selCode.data) + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "</pre></li>\n").c_str());
  server->sendContent_P("            <li>Local IP <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + WiFi.localIP().toString() + ":" + String(port) + "/msg?code=" + String(selCode.data) + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "</pre></li>\n").c_str());
  server->sendContent_P("            <li>External IP <span class='label label-default'>MSG</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + externalIP() + ":" + String(port) + "/msg?code=" + selCode.data + ":" + String(selCode.encoding) + ":" + String(selCode.bits) + "</pre></li></ul>\n").c_str());
  server->sendContent_P("          <ul class='list-unstyled'>\n");
  server->sendContent_P("            <li>Hostname <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + String(host_name) + ".local:" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + "}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>Local IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + WiFi.localIP().toString() + ":" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + "}]</pre></li>\n").c_str());
  server->sendContent_P("            <li>External IP <span class='label label-default'>JSON</span></li>\n");
  server->sendContent_P(("            <li><pre>http://" + externalIP() + ":" + String(port) + "/json?plain=[{data:'" + String(selCode.data) + "',type:'" + String(selCode.encoding) + "',length:" + String(selCode.bits) + "}]</pre></li></ul>\n").c_str());
  }
  server->sendContent_P("        </div>\n");
  server->sendContent_P("     </div>\n");
  sendFooter();
}


//+=============================================================================
// Send command to local roku
//
int rokuCommand(String ip, String data, int repeat, int rdelay) {
  String url = "http://" + ip + ":8060/" + data;

  int output = 0;
  bool retry = true;

  for (int r = 0; r < repeat; r++) {
    http.begin(client, url);
    Serial.println(url);
    Serial.println("Sending roku command");
  
    copyCode(last_send_4, last_send_5);
    copyCode(last_send_3, last_send_4);
    copyCode(last_send_2, last_send_3);
    copyCode(last_send, last_send_2);
  
    strncpy(last_send.data, data.c_str(), 40);
    last_send.bits = 1;
    strncpy(last_send.encoding, "roku", 14);
    strncpy(last_send.address, ip.c_str(), 20);
    last_send.timestamp = now();
    last_send.valid = true;
  
    output = http.POST("");
    if (output < 0 && retry) {
      r--;
      retry = false;
    } else {
      retry = true;
    }
    Serial.println(output);
    http.end();
    delay(100);

    if (r + 1 < repeat) delay(rdelay);
  }
  return output;
}

//+=============================================================================
// Split string by character
//
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }

  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}


//+=============================================================================
// Return which IRsend object to act on
//
IRsend pickIRsend (int out) {
  switch (out) {
    case 1: return irsend1;
    case 2: return irsend2;
    case 3: return irsend3;
    case 4: return irsend4;
    default: return irsend1;
  }
}


//+=============================================================================
// Display encoding type
//
String encoding(decode_results *results) {
  return typeToString(results->decode_type);
}

//+=============================================================================
// Code to string
//
void fullCode (decode_results *results)
{
  Serial.print("One line: ");
  serialPrintUint64(results->value, 16);
  Serial.print(":");
  Serial.print(encoding(results));
  Serial.print(":");
  Serial.print(results->bits, DEC);
  if (results->repeat) Serial.print(" (Repeat)");
  Serial.println("");
  if (results->overflow)
    Serial.println("WARNING: IR code too long. "
                   "Edit IRController.ino and increase captureBufSize");
}

//+=============================================================================
// Code to JsonObject
//
void cvrtCode(Code& codeData, decode_results *results) {
  strncpy(codeData.data, uint64ToString(results->value, 16).c_str(), 40);
  strncpy(codeData.encoding, encoding(results).c_str(), 14);
  codeData.bits = results->bits;
  String r = "";
      for (uint16_t i = 1; i < results->rawlen; i++) {
      r += results->rawbuf[i] * kRawTick;
      if (i < results->rawlen - 1)
        r += ",";                           // ',' not needed on last one
      //if (!(i & 1)) r += " ";
    }
  codeData.raw = r;
  if (results->decode_type != UNKNOWN) {
    strncpy(codeData.address, ("0x" + String(results->address, HEX)).c_str(), 20);
    strncpy(codeData.command, ("0x" + String(results->command, HEX)).c_str(), 40);
  } else {
    strncpy(codeData.address, "0x0", 20);
    strncpy(codeData.command, "0x0", 40);
  }
}

//+=============================================================================
// Dump out the decode_results structure.
//
void dumpInfo(decode_results *results) {
  if (results->overflow)
    Serial.println("WARNING: IR code too long. "
                   "Edit IRrecv.h and increase RAWBUF");

  // Show Encoding standard
  Serial.print("Encoding  : ");
  Serial.print(encoding(results));
  Serial.println("");

  // Show Code & length
  Serial.print("Code      : ");
  serialPrintUint64(results->value, 16);
  Serial.print(" (");
  Serial.print(results->bits, DEC);
  Serial.println(" bits)");
}


//+=============================================================================
// Dump out the decode_results structure.
//
void dumpRaw(decode_results *results) {
  // Print Raw data
  Serial.print("Timing[");
  Serial.print(results->rawlen - 1, DEC);
  Serial.println("]: ");

  for (uint16_t i = 1;  i < results->rawlen;  i++) {
    if (i % 100 == 0)
      yield();  // Preemptive yield every 100th entry to feed the WDT.
    uint32_t x = results->rawbuf[i] * kRawTick;
    if (!(i & 1)) {  // even
      Serial.print("-");
      if (x < 1000) Serial.print(" ");
      if (x < 100) Serial.print(" ");
      Serial.print(x, DEC);
    } else {  // odd
      Serial.print("     ");
      Serial.print("+");
      if (x < 1000) Serial.print(" ");
      if (x < 100) Serial.print(" ");
      Serial.print(x, DEC);
      if (i < results->rawlen - 1)
        Serial.print(", ");  // ',' not needed for last one
    }
    if (!(i % 8)) Serial.println("");
  }
  Serial.println("");  // Newline
}


//+=============================================================================
// Dump out the decode_results structure.
//
void dumpCode(decode_results *results) {
  // Start declaration
  Serial.print("uint16_t  ");              // variable type
  Serial.print("rawData[");                // array name
  Serial.print(results->rawlen - 1, DEC);  // array size
  Serial.print("] = {");                   // Start declaration

  // Dump data
  for (uint16_t i = 1; i < results->rawlen; i++) {
    Serial.print(results->rawbuf[i] * kRawTick, DEC);
    if (i < results->rawlen - 1)
      Serial.print(",");  // ',' not needed on last one
    if (!(i & 1)) Serial.print(" ");
  }

  // End declaration
  Serial.print("};");  //

  // Comment
  Serial.print("  // ");
  Serial.print(encoding(results));
  Serial.print(" ");
  serialPrintUint64(results->value, 16);

  // Newline
  Serial.println("");

  // Now dump "known" codes
  if (results->decode_type != UNKNOWN) {
    // Some protocols have an address &/or command.
    // NOTE: It will ignore the atypical case when a message has been decoded
    // but the address & the command are both 0.
    if (results->address > 0 || results->command > 0) {
      Serial.print("uint32_t  address = 0x");
      Serial.print(results->address, HEX);
      Serial.println(";");
      Serial.print("uint32_t  command = 0x");
      Serial.print(results->command, HEX);
      Serial.println(";");
    }

    // All protocols have data
    Serial.print("uint64_t  data = 0x");
    serialPrintUint64(results->value, 16);
    Serial.println(";");
  }
}


//+=============================================================================
// Binary value to hex
//
String bin2hex(const uint8_t* bin, const int length) {
  String hex = "";

  for (int i = 0; i < length; i++) {
    if (bin[i] < 16) {
      hex += "0";
    }
    hex += String(bin[i], HEX);
  }

  return hex;
}


//+=============================================================================
// Send IR codes to variety of sources
//
void irblast(String type, String dataStr, unsigned int len, int rdelay, int pulse, int pdelay, int repeat, long address, IRsend irsend) {
  Serial.println("Blasting off");
  type.toLowerCase();
  uint64_t data = strtoull(("0x" + dataStr).c_str(), 0, 0);
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");
  // Repeat Loop
  for (int r = 0; r < repeat; r++) {
    // Pulse Loop
    for (int p = 0; p < pulse; p++) {
      serialPrintUint64(data, HEX);
      Serial.print(":");
      Serial.print(type);
      Serial.print(":");
      Serial.println(len);
      if (type == "nec") {
        irsend.sendNEC(data, len);
      } else if (type == "sony") {
        irsend.sendSony(data, len);
      } else if (type == "coolix") {
        irsend.sendCOOLIX(data, len);
      } else if (type == "whynter") {
        irsend.sendWhynter(data, len);
      } else if (type == "panasonic") {
        Serial.print("Address: ");
        Serial.println(address);
        irsend.sendPanasonic(address, data);
      } else if (type == "jvc") {
        irsend.sendJVC(data, len, 0);
      } else if (type == "samsung") {
        irsend.sendSAMSUNG(data, len);
      } else if (type == "sharpraw") {
        irsend.sendSharpRaw(data, len);
      } else if (type == "dish") {
        irsend.sendDISH(data, len);
      } else if (type == "rc5") {
        irsend.sendRC5(_rc5toggle ? data: irsend.toggleRC5(data), len);
      } else if (type == "rc6") {
        irsend.sendRC6(_rc6toggle ? data: irsend.toggleRC6(data, len), len);
      } else if (type == "denon") {
        irsend.sendDenon(data, len);
      } else if (type == "lg") {
        irsend.sendLG(data, len);
      } else if (type == "sharp") {
        irsend.sendSharpRaw(data, len);
      } else if (type == "rcmm") {
        irsend.sendRCMM(data, len);
      } else if (type == "gree") {
        irsend.sendGree(data, len);
      } else if (type == "lutron") {
        irsend.sendLutron(data, len);
      } else if (type == "roomba") {
        roomba_send(atoi(dataStr.c_str()), pulse, pdelay, irsend);
      } else if (type == "ecoclim") {
        irsend.sendEcoclim(data, len);
      }
      if (p + 1 < pulse) delay(pdelay);
    }
    if (r + 1 < repeat) delay(rdelay);

    if (toggleRC) {
      if (type == "rc5") { _rc5toggle = !_rc5toggle; }
      if (type == "rc6") { _rc6toggle = !_rc6toggle; }
    }
  }

  Serial.println("Transmission complete");

  copyCode(last_send_4, last_send_5);
  copyCode(last_send_3, last_send_4);
  copyCode(last_send_2, last_send_3);
  copyCode(last_send, last_send_2);

  strncpy(last_send.data, dataStr.c_str(), 40);
  last_send.bits = len;
  strncpy(last_send.encoding, type.c_str(), 14);
  strncpy(last_send.address, ("0x" + String(address, HEX)).c_str(), 20);
  last_send.timestamp = now();
  last_send.valid = true;

  resetReceive();
}

void pronto(JsonArray &pronto, int rdelay, int pulse, int pdelay, int repeat, IRsend irsend) {
  Serial.println("Pronto transmit");
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");
  int psize = pronto.size();
  // Repeat Loop
  for (int r = 0; r < repeat; r++) {
    // Pulse Loop
    for (int p = 0; p < pulse; p++) {
      Serial.println("Sending pronto code");
      uint16_t output[psize];
      for (int d = 0; d < psize; d++) {
        String phexp = pronto[d];
        output[d] = strtoul(phexp.c_str(), 0, 0);
      }
      irsend.sendPronto(output, psize);
      if (p + 1 < pulse) delay(pdelay);
    }
    if (r + 1 < repeat) delay(rdelay);
  }
  Serial.println("Transmission complete");

  copyCode(last_send_4, last_send_5);
  copyCode(last_send_3, last_send_4);
  copyCode(last_send_2, last_send_3);
  copyCode(last_send, last_send_2);

  strncpy(last_send.data, "", 40);
  last_send.bits = psize;
  strncpy(last_send.encoding, "PRONTO", 14);
  strncpy(last_send.address, "0x0", 20);
  last_send.timestamp = now();
  last_send.valid = true;

  resetReceive();
}

void rawblast(JsonArray &raw, int khz, int rdelay, int pulse, int pdelay, int repeat, IRsend irsend,int duty) {
  Serial.println("Raw transmit");
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");
  // Repeat Loop
  for (int r = 0; r < repeat; r++) {
    // Pulse Loop
    for (int p = 0; p < pulse; p++) {
      Serial.println("Sending code");
      irsend.enableIROut(khz,duty);
      for (unsigned int i = 0; i < raw.size(); i++) {
        int val = raw[i];
        if (i & 1) irsend.space(std::max(0, val));
        else       irsend.mark(val);
      }
      irsend.space(0);
      if (p + 1 < pulse) delay(pdelay);
    }
    if (r + 1 < repeat) delay(rdelay);
  }

  Serial.println("Transmission complete");

  copyCode(last_send_4, last_send_5);
  copyCode(last_send_3, last_send_4);
  copyCode(last_send_2, last_send_3);
  copyCode(last_send, last_send_2);

  strncpy(last_send.data, "", 40);
  last_send.bits = raw.size();
  strncpy(last_send.encoding, "RAW", 14);
  strncpy(last_send.address, "0x0", 20);
  last_send.timestamp = now();
  last_send.valid = true;

  resetReceive();
}


void roomba_send(int code, int pulse, int pdelay, IRsend irsend)
{
  Serial.print("Sending Roomba code ");
  Serial.println(code);
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");

  int length = 8;
  uint16_t raw[length * 2];
  unsigned int one_pulse = 3000;
  unsigned int one_break = 1000;
  unsigned int zero_pulse = one_break;
  unsigned int zero_break = one_pulse;
  uint16_t len = 15;
  uint16_t hz = 38;

  int arrayposition = 0;
  for (int counter = length - 1; counter >= 0; --counter) {
    if (code & (1 << counter)) {
      raw[arrayposition] = one_pulse;
      raw[arrayposition + 1] = one_break;
    }
    else {
      raw[arrayposition] = zero_pulse;
      raw[arrayposition + 1] = zero_break;
    }
    arrayposition = arrayposition + 2;
  }
  for (int i = 0; i < pulse; i++) {
    irsend.sendRaw(raw, len, hz);
    delay(pdelay);
  }

  resetReceive();
}

void copyCode (Code& c1, Code& c2) {
  strncpy(c2.data, c1.data, 40);
  strncpy(c2.encoding, c1.encoding, 14);
  //strncpy(c2.timestamp, c1.timestamp, 40);
  strncpy(c2.address, c1.address, 20);
  strncpy(c2.command, c1.command, 40);
  c2.bits = c1.bits;
  c2.raw = c1.raw;
  c2.timestamp = c1.timestamp;
  c2.valid = c1.valid;
}

void loop() {
  // Serial.println(ESP.getFreeHeap());
#if enabledMQTT == 1
  if (mqtt_enabled()) {
    if (!mqtt_client.connected()) {
      long now = millis();
      if (now - mqtt_lastReconnectAttempt > 5000) {
        mqtt_lastReconnectAttempt = now;
        // Attempt to reconnect
        if (mqtt_reconnect()) {
          mqtt_lastReconnectAttempt = 0;
        }
      }
    } else {
      // Client connected
      mqtt_client.loop();
    }
  }
#else
  ArduinoOTA.handle();
#endif
  server->handleClient();
  decode_results  results;                                        // Somewhere to store the results

  if (irrecv.decode(&results) && !holdReceive) {                  // Grab an IR code
    Serial.println("Signal received:");
    fullCode(&results);                                           // Print the singleline value
    dumpCode(&results);                                           // Output the results as source code
#if enabledMQTT == 0
    copyCode(last_recv_4, last_recv_5);                           // Pass
    copyCode(last_recv_3, last_recv_4);                           // Pass
#endif
    copyCode(last_recv_2, last_recv_3);                           // Pass
    copyCode(last_recv, last_recv_2);                             // Pass
    cvrtCode(last_recv, &results);                                // Store the results
    last_recv.timestamp = now();                                  // Set the new update time
    last_recv.valid = true;
    Serial.println("");                                           // Blank line between entries
    irrecv.resume();                                              // Prepare for the next value
    digitalWrite(ledpin, LOW);                                    // Turn on the LED for 0.5 seconds
    ticker.attach(0.5, disableLed);
  }

  delay(200);
}
