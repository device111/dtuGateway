#include "ESP8266TimerInterrupt.h"
#include <ESP8266_ISR_Timer.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

#include <ESP8266WebServer.h>

#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>

#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

#include <PubSubClient.h>

#include <ArduinoJson.h>

#include <display.h>
#include <displayTFT.h>

#include <dtuInterface.h>

#include <mqttHandler.h>

#include "index_html.h"
#include "jquery_min_js.h"
#include "style_css.h"

#include "version.h"
#include "Config.h"

// first start AP name
const char *apNameStart = "dtuGateway"; // + chipid

uint32_t chipID = ESP.getChipId();
String espUniqueName = String(apNameStart) + "_" + chipID;

// OTA
ESP8266HTTPUpdateServer httpUpdater;
// built json during compile to announce the latest greatest on snapshot or release channel
// { "version": "0.0.1", "versiondate": "01.01.2024 - 01:00:00", "linksnapshot": "https://<domain>/path/to/firmware/<file>.<bin>", "link": "https://<domain>/path/to/firmware/<file>.<bin>" }
char updateInfoWebPath[128] = "https://github.com/ohAnd/dtuGateway/releases/download/snapshot/version.json";
char updateInfoWebPathRelease[128] = "https://github.com/ohAnd/dtuGateway/releases/latest/download/version.json";

char versionServer[32] = "checking";
char versiondateServer[32] = "...";
char updateURL[196] = ""; // will be read by getting -> updateInfoWebPath
char versionServerRelease[32] = "checking";
char versiondateServerRelease[32] = "...";
char updateURLRelease[196] = ""; // will be read by getting -> updateInfoWebPath
boolean updateAvailable = false;
boolean updateRunning = false;
boolean updateInfoRequested = false;
float updateProgress = 0;
char updateState[16] = "waiting";

// user config
UserConfigManager configManager;

#define WIFI_RETRY_TIME_SECONDS 30
#define WIFI_RETRY_TIMEOUT_SECONDS 30
#define RECONNECTS_ARRAY_SIZE 50
unsigned long reconnects[RECONNECTS_ARRAY_SIZE];
int reconnectsCnt = -1; // first needed run inkrement to 0

// blink code for status display
#define BLINK_NORMAL_CONNECTION 0    // 1 Hz blip - normal connection and running
#define BLINK_WAITING_NEXT_TRY_DTU 1 // 1 Hz - waiting for next try to connect to DTU
#define BLINK_WIFI_OFF 2             // 2 Hz - wifi off
#define BLINK_TRY_CONNECT_DTU 3      // 5 Hz - try to connect to DTU
#define BLINK_PAUSE_CLOUD_UPDATE 4   // 0,5 Hz blip - DTO - Cloud update
int8_t blinkCode = BLINK_WIFI_OFF;

Display displayOLED;
DisplayTFT displayTFT;

WiFiUDP ntpUDP;
WiFiClient dtuClient;
NTPClient timeClient(ntpUDP); // By default 'pool.ntp.org' is used with 60 seconds update interval

WiFiClient puSubClient;
// PubSubClient mqttClient(puSubClient);
MQTTHandler mqttHandler(userConfig.mqttBrokerIpDomain, userConfig.mqttBrokerPort, userConfig.mqttBrokerUser, userConfig.mqttBrokerPassword, userConfig.mqttUseTLS, espUniqueName.c_str());

ESP8266WebServer server(80);

IPAddress dtuGatewayIP;
unsigned long starttime = 0;

// intervall for getting and sending temp
// Select a Timer Clock
#define USING_TIM_DIV1 false   // for shortest and most accurate timer
#define USING_TIM_DIV16 true   // for medium time and medium accurate timer
#define USING_TIM_DIV256 false // for longest timer but least accurate. Default

// Init ESP8266 only and only Timer 1
ESP8266Timer ITimer;
#define TIMER_INTERVAL_MS 1000

const long interval50ms = 50;   // interval (milliseconds)
const long interval100ms = 100; // interval (milliseconds)
const long intervalShort = 1;   // interval (seconds)
const long interval5000ms = 5;  // interval (seconds)
unsigned long intervalMid = 32; // interval (seconds)
const long intervalLong = 60;   // interval (seconds)
unsigned long previousMillis50ms = 0;
unsigned long previousMillis100ms = 0;
unsigned long previousMillisShort = 1704063600;
unsigned long previousMillis5000ms = 1704063600;
unsigned long previousMillisMid = 1704063600;
unsigned long previousMillisLong = 1704063600;
unsigned long timeStampInSecondsDtuSynced = 1704063600;

struct controls
{
  boolean wifiSwitch = true;
  boolean getDataAuto = true;
  boolean getDataOnce = false;
  boolean dataFormatJSON = false;
};
controls globalControls;

// wifi functions
boolean wifi_connecting = false;
int wifiTimeoutShort = WIFI_RETRY_TIMEOUT_SECONDS;
int wifiTimeoutLong = WIFI_RETRY_TIME_SECONDS;

boolean checkWifiTask()
{
  if (WiFi.status() != WL_CONNECTED && !wifi_connecting) // start connecting wifi
  {
    // reconnect counter - and reset to default
    reconnects[reconnectsCnt++] = timeClient.getEpochTime();
    if (reconnectsCnt >= 25)
    {
      reconnectsCnt = 0;
      Serial.println(F("No Wifi connection after 25 tries!"));
      // after 20 reconnects inner 7 min - write defaults
      if ((timeClient.getEpochTime() - reconnects[0]) < (WIFI_RETRY_TIME_SECONDS * 1000)) //
      {
        Serial.println(F("No Wifi connection after 5 tries and inner 5 minutes"));
      }
    }

    // try to connect with current values
    Serial.println("No Wifi connection! Connecting... try to connect to wifi: '" + String(userConfig.wifiSsid) + "' with pass: '" + userConfig.wifiPassword + "'");

    WiFi.disconnect();
    WiFi.begin(userConfig.wifiSsid, userConfig.wifiPassword);
    wifi_connecting = true;
    blinkCode = BLINK_TRY_CONNECT_DTU;

    // startServices();
    return false;
  }
  else if (WiFi.status() != WL_CONNECTED && wifi_connecting && wifiTimeoutShort > 0) // check during connecting wifi and decrease for short timeout
  {
    // Serial.printf("\ncheckWifiTask - connecting - timeout: %i ", wifiTimeoutShort);
    // Serial.print(".");
    wifiTimeoutShort--;
    if (wifiTimeoutShort == 0)
    {
      Serial.println("\nstill no Wifi connection - next try in " + String(wifiTimeoutLong) + " seconds (current retry count: " + String(reconnectsCnt) + ")");
      WiFi.disconnect();
      blinkCode = BLINK_WAITING_NEXT_TRY_DTU;
    }
    return false;
  }
  else if (WiFi.status() != WL_CONNECTED && wifi_connecting && wifiTimeoutShort == 0 && wifiTimeoutLong-- <= 0) // check during connecting wifi and decrease for short timeout
  {
    Serial.println(F("\ncheckWifiTask - state 'connecting' - wait time done"));
    wifiTimeoutShort = WIFI_RETRY_TIMEOUT_SECONDS;
    wifiTimeoutLong = WIFI_RETRY_TIME_SECONDS;
    wifi_connecting = false;
    return false;
  }
  else if (WiFi.status() == WL_CONNECTED && wifi_connecting) // is connected after connecting
  {
    Serial.println(F("\ncheckWifiTask - is now connected after state: 'connecting'"));
    wifi_connecting = false;
    wifiTimeoutShort = WIFI_RETRY_TIMEOUT_SECONDS;
    wifiTimeoutLong = WIFI_RETRY_TIME_SECONDS;
    startServices();
    return true;
  }
  else if (WiFi.status() == WL_CONNECTED) // everything fine & connected
  {
    // Serial.println(F("Wifi connection: checked and fine ..."));
    blinkCode = BLINK_NORMAL_CONNECTION;
    return true;
  }
  else
  {
    return false;
  }
}

// scan network for first settings or change
int networkCount = 0;
String foundNetworks = "[{\"name\":\"empty\",\"wifi\":0,\"chan\":0}]";

boolean scanNetworksResult()
{
  int networksFound = WiFi.scanComplete();
  // print out Wi-Fi network scan result upon completion
  if (networksFound > 0)
  {
    Serial.print(F("\nscan for wifi networks done: "));
    Serial.println(String(networksFound) + " wifi's found");
    networkCount = networksFound;
    foundNetworks = "[";
    for (int i = 0; i < networksFound; i++)
    {
      int wifiPercent = 2 * (WiFi.RSSI(i) + 100);
      if (wifiPercent > 100)
      {
        wifiPercent = 100;
      }
      // Serial.printf("%d: %s, Ch:%d (%ddBm, %d) %s\n", i + 1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), wifiPercent, WiFi.encryptionType(i) == ENC_TYPE_NONE ? "open" : "");
      foundNetworks = foundNetworks + "{\"name\":\"" + WiFi.SSID(i).c_str() + "\",\"wifi\":" + wifiPercent + ",\"rssi\":" + WiFi.RSSI(i) + ",\"chan\":" + WiFi.channel(i) + "}";
      if (i < networksFound - 1)
      {
        foundNetworks = foundNetworks + ",";
      }
    }
    foundNetworks = foundNetworks + "]";
    WiFi.scanDelete();
    return true;
  }
  else
  {
    // Serial.println(F("no networks found after scanning!"));
    return false;
  }
}

// web page
// webpage
void handleRoot()
{
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", INDEX_HTML);
}
// serve json as api
void handleDataJson()
{
  String JSON = "{";
  JSON = JSON + "\"localtime\": " + String(timeStampInSecondsDtuSynced) + ",";
  JSON = JSON + "\"ntpStamp\": " + String(timeClient.getEpochTime() - userConfig.timezoneOffest) + ",";

  JSON = JSON + "\"lastResponse\": " + globalData.lastRespTimestamp + ",";
  JSON = JSON + "\"dtuConnState\": " + dtuConnection.dtuConnectState + ",";
  JSON = JSON + "\"dtuErrorState\": " + dtuConnection.dtuErrorState + ",";

  JSON = JSON + "\"starttime\": " + String(starttime - userConfig.timezoneOffest) + ",";

  JSON = JSON + "\"inverter\": {";
  JSON = JSON + "\"pLim\": " + String(globalData.powerLimit) + ",";
  JSON = JSON + "\"pLimSet\": " + String(globalData.powerLimitSet) + ",";
  JSON = JSON + "\"temp\": " + String(globalData.inverterTemp) + ",";
  JSON = JSON + "\"uptodate\": " + String(globalData.uptodate);
  JSON = JSON + "},";

  JSON = JSON + "\"grid\": {";
  JSON = JSON + "\"v\": " + String(globalData.grid.voltage) + ",";
  JSON = JSON + "\"c\": " + String(globalData.grid.current) + ",";
  JSON = JSON + "\"p\": " + String(globalData.grid.power) + ",";
  JSON = JSON + "\"dE\": " + String(globalData.grid.dailyEnergy, 3) + ",";
  JSON = JSON + "\"tE\": " + String(globalData.grid.totalEnergy, 3);
  JSON = JSON + "},";

  JSON = JSON + "\"pv0\": {";
  JSON = JSON + "\"v\": " + String(globalData.pv0.voltage) + ",";
  JSON = JSON + "\"c\": " + String(globalData.pv0.current) + ",";
  JSON = JSON + "\"p\": " + String(globalData.pv0.power) + ",";
  JSON = JSON + "\"dE\": " + String(globalData.pv0.dailyEnergy, 3) + ",";
  JSON = JSON + "\"tE\": " + String(globalData.pv0.totalEnergy, 3);
  JSON = JSON + "},";

  JSON = JSON + "\"pv1\": {";
  JSON = JSON + "\"v\": " + String(globalData.pv1.voltage) + ",";
  JSON = JSON + "\"c\": " + String(globalData.pv1.current) + ",";
  JSON = JSON + "\"p\": " + String(globalData.pv1.power) + ",";
  JSON = JSON + "\"dE\": " + String(globalData.pv1.dailyEnergy, 3) + ",";
  JSON = JSON + "\"tE\": " + String(globalData.pv1.totalEnergy, 3);
  JSON = JSON + "}";
  JSON = JSON + "}";

  server.send(200, "application/json; charset=utf-8", JSON);
}

void handleInfojson()
{
  String JSON = "{";
  JSON = JSON + "\"chipid\": " + String(chipID) + ",";
  JSON = JSON + "\"host\": \"" + espUniqueName + "\",";
  JSON = JSON + "\"initMode\": " + userConfig.wifiAPstart + ",";

  JSON = JSON + "\"firmware\": {";
  JSON = JSON + "\"version\": \"" + String(VERSION) + "\",";
  JSON = JSON + "\"versiondate\": \"" + String(BUILDTIME) + "\",";
  JSON = JSON + "\"versionServer\": \"" + String(versionServer) + "\",";
  JSON = JSON + "\"versiondateServer\": \"" + String(versiondateServer) + "\",";
  JSON = JSON + "\"versionServerRelease\": \"" + String(versionServerRelease) + "\",";
  JSON = JSON + "\"versiondateServerRelease\": \"" + String(versiondateServerRelease) + "\",";
  JSON = JSON + "\"selectedUpdateChannel\": \"" + String(userConfig.selectedUpdateChannel) + "\",";
  JSON = JSON + "\"updateAvailable\": " + updateAvailable;
  JSON = JSON + "},";

  JSON = JSON + "\"openHabConnection\": {";
  JSON = JSON + "\"ohActive\": " + userConfig.openhabActive + ",";
  JSON = JSON + "\"ohHostIp\": \"" + String(userConfig.openhabHostIpDomain) + "\",";
  JSON = JSON + "\"ohItemPrefix\": \"" + String(userConfig.openItemPrefix) + "\"";
  JSON = JSON + "},";

  JSON = JSON + "\"mqttConnection\": {";
  JSON = JSON + "\"mqttActive\": " + userConfig.mqttActive + ",";
  JSON = JSON + "\"mqttIp\": \"" + String(userConfig.mqttBrokerIpDomain) + "\",";
  JSON = JSON + "\"mqttPort\": " + String(userConfig.mqttBrokerPort) + ",";
  JSON = JSON + "\"mqttUseTLS\": " + userConfig.mqttUseTLS + ",";
  JSON = JSON + "\"mqttUser\": \"" + String(userConfig.mqttBrokerUser) + "\",";
  JSON = JSON + "\"mqttPass\": \"" + String(userConfig.mqttBrokerPassword) + "\",";
  JSON = JSON + "\"mqttMainTopic\": \"" + String(userConfig.mqttBrokerMainTopic) + "\",";
  JSON = JSON + "\"mqttHAautoDiscoveryON\": " + userConfig.mqttHAautoDiscoveryON;
  JSON = JSON + "},";

  JSON = JSON + "\"dtuConnection\": {";
  JSON = JSON + "\"dtuHostIpDomain\": \"" + String(userConfig.dtuHostIpDomain) + "\",";
  JSON = JSON + "\"dtuSsid\": \"" + String(userConfig.dtuSsid) + "\",";
  JSON = JSON + "\"dtuPassword\": \"" + String(userConfig.dtuPassword) + "\",";
  JSON = JSON + "\"dtuRssi\": " + globalData.dtuRssi + ",";
  JSON = JSON + "\"dtuDataCycle\": " + userConfig.dtuUpdateTime + ",";
  JSON = JSON + "\"dtuResetRequested\": " + globalData.dtuResetRequested + ",";
  JSON = JSON + "\"dtuCloudPause\": " + userConfig.dtuCloudPauseActive + ",";
  JSON = JSON + "\"dtuCloudPauseTime\": " + userConfig.dtuCloudPauseTime;
  JSON = JSON + "},";

  JSON = JSON + "\"wifiConnection\": {";
  JSON = JSON + "\"networkCount\": " + networkCount + ",";
  JSON = JSON + "\"foundNetworks\":" + foundNetworks + ",";
  JSON = JSON + "\"wifiSsid\": \"" + String(userConfig.wifiSsid) + "\",";
  JSON = JSON + "\"wifiPassword\": \"" + String(userConfig.wifiPassword) + "\",";
  JSON = JSON + "\"rssiGW\": " + globalData.wifi_rssi_gateway;
  JSON = JSON + "}";

  JSON = JSON + "}";

  server.send(200, "application/json; charset=utf-8", JSON);
}

void handleUpdateWifiSettings()
{
  String wifiSSIDUser = server.arg("wifiSSIDsend"); // retrieve message from webserver
  String wifiPassUser = server.arg("wifiPASSsend"); // retrieve message from webserver
  Serial.println("\nhandleUpdateWifiSettings - got WifiSSID: " + wifiSSIDUser + " - got WifiPass: " + wifiPassUser);

  wifiSSIDUser.toCharArray(userConfig.wifiSsid, sizeof(userConfig.wifiSsid));
  wifiPassUser.toCharArray(userConfig.wifiPassword, sizeof(userConfig.wifiPassword));

  // after saving from user entry - no more in init state
  if (userConfig.wifiAPstart)
  {
    userConfig.wifiAPstart = false;
    // after first startup reset to current display
    if (userConfig.displayConnected == 0)
    {
      userConfig.displayConnected = 1;
      displayTFT.setup(); // new setup to get blank screen
    }
    else if (userConfig.displayConnected == 1)
      userConfig.displayConnected = 0;
    configManager.saveConfig(userConfig);
  }

  // handleRoot();
  String JSON = "{";
  JSON = JSON + "\"wifiSSIDUser\": \"" + userConfig.wifiSsid + "\",";
  JSON = JSON + "\"wifiPassUser\": \"" + userConfig.wifiPassword + "\",";
  JSON = JSON + "}";

  server.send(200, "application/json", JSON);

  // reconnect with new values
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  checkWifiTask();

  Serial.println("handleUpdateWifiSettings - send JSON: " + String(JSON));
}

void handleUpdateDtuSettings()
{
  String dtuHostIpDomainUser = server.arg("dtuHostIpDomainSend"); // retrieve message from webserver
  String dtuDataCycle = server.arg("dtuDataCycleSend");           // retrieve message from webserver
  String dtuCloudPause = server.arg("dtuCloudPauseSend");         // retrieve message from webserver
  String dtuSSIDUser = server.arg("dtuSsidSend");                 // retrieve message from webserver
  String dtuPassUser = server.arg("dtuPasswordSend");             // retrieve message from webserver
  Serial.println("\nhandleUpdateDtuSettings - got dtu ip: " + dtuHostIpDomainUser + "- got dtuDataCycle: " + dtuDataCycle + "- got dtu dtuCloudPause: " + dtuCloudPause);
  Serial.println("handleUpdateDtuSettings - got dtu ssid: " + dtuSSIDUser + " - got WifiPass: " + dtuPassUser);

  dtuHostIpDomainUser.toCharArray(userConfig.dtuHostIpDomain, sizeof(userConfig.dtuHostIpDomain));
  userConfig.dtuUpdateTime = dtuDataCycle.toInt();
  if (dtuCloudPause)
    userConfig.dtuCloudPauseActive = true;
  else
    userConfig.dtuCloudPauseActive = false;
  dtuSSIDUser.toCharArray(userConfig.dtuSsid, sizeof(userConfig.dtuSsid));
  dtuPassUser.toCharArray(userConfig.dtuPassword, sizeof(userConfig.dtuPassword));

  configManager.saveConfig(userConfig);

  intervalMid = userConfig.dtuUpdateTime;
  dtuConnection.preventCloudErrors = userConfig.dtuCloudPauseActive;
  Serial.println("\nhandleUpdateDtuSettings - setting dtu cycle to:" + String(intervalMid));

  String JSON = "{";
  JSON = JSON + "\"dtuHostIpDomain\": \"" + userConfig.dtuHostIpDomain + "\",";
  JSON = JSON + "\"dtuSsid\": \"" + userConfig.dtuSsid + "\",";
  JSON = JSON + "\"dtuPassword\": \"" + userConfig.dtuPassword + "\"";
  JSON = JSON + "}";

  server.send(200, "application/json", JSON);

  // stopping connection to DTU - to force reconnect with new data
  dtuConnectionStop(&dtuClient, DTU_STATE_TRY_RECONNECT);
}

void handleUpdateBindingsSettings()
{
  String openhabHostIpDomainUser = server.arg("openhabHostIpDomainSend"); // retrieve message from webserver
  String openhabPrefix = server.arg("openhabPrefixSend");
  String openhabActive = server.arg("openhabActiveSend");

  String mqttIP = server.arg("mqttIpSend");
  String mqttPort = server.arg("mqttPortSend");
  String mqttUser = server.arg("mqttUserSend");
  String mqttPass = server.arg("mqttPassSend");
  String mqttMainTopic = server.arg("mqttMainTopicSend");
  String mqttActive = server.arg("mqttActiveSend");
  String mqttUseTLS = server.arg("mqttUseTLSSend");
  String mqttHAautoDiscoveryON = server.arg("mqttHAautoDiscoveryONSend");
  bool mqttHAautoDiscoveryONlastState = userConfig.mqttHAautoDiscoveryON;

  Serial.println("handleUpdateBindingsSettings - HAautoDiscovery current state: " + String(mqttHAautoDiscoveryONlastState));

  openhabHostIpDomainUser.toCharArray(userConfig.openhabHostIpDomain, sizeof(userConfig.openhabHostIpDomain));
  openhabPrefix.toCharArray(userConfig.openItemPrefix, sizeof(userConfig.openItemPrefix));

  if (openhabActive == "1")
    userConfig.openhabActive = true;
  else
    userConfig.openhabActive = false;

  mqttIP.toCharArray(userConfig.mqttBrokerIpDomain, sizeof(userConfig.mqttBrokerIpDomain));
  userConfig.mqttBrokerPort = mqttPort.toInt();
  mqttUser.toCharArray(userConfig.mqttBrokerUser, sizeof(userConfig.mqttBrokerUser));
  mqttPass.toCharArray(userConfig.mqttBrokerPassword, sizeof(userConfig.mqttBrokerPassword));
  mqttMainTopic.toCharArray(userConfig.mqttBrokerMainTopic, sizeof(userConfig.mqttBrokerMainTopic));

  if (mqttActive == "1")
    userConfig.mqttActive = true;
  else
    userConfig.mqttActive = false;

  if (mqttUseTLS == "1")
    userConfig.mqttUseTLS = true;
  else
    userConfig.mqttUseTLS = false;

  if (mqttHAautoDiscoveryON == "1")
    userConfig.mqttHAautoDiscoveryON = true;
  else
    userConfig.mqttHAautoDiscoveryON = false;

  configManager.saveConfig(userConfig);

  // changing to given mqtt setting - inlcuding reset the connection
  mqttHandler.setBroker(userConfig.mqttBrokerIpDomain);
  mqttHandler.setPort(userConfig.mqttBrokerPort);
  mqttHandler.setUser(userConfig.mqttBrokerUser);
  mqttHandler.setPassword(userConfig.mqttBrokerPassword);
  mqttHandler.setUseTLS(userConfig.mqttUseTLS); // Enable TLS

  Serial.println("handleUpdateBindingsSettings - HAautoDiscovery new state: " + String(userConfig.mqttHAautoDiscoveryON));
  // mqttHAautoDiscoveryON going from on to off - send one time the delete messages
  if (!userConfig.mqttHAautoDiscoveryON && mqttHAautoDiscoveryONlastState)
    mqttHandler.reconnect(userConfig.mqttHAautoDiscoveryON, userConfig.mqttBrokerMainTopic, true, dtuGatewayIP.toString());
  else
    mqttHandler.reconnect(userConfig.mqttHAautoDiscoveryON, userConfig.mqttBrokerMainTopic, false, dtuGatewayIP.toString());

  String JSON = "{";
  JSON = JSON + "\"openhabActive\": " + userConfig.openhabActive + ",";
  JSON = JSON + "\"openhabHostIpDomain\": \"" + userConfig.openhabHostIpDomain + "\",";
  JSON = JSON + "\"openItemPrefix\": \"" + userConfig.openItemPrefix + "\",";
  JSON = JSON + "\"mqttActive\": " + userConfig.mqttActive + ",";
  JSON = JSON + "\"mqttBrokerIpDomain\": \"" + userConfig.mqttBrokerIpDomain + "\",";
  JSON = JSON + "\"mqttBrokerPort\": " + String(userConfig.mqttBrokerPort) + ",";
  JSON = JSON + "\"mqttUseTLS\": " + userConfig.mqttUseTLS + ",";
  JSON = JSON + "\"mqttBrokerUser\": \"" + userConfig.mqttBrokerUser + "\",";
  JSON = JSON + "\"mqttBrokerPassword\": \"" + userConfig.mqttBrokerPassword + "\",";
  JSON = JSON + "\"mqttBrokerMainTopic\": \"" + userConfig.mqttBrokerMainTopic + "\",";
  JSON = JSON + "\"mqttHAautoDiscoveryON\": " + userConfig.mqttHAautoDiscoveryON;

  JSON = JSON + "}";

  server.send(200, "application/json", JSON);
  Serial.println("handleUpdateBindingsSettings - send JSON: " + String(JSON));
}

void handleUpdatePowerLimit()
{
  String powerLimitSetNew = server.arg("powerLimitSend"); // retrieve message from webserver
  Serial.println("\nhandleUpdatePowerLimit - got powerLimitSend: " + powerLimitSetNew);
  uint8_t gotLimit;
  bool conversionSuccess = false;

  if (powerLimitSetNew.length() > 0)
  {
    gotLimit = powerLimitSetNew.toInt();
    // Check if the conversion was successful by comparing the string with its integer representation, to avoid wronmg interpretations of 0 after toInt by a "no number string"
    conversionSuccess = (String(gotLimit) == powerLimitSetNew);
  }

  if (conversionSuccess)
  {
    if (gotLimit < 2)
      globalData.powerLimitSet = 2;
    else if (gotLimit > 100)
      globalData.powerLimitSet = 2;
    else
      globalData.powerLimitSet = gotLimit;

    // Serial.print("got SetLimit: " + String(globalData.powerLimitSet) + " - current limit: " + String(globalData.powerLimit) + " %");
  
    String JSON = "{";
    JSON = JSON + "\"PowerLimitSet\": \"" + globalData.powerLimitSet + "\"";
    JSON = JSON + "}";

    server.send(200, "application/json", JSON);
    Serial.println("handleUpdatePowerLimit - send JSON: " + String(JSON));
  }
  else
  {
    Serial.print("got wrong data for SetLimit: " + powerLimitSetNew);
      
      server.send(400, "text/plain", "powerLimit out of range");
      return;
  }
  
  // trigger new update info with changed release channel
  // getUpdateInfo(AsyncWebServerRequest *request);
  //updateInfoRequested = true;
}

void handleUpdateOTASettings()
{
  String releaseChannel = server.arg("releaseChannel"); // retrieve message from webserver
  Serial.println("\nhandleUpdateOTASettings - got releaseChannel: " + releaseChannel);

  userConfig.selectedUpdateChannel = releaseChannel.toInt();

  configManager.saveConfig(userConfig);

  String JSON = "{";
  JSON = JSON + "\"releaseChannel\": \"" + userConfig.selectedUpdateChannel + "\"";
  JSON = JSON + "}";

  server.send(200, "application/json", JSON);
  Serial.println("handleUpdateDtuSettings - send JSON: " + String(JSON));

  // trigger new update info with changed release channel
  // getUpdateInfo(AsyncWebServerRequest *request);
  updateInfoRequested = true;
}

void handleConfigPage()
{
  JsonDocument doc;
  bool gotUserChanges = false;

  if (server.args() && server.hasArg("local.wifiAPstart") && server.arg("local.wifiAPstart") == "false")
  {
    gotUserChanges = true;

    for (int i = 0; i < server.args(); i++)
    {
      String key = server.argName(i);
      String value = server.arg(key);
      String key1 = key.substring(0, key.indexOf("."));
      String key2 = key.substring(key.indexOf(".") + 1);

      if (value == "false" || value == "true")
      {
        bool boolValue = (value == "true");
        doc[key1][key2] = boolValue;
      }
      else
        doc[key1][key2] = value;
    }
  }

  String html = configManager.getWebHandler(doc);
  server.send(200, "text/html", html);

  delay(1000);
  if (gotUserChanges)
    ESP.restart();
}

// webserver port 80

void initializeWebServer()
{
  server.on("/", HTTP_GET, handleRoot);

  server.on("/jquery.min.js", HTTP_GET, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", JQUERY_MIN_JS); });

  server.on("/style.css", HTTP_GET, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", STYLE_CSS); });

  server.on("/updateWifiSettings", handleUpdateWifiSettings);
  server.on("/updateDtuSettings", handleUpdateDtuSettings);
  server.on("/updateOTASettings", handleUpdateOTASettings);
  server.on("/updateBindingsSettings", handleUpdateBindingsSettings);
  server.on("/updatePowerLimit", handleUpdatePowerLimit);

  // api GETs
  server.on("/api/data", handleDataJson);
  server.on("/api/info", handleInfojson);

  // OTA update
  server.on("/updateGetInfo", requestUpdateInfo);
  server.on("/updateRequest", handleUpdateRequest);

  server.on("/config", handleConfigPage);

  server.begin();
}

// OTA
// ---> /updateRequest
void handleUpdateRequest()
{
  String urlToBin = "";
  if (userConfig.selectedUpdateChannel == 0)
    urlToBin = updateURLRelease;
  else
    urlToBin = updateURL;

  BearSSL::WiFiClientSecure updateclient;
  updateclient.setInsecure();

  if (urlToBin == "" || updateAvailable != true)
  {
    Serial.println(F("[update] no url given or no update available"));
    return;
  }

  server.sendHeader("Connection", "close");
  server.send(200, "application/json", "{\"update\": \"in_progress\"}");

  Serial.println(F("[update] Update requested"));
  Serial.println("[update] try download from " + urlToBin);

  // wait to seconds to load css on client side
  Serial.println(F("[update] starting update"));

  ESPhttpUpdate.onStart(update_started);
  ESPhttpUpdate.onEnd(update_finished);
  ESPhttpUpdate.onProgress(update_progress);
  ESPhttpUpdate.onError(update_error);
  ESPhttpUpdate.closeConnectionsOnUpdate(false);

  // ESPhttpUpdate.rebootOnUpdate(false); // remove automatic update

  Serial.println(F("[update] starting update"));
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  updateRunning = true;

  // stopping all services to prevent OOM/ stackoverflow
  timeClient.end();
  ntpUDP.stopAll();
  puSubClient.stopAll();
  dtuClient.stopAll();
  MDNS.close();
  server.stop();
  server.close();

  t_httpUpdate_return ret = ESPhttpUpdate.update(updateclient, urlToBin);

  switch (ret)
  {
  case HTTP_UPDATE_FAILED:
    Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
    Serial.println(F("[update] Update failed."));
    // restart all services if failed
    initializeWebServer(); // starting server again
    startServices();
    updateRunning = false;
    break;
  case HTTP_UPDATE_NO_UPDATES:
    Serial.println(F("[update] Update no Update."));
    break;
  case HTTP_UPDATE_OK:
    Serial.println(F("[update] Update ok.")); // may not be called since we reboot the ESP
    break;
  }
  Serial.println("[update] Update routine done - ReturnCode: " + String(ret));
}

//
void requestUpdateInfo()
{
  updateInfoRequested = true;
  server.send(200, "application/json", "{\"updateInfoRequested\": \"done\"}");
}

// get the info about update from remote
boolean getUpdateInfo()
{
  String versionUrl = "";
  std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
  secClient->setInsecure();

  if (userConfig.selectedUpdateChannel == 0)
  {
    versionUrl = updateInfoWebPathRelease;
  }
  else
  {
    versionUrl = updateInfoWebPath;
  }

  Serial.print("\n---> getUpdateInfo - check for: " + versionUrl + "\n");

  // create an HTTPClient instance
  HTTPClient https;

  // Initializing an HTTPS communication using the secure client
  if (https.begin(*secClient, versionUrl))
  { // HTTPS
    Serial.print(F("\n---> getUpdateInfo - https connected\n"));
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Enable automatic following of redirects
    int httpCode = https.GET();
    Serial.println("\n---> getUpdateInfo - got http ret code:" + String(httpCode));

    // httpCode will be negative on error
    if (httpCode > 0)
    {
      // HTTP header has been send and Server response header has been handled
      // file found at server
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
      {
        String payload = https.getString();

        // Parse JSON using ArduinoJson library
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        // Test if parsing succeeds.
        if (error)
        {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.f_str());
          server.sendHeader("Connection", "close");
          server.send(200, "application/json", "{\"updateRequest\": \"" + String(error.f_str()) + "\"}");
          return false;
        }
        else
        {
          // for special versions: develop, feature, localDev the version has to be truncated
          String localVersion = String(VERSION);
          if (localVersion.indexOf("_"))
          {
            localVersion = localVersion.substring(0, localVersion.indexOf("_"));
          }

          if (userConfig.selectedUpdateChannel == 0)
          {
            strcpy(versionServerRelease, (const char *)(doc["version"]));
            strcpy(versiondateServerRelease, (const char *)(doc["versiondate"]));
            strcpy(updateURLRelease, (const char *)(doc["link"]));
            updateAvailable = checkVersion(localVersion, versionServerRelease);
          }
          else
          {
            strcpy(versionServer, (const char *)(doc["version"]));
            String versionSnapshot = versionServer;
            if (versionSnapshot.indexOf("_"))
            {
              versionSnapshot = versionSnapshot.substring(0, versionSnapshot.indexOf("_"));
            }

            strcpy(versiondateServer, (const char *)(doc["versiondate"]));
            strcpy(updateURL, (const char *)(doc["linksnapshot"]));
            updateAvailable = checkVersion(localVersion, versionSnapshot);
          }

          server.sendHeader("Connection", "close");
          server.send(200, "application/json", "{\"updateRequest\": \"done\"}");
        }
      }
    }
    else
    {
      Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    secClient->stop();
    https.end();
  }
  else
  {
    Serial.println(F("\ngetUpdateInfo - [HTTPS] Unable to connect to server"));
  }
  // secClient->stopAll();
  updateInfoRequested = false;
  return true;
}

// check version local with remote
boolean checkVersion(String v1, String v2)
{
  Serial.println("\ncompare versions: " + String(v1) + " - " + String(v2));
  // Method to compare two versions.
  // Returns 1 if v2 is smaller, -1
  // if v1 is smaller, 0 if equal
  // int result = 0;
  int vnum1 = 0, vnum2 = 0;

  // loop until both string are
  // processed
  for (unsigned int i = 0, j = 0; (i < v1.length() || j < v2.length());)
  {
    // storing numeric part of
    // version 1 in vnum1
    while (i < v1.length() && v1[i] != '.')
    {
      vnum1 = vnum1 * 10 + (v1[i] - '0');
      i++;
    }

    // storing numeric part of
    // version 2 in vnum2
    while (j < v2.length() && v2[j] != '.')
    {
      vnum2 = vnum2 * 10 + (v2[j] - '0');
      j++;
    }

    if (vnum1 > vnum2)
    {
      // result = 1; // v2 is smaller
      // Serial.println("vgl (i=" + String(i) + ") v2 smaller - vnum1 " + String(vnum1) + " - " + String(vnum2));
      return false;
    }

    if (vnum2 > vnum1)
    {
      // result = -1; // v1 is smaller
      // Serial.println("vgl (i=" + String(i) + ") v1 smaller - vnum1 " + String(vnum1) + " - " + String(vnum2));
      return true;
    }

    // if equal, reset variables and
    // go for next numeric part
    // Serial.println("vgl (i=" + String(i) + ") v1 equal 2 - vnum1 " + String(vnum1) + " - " + String(vnum2));
    vnum1 = vnum2 = 0;
    i++;
    j++;
  }
  // 0 if equal
  return false;
}

void update_started()
{
  Serial.println(F("CALLBACK:  HTTP update process started"));
  strcpy(updateState, "started");
}
void update_finished()
{
  Serial.println(F("CALLBACK:  HTTP update process finished"));
  strcpy(updateState, "done");
}
void update_progress(int cur, int total)
{
  updateProgress = ((float)cur / (float)total) * 100;
  strcpy(updateState, "running");
  Serial.print("CALLBACK:  HTTP update process at " + String(cur) + "  of " + String(total) + " bytes - " + String(updateProgress, 1) + " %\n");
}
void update_error(int err)
{
  Serial.printf("CALLBACK:  HTTP update fatal error code %d\n", err);
  strcpy(updateState, "error");
}

// send values to openhab

boolean postMessageToOpenhab(String key, String value)
{
  WiFiClient client;
  HTTPClient http;
  String openhabHost = "http://" + String(userConfig.openhabHostIpDomain) + ":8080/rest/items/";
  http.setTimeout(1000); // prevent blocking of progam
  // Serial.print("postMessageToOpenhab (" + openhabHost + ") - " + key + " -> " + value);
  if (http.begin(client, openhabHost + key))
  {
    http.addHeader("Content-Type", "text/plain");
    http.addHeader("Accept", "application/json");

    int httpCode = http.POST(value);
    // Check for timeout
    if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED || httpCode == HTTPC_ERROR_SEND_HEADER_FAILED ||
        httpCode == HTTPC_ERROR_SEND_PAYLOAD_FAILED)
    {
      Serial.print("\n[HTTP] postMessageToOpenhab (" + key + ") Timeout error: " + String(httpCode) + "\n");
      http.end();
      return false; // Return timeout error
    }

    http.writeToStream(&Serial);
    http.end();
    return true;
  }
  else
  {
    Serial.print("[HTTP] postMessageToOpenhab Unable to connect " + openhabHost + " \n");
    return false;
  }
}

String getMessageFromOpenhab(String key)
{
  WiFiClient client;
  HTTPClient http;
  if (WiFi.status() == WL_CONNECTED)
  {
    String openhabHost = "http://" + String(userConfig.openhabHostIpDomain) + ":8080/rest/items/";
    http.setTimeout(2000); // prevent blocking of progam
    if (http.begin(client, openhabHost + key + "/state"))
    {
      String payload = "";
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK)
      {
        payload = http.getString();
      }
      http.end();
      return payload;
    }
    else
    {
      Serial.print("[HTTP] getMessageFromOpenhab Unable to connect " + openhabHost + " \n");
      return "connectError";
    }
  }
  else
  {
    Serial.print("getMessageFromOpenhab - can not connect to openhab - wifi not connected \n");
    return "connectError";
  }
}

boolean getPowerSetDataFromOpenHab()
{
  // get data from openhab if connected to DTU
  // if (dtuConnection.dtuConnectState == DTU_STATE_CONNECTED)
  // {
  uint8_t gotLimit;
  bool conversionSuccess = false;

  String openhabMessage = getMessageFromOpenhab(String(userConfig.openItemPrefix) + "_PowerLimit_Set");
  if (openhabMessage.length() > 0)
  {
    gotLimit = openhabMessage.toInt();
    // Check if the conversion was successful by comparing the string with its integer representation, to avoid wronmg interpretations of 0 after toInt by a "no number string"
    conversionSuccess = (String(gotLimit) == openhabMessage);
  }

  if (conversionSuccess)
  {
    if (gotLimit < 2)
      globalData.powerLimitSet = 2;
    else if (gotLimit > 100)
      globalData.powerLimitSet = 2;
    else
      globalData.powerLimitSet = gotLimit;
  }
  else
  {
    Serial.print("got wrong data for SetLimit: " + openhabMessage);
    return false;
  }
  // Serial.print("got SetLimit: " + String(globalData.powerLimitSet) + " - current limit: " + String(globalData.powerLimit) + " %");
  return true;
  // }
  // return false;
}

boolean updateValueToOpenhab()
{
  boolean sendOk = postMessageToOpenhab(String(userConfig.openItemPrefix) + "Grid_U", (String)globalData.grid.voltage);
  if (sendOk)
  {
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "Grid_I", (String)globalData.grid.current);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "Grid_P", (String)globalData.grid.power);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV_E_day", String(globalData.grid.dailyEnergy, 3));
    if (globalData.grid.totalEnergy != 0)
    {
      postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV_E_total", String(globalData.grid.totalEnergy, 3));
    }

    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV1_U", (String)globalData.pv0.voltage);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV1_I", (String)globalData.pv0.current);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV1_P", (String)globalData.pv0.power);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV1_E_day", String(globalData.pv0.dailyEnergy, 3));
    if (globalData.pv0.totalEnergy != 0)
    {
      postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV1_E_total", String(globalData.pv0.totalEnergy, 3));
    }

    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV2_U", (String)globalData.pv1.voltage);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV2_I", (String)globalData.pv1.current);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV2_P", (String)globalData.pv1.power);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV2_E_day", String(globalData.pv1.dailyEnergy, 3));
    if (globalData.pv1.totalEnergy != 0)
    {
      postMessageToOpenhab(String(userConfig.openItemPrefix) + "PV2_E_total", String(globalData.pv1.totalEnergy, 3));
    }

    postMessageToOpenhab(String(userConfig.openItemPrefix) + "_Temp", (String)globalData.inverterTemp);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "_PowerLimit", (String)globalData.powerLimit);
    postMessageToOpenhab(String(userConfig.openItemPrefix) + "_WifiRSSI", (String)globalData.dtuRssi);
  }
  Serial.println("\nsent values to openHAB");
  return true;
}

// mqtt client

// publishing data in standard or HA mqtt auto discovery format
void updateValuesToMqtt(boolean haAutoDiscovery = false)
{
  Serial.println("\nMQTT: publish data (HA autoDiscovery = " + String(haAutoDiscovery) + ")");
  std::map<std::string, std::string> keyValueStore;

  keyValueStore["time_stamp"] = String(timeStampInSecondsDtuSynced).c_str();

  keyValueStore["grid_U"] = String(globalData.grid.voltage).c_str();
  keyValueStore["grid_I"] = String(globalData.grid.current).c_str();
  keyValueStore["grid_P"] = String(globalData.grid.power).c_str();
  keyValueStore["grid_dailyEnergy"] = String(globalData.grid.dailyEnergy, 3).c_str();
  if (globalData.grid.totalEnergy != 0)
    keyValueStore["grid_totalEnergy"] = String(globalData.grid.totalEnergy, 3).c_str();

  keyValueStore["pv0_U"] = String(globalData.pv0.voltage).c_str();
  keyValueStore["pv0_I"] = String(globalData.pv0.current).c_str();
  keyValueStore["pv0_P"] = String(globalData.pv0.power).c_str();
  keyValueStore["pv0_dailyEnergy"] = String(globalData.pv0.dailyEnergy, 3).c_str();
  if (globalData.pv0.totalEnergy != 0)
    keyValueStore["pv0_totalEnergy"] = String(globalData.pv0.totalEnergy, 3).c_str();

  keyValueStore["pv1_U"] = String(globalData.pv1.voltage).c_str();
  keyValueStore["pv1_I"] = String(globalData.pv1.current).c_str();
  keyValueStore["pv1_P"] = String(globalData.pv1.power).c_str();
  keyValueStore["pv1_dailyEnergy"] = String(globalData.pv1.dailyEnergy, 3).c_str();
  if (globalData.pv0.totalEnergy != 0)
    keyValueStore["pv1_totalEnergy"] = String(globalData.pv1.totalEnergy, 3).c_str();

  keyValueStore["inverter_Temp"] = String(globalData.inverterTemp).c_str();
  keyValueStore["inverter_PowerLimit"] = String(globalData.powerLimit).c_str();
  keyValueStore["inverter_WifiRSSI"] = String(globalData.dtuRssi).c_str();

  for (const auto &pair : keyValueStore)
  {
    String subtopic = (pair.first).c_str();
    subtopic.replace("_", "/");
    mqttHandler.publishStandardData(String(userConfig.mqttBrokerMainTopic) + "/" + subtopic, (pair.second).c_str());
  }
}

// ****

void setup()
{
  // switch off SCK LED
  // pinMode(14, OUTPUT);
  // digitalWrite(14, LOW);

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // turn the LED off by making the voltage LOW

  Serial.begin(115200);
  Serial.print(F("\nBooting - with firmware version "));
  Serial.println(VERSION);

  if (!configManager.begin())
  {
    Serial.println("Failed to initialize UserConfigManager");
    return;
  }

  if (configManager.loadConfig(userConfig))
    configManager.printConfigdata();
  else
    Serial.println("Failed to load user config");
  // ------- user config loaded --------------------------------------------

  // init display according to userConfig
  if (userConfig.displayConnected == 0)
    displayOLED.setup();
  else if (userConfig.displayConnected == 1)
    displayTFT.setup();

  if (userConfig.wifiAPstart)
  {
    Serial.println(F("\n+++ device in 'first start' mode - have to be initialized over own served wifi +++\n"));
    // first scan of networks - synchronous
    // scanNetworksResult(WiFi.scanNetworks());

    WiFi.scanNetworks();
    scanNetworksResult();

    // Connect to Wi-Fi as AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(espUniqueName);
    Serial.println("\n +++ serving access point with SSID: '" + espUniqueName + "' +++\n");

    // IP Address of the ESP8266 on the AP network
    IPAddress apIP = WiFi.softAPIP();
    Serial.print(F("AP IP address: "));
    Serial.println(apIP);

    MDNS.begin("dtuGateway");
    MDNS.addService("http", "tcp", 80);
    Serial.println(F("Ready! Open http://dtuGateway.local in your browser"));

    // display - change every reboot in first start mode
    if (userConfig.displayConnected == 0)
    {
      displayOLED.drawFactoryMode(String(VERSION), espUniqueName, apIP.toString());
      userConfig.displayConnected = 1;
    }
    else if (userConfig.displayConnected == 1)
    {
      displayTFT.drawFactoryMode(String(VERSION), espUniqueName, apIP.toString());
      userConfig.displayConnected = 0;
    }
    // deafult setting for mqtt main topic
    ("dtu_" + String(chipID)).toCharArray(userConfig.mqttBrokerMainTopic, sizeof(userConfig.mqttBrokerMainTopic));
    configManager.saveConfig(userConfig);

    initializeWebServer();
  }
  else
  {
    WiFi.mode(WIFI_STA);
  }

  // CRC for protobuf
  initializeCRC();

  // setting startup interval for dtucylce
  intervalMid = userConfig.dtuUpdateTime;
  Serial.print(F("\nsetup - setting dtu cycle to:"));
  Serial.println(intervalMid);

  // setting startup for dtu cloud pause
  dtuConnection.preventCloudErrors = userConfig.dtuCloudPauseActive;

  // Interval in microsecs
  if (ITimer.setInterval(TIMER_INTERVAL_MS * 1000, timer1000MilliSeconds))
  {
    unsigned long lastMillis = millis();
    Serial.print(F("Starting  ITimer OK, millis() = "));
    Serial.println(lastMillis);
  }
  else
    Serial.println(F("Can't set ITimer correctly. Select another freq. or interval"));
}
// after startup or reconnect with wifi
void startServices()
{
  if (WiFi.waitForConnectResult() == WL_CONNECTED)
  {
    Serial.print(F("\nConnected! IP address: "));
    dtuGatewayIP = WiFi.localIP();
    Serial.println(dtuGatewayIP.toString());
    Serial.print(F("IP address of gateway: "));
    Serial.println(WiFi.gatewayIP());

    httpUpdater.setup(&server);

    MDNS.begin(espUniqueName);
    MDNS.addService("http", "tcp", 80);
    Serial.println("Ready! Open http://" + espUniqueName + ".local in your browser");

    // ntp time - offset in summertime 7200 else 3600
    timeClient.begin();
    timeClient.setTimeOffset(userConfig.timezoneOffest);
    // get first time
    timeClient.update();
    starttime = timeClient.getEpochTime();
    Serial.print(F("got time from time server: "));
    Serial.println(String(starttime));

    // start first search for available wifi networks
    WiFi.scanNetworks(true);

    initializeWebServer();

    if (userConfig.mqttActive)
    {
      Serial.println(F("MQTT: setup ..."));
      mqttHandler.setup(userConfig.mqttHAautoDiscoveryON);
    }
  }
  else
  {
    Serial.println(F("WiFi Failed"));
  }
}

uint16_t ledCycle = 0;
void blinkCodeTask()
{
  int8_t ledOffCount = 2;
  int8_t ledOffReset = 11;

  ledCycle++;
  if (blinkCode == BLINK_NORMAL_CONNECTION) // Blip every 5 sec
  {
    ledOffCount = 2;  // 200 ms
    ledOffReset = 50; // 5000 ms
  }
  else if (blinkCode == BLINK_WAITING_NEXT_TRY_DTU) // 0,5 Hz
  {
    ledOffCount = 10; // 1000 ms
    ledOffReset = 20; // 2000 ms
  }
  else if (blinkCode == BLINK_WIFI_OFF) // long Blip every 5 sec
  {
    ledOffCount = 5;  // 500 ms
    ledOffReset = 50; // 5000 ms
  }
  else if (blinkCode == BLINK_TRY_CONNECT_DTU) // 5 Hz
  {
    ledOffCount = 2; // 200 ms
    ledOffReset = 2; // 200 ms
  }
  else if (blinkCode == BLINK_PAUSE_CLOUD_UPDATE) // Blip every 2 sec
  {
    ledOffCount = 2;  // 200 ms
    ledOffReset = 21; // 2000 ms
  }

  if (ledCycle == 1)
  {
    digitalWrite(LED_BUILTIN, LOW); // turn the LED off by making the voltage LOW
  }
  else if (ledCycle == ledOffCount)
  {
    digitalWrite(LED_BUILTIN, HIGH); // turn the LED on (HIGH is the voltage level)
  }
  if (ledCycle >= ledOffReset)
  {
    ledCycle = 0;
  }
}

// serial comm
String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++)
  {
    if (data.charAt(i) == separator || i == maxIndex)
    {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }

  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void serialInputTask()
{
  // Check to see if anything is available in the serial receive buffer
  if (Serial.available() > 0)
  {
    static char message[20];
    static unsigned int message_pos = 0;
    char inByte = Serial.read();
    if (inByte != '\n' && (message_pos < 20 - 1))
    {
      message[message_pos] = inByte;
      message_pos++;
    }
    else // Full message received...
    {
      // Add null character to string
      message[message_pos] = '\0';
      // Print the message (or do other things)
      Serial.print(F("GotCmd: "));
      Serial.println(message);
      getSerialCommand(getValue(message, ' ', 0), getValue(message, ' ', 1));
      // Reset for the next message
      message_pos = 0;
    }
  }
}

void getSerialCommand(String cmd, String value)
{
  int val = value.toInt();
  Serial.print(F("CmdOut: "));
  if (cmd == "setPower")
  {
    Serial.print(F("'setPower' to "));
    globalData.powerLimitSet = val;
    Serial.print(String(globalData.powerLimitSet));
  }
  else if (cmd == "getDataAuto")
  {
    Serial.print(F("'getDataAuto' to "));
    if (val == 1)
    {
      globalControls.getDataAuto = true;
      Serial.print(F(" 'ON' "));
    }
    else
    {
      globalControls.getDataAuto = false;
      Serial.print(F(" 'OFF' "));
    }
  }
  else if (cmd == "getDataOnce")
  {
    Serial.print(F("'getDataOnce' to "));
    if (val == 1)
    {
      globalControls.getDataOnce = true;
      Serial.print(F(" 'ON' "));
    }
    else
    {
      globalControls.getDataOnce = false;
      Serial.print(F(" 'OFF' "));
    }
  }
  else if (cmd == "dataFormatJSON")
  {
    Serial.print(F("'dataFormatJSON' to "));
    if (val == 1)
    {
      globalControls.dataFormatJSON = true;
      Serial.print(F(" 'ON' "));
    }
    else
    {
      globalControls.dataFormatJSON = false;
      Serial.print(F(" 'OFF' "));
    }
  }
  else if (cmd == "setWifi")
  {
    Serial.print(F("'setWifi' to "));
    if (val == 1)
    {
      globalControls.wifiSwitch = true;
      Serial.print(F(" 'ON' "));
    }
    else
    {
      globalControls.wifiSwitch = false;
      blinkCode = BLINK_WIFI_OFF;
      Serial.print(F(" 'OFF' "));
    }
  }
  else if (cmd == "setInterval")
  {
    intervalMid = long(val);
    Serial.print("'setInterval' to " + String(intervalMid));
  }
  else if (cmd == "getInterval")
  {
    Serial.print("'getInterval' => " + String(intervalMid));
  }
  else if (cmd == "setCloudSave")
  {
    Serial.print(F("'setCloudSave' to "));
    if (val == 1)
    {
      dtuConnection.preventCloudErrors = true;
      Serial.print(F(" 'ON' "));
    }
    else
    {
      dtuConnection.preventCloudErrors = false;
      Serial.print(F(" 'OFF' "));
    }
  }
  else if (cmd == "resetToFactory")
  {
    Serial.print(F("'resetToFactory' to "));
    if (val == 1)
    {
      configManager.resetConfig();
      Serial.print(F(" reinitialize UserConfig data and reboot ... "));
      ESP.restart();
    }
  }
  else if (cmd == "rebootDevice")
  {
    Serial.print(F(" rebootDevice "));
    if (val == 1)
    {
      Serial.print(F(" ... rebooting ... "));
      ESP.restart();
    }
  }
  else if (cmd == "rebootDTU")
  {
    Serial.print(F(" rebootDTU "));
    if (val == 1)
    {
      Serial.print(F(" send reboot request "));
      writeCommandRestartDevice(&dtuClient, timeStampInSecondsDtuSynced);
    }
  }
  else if (cmd == "selectDisplay")
  {
    Serial.print(F(" selected Display"));
    if (val == 0)
    {
      userConfig.displayConnected = 0;
      Serial.print(F(" OLED"));
    }
    else if (val == 1)
    {
      userConfig.displayConnected = 1;
      Serial.print(F(" ROUND TFT 1.28"));
    }
    configManager.saveConfig(userConfig);
    configManager.printConfigdata();
    Serial.println(F("restart the device to make the changes take effect"));
    ESP.restart();
  }
  else
  {
    Serial.print(F("Cmd not recognized\n"));
  }
  Serial.print(F("\n"));
}

// main

// get precise localtime - increment
void IRAM_ATTR timer1000MilliSeconds()
{
  // localtime counter - increase every second
  timeStampInSecondsDtuSynced++;
}

void loop()
{
  unsigned long currentMillis = millis();
  // skip all tasks if update is running
  if (updateRunning)
    return;

  if (WiFi.status() == WL_CONNECTED)
  {
    // web server runner
    server.handleClient();
    // serving domain name
    MDNS.update();

    // runner for mqttClient to hold a already etablished connection
    if (userConfig.mqttActive)
    {
      mqttHandler.loop(userConfig.mqttHAautoDiscoveryON, userConfig.mqttBrokerMainTopic, dtuGatewayIP.toString());
    }
  }

  // 50ms task
  if (currentMillis - previousMillis50ms >= interval50ms)
  {
    previousMillis50ms = currentMillis;
    // -------->
    if (!userConfig.wifiAPstart)
    {
      // display tasks every 50ms = 20Hz
      if (userConfig.displayConnected == 0)
        displayOLED.renderScreen(timeClient.getFormattedTime(), String(VERSION));
      else if (userConfig.displayConnected == 1)
        displayTFT.renderScreen(timeClient.getFormattedTime(), String(VERSION));
    }
  }

  // 100ms task
  if (currentMillis - previousMillis100ms >= interval100ms)
  {
    previousMillis100ms = currentMillis;
    // -------->
    blinkCodeTask();
    serialInputTask();

    if (userConfig.mqttActive)
    {
      // getting powerlimitSet over MQTT, only on demand - to avoid oversteering for openhab receiving with constant MQTT values, if both bindings are active
      // the time difference between publishing and take over have to be less then 100 ms
      PowerLimitSet lastSetting = mqttHandler.getPowerLimitSet();
      if (currentMillis - lastSetting.timestamp < 100)
      {
        globalData.powerLimitSet = lastSetting.setValue;
        Serial.println("\nMQTT: changed powerset value to '" + String(globalData.powerLimitSet) + "'");
      }
    }
  }

  // CHANGE to precise 1 second timer increment
  currentMillis = timeStampInSecondsDtuSynced;

  // short task
  if (currentMillis - previousMillisShort >= intervalShort)
  {
    // Serial.printf("\n>>>>> %02is task - state --> ", int(intervalShort));
    // Serial.print("local: " + getTimeStringByTimestamp(timeStampInSecondsDtuSynced));
    // Serial.print(" --- NTP: " + timeClient.getFormattedTime() + " --- currentMillis " + String(currentMillis) + " --- ");
    previousMillisShort = currentMillis;
    // Serial.print(F("free mem: "));
    // Serial.print(ESP.getFreeHeap());
    // Serial.print(F(" - heap fragm: "));
    // Serial.print(ESP.getHeapFragmentation());
    // Serial.print(F(" - max free block size: "));
    // Serial.print(ESP.getMaxFreeBlockSize());
    // Serial.print(F(" - free cont stack: "));
    // Serial.print(ESP.getFreeContStack());
    // Serial.print(F(" \n"));

    // -------->

    if (globalControls.wifiSwitch && !userConfig.wifiAPstart)
      checkWifiTask();
    else
    {
      // stopping connection to DTU before go wifi offline
      dtuConnectionStop(&dtuClient, DTU_STATE_OFFLINE);
      WiFi.disconnect();
    }

    if (dtuConnection.preventCloudErrors)
    {
      // task to check and change for cloud update pause
      if (dtuCloudPauseActiveControl(timeStampInSecondsDtuSynced))
      {
        globalData.uptodate = false;
        blinkCode = BLINK_PAUSE_CLOUD_UPDATE;
        dtuConnectionStop(&dtuClient, DTU_STATE_CLOUD_PAUSE); // disconnet DTU server, if prevention on
      }
    }

    if (userConfig.openhabActive)
      getPowerSetDataFromOpenHab();

    // direct request of new powerLimit
    if (globalData.powerLimitSet != globalData.powerLimit && globalData.powerLimitSet != 101 && globalData.uptodate)
    {
      Serial.print("\n----- ----- set new power limit from " + String(globalData.powerLimit) + " to " + String(globalData.powerLimitSet));
      if (writeReqCommand(&dtuClient, globalData.powerLimitSet, timeStampInSecondsDtuSynced))
      {
        Serial.println(F(" --- done"));
        // set next normal request in 5 seconds from now on, only if last data updated within last 2 times of user setted update rate
        if (currentMillis - globalData.lastRespTimestamp < (intervalMid * 2))
          previousMillisMid = currentMillis - (intervalMid - 5);
      }
      else
      {
        Serial.println(F(" --- error"));
      }
    }

    //
    if (updateInfoRequested)
    {
      getUpdateInfo();
    }
  }

  // 5s task
  if (currentMillis - previousMillis5000ms >= interval5000ms)
  {
    Serial.printf("\n>>>>> %02is task - state --> ", int(interval5000ms));
    Serial.print("local: " + getTimeStringByTimestamp(timeStampInSecondsDtuSynced));
    Serial.print(" --- NTP: " + timeClient.getFormattedTime());

    // Serial.print(" --- currentMillis " + String(currentMillis) + " --- ");
    previousMillis5000ms = currentMillis;
    // -------->
    // -----------------------------------------
    if (WiFi.status() == WL_CONNECTED)
    {
      // get current RSSI to AP
      int wifiPercent = 2 * (WiFi.RSSI() + 100);
      if (wifiPercent > 100)
        wifiPercent = 100;
      globalData.wifi_rssi_gateway = wifiPercent;
      // Serial.print(" --- RSSI to AP: '" + String(WiFi.SSID()) + "': " + String(globalData.wifi_rssi_gateway) + " %");

      if (userConfig.openhabActive)
        getPowerSetDataFromOpenHab();
    }

    // for testing
    // globalData.grid.totalEnergy = 1.34;
    // globalData.pv0.totalEnergy = 1.0;
    // globalData.pv1.totalEnergy = 0.34;

    // globalData.grid.power = globalData.grid.power + 1;
    // if (userConfig.mqttActive)
    //   updateValuesToMqtt(userConfig.mqttHAautoDiscoveryON);
    // if (globalData.grid.power > 450)
    //   globalData.grid.power = 0;
  }

  // mid task
  if (currentMillis - previousMillisMid >= intervalMid)
  {
    Serial.printf("\n>>>>> %02is task - state --> ", int(intervalMid));
    Serial.print("local: " + getTimeStringByTimestamp(timeStampInSecondsDtuSynced));
    Serial.print(" --- NTP: " + timeClient.getFormattedTime() + "\n");

    previousMillisMid = currentMillis;
    // -------->

    if (WiFi.status() == WL_CONNECTED)
    {
      dtuConnectionEstablish(&dtuClient, userConfig.dtuHostIpDomain);
      timeStampInSecondsDtuSynced = getDtuRemoteTimeAndDataUpdate(&dtuClient, timeStampInSecondsDtuSynced);

      if (!dtuConnection.dtuActiveOffToCloudUpdate)
      {
        if ((globalControls.getDataAuto || globalControls.getDataOnce) && globalData.uptodate)
        {
          if (userConfig.openhabActive)
            updateValueToOpenhab();
          if (userConfig.mqttActive)
            updateValuesToMqtt(userConfig.mqttHAautoDiscoveryON);

          if (globalControls.dataFormatJSON)
          {
            printDataAsJsonToSerial();
          }
          else
          {
            Serial.print("\n+++ update at remote: " + getTimeStringByTimestamp(globalData.respTimestamp) + " - uptodate: " + String(globalData.uptodate) + " --- ");
            Serial.print("wifi rssi: " + String(globalData.dtuRssi) + " % (DTU->Cloud) - " + String(globalData.wifi_rssi_gateway) + " % (Client->AP) \n");
            printDataAsTextToSerial();
          }
          if (globalControls.getDataOnce)
            globalControls.getDataOnce = false;
        }
        else if ((timeStampInSecondsDtuSynced - globalData.lastRespTimestamp) > (5 * 60) && globalData.grid.voltage > 0) // globalData.grid.voltage > 0 indicates dtu/ inverter working
        {
          globalData.grid.power = 0;
          globalData.grid.current = 0;
          globalData.grid.voltage = 0;

          globalData.pv0.power = 0;
          globalData.pv0.current = 0;
          globalData.pv0.voltage = 0;

          globalData.pv1.power = 0;
          globalData.pv1.current = 0;
          globalData.pv1.voltage = 0;

          if (userConfig.openhabActive)
            updateValueToOpenhab();
          if (userConfig.mqttActive)
            updateValuesToMqtt(userConfig.mqttHAautoDiscoveryON);
          dtuConnection.dtuErrorState = DTU_ERROR_LAST_SEND;
          Serial.print(F("\n>>>>> TIMEOUT 5 min for DTU -> NIGHT - send zero values\n"));
        }
      }
    }
  }

  // long task
  if (currentMillis - previousMillisLong >= intervalLong)
  {
    // Serial.printf("\n>>>>> %02is task - state --> ", int(interval5000ms));
    // Serial.print("local: " + getTimeStringByTimestamp(timeStampInSecondsDtuSynced));
    // Serial.print(" --- NTP: " + timeClient.getFormattedTime() + " --- currentMillis " + String(currentMillis) + " --- ");

    previousMillisLong = currentMillis;
    // -------->
    if (WiFi.status() == WL_CONNECTED)
    {
      timeClient.update();
    }
    // start async scan for wifi'S
    Serial.print(F("\nstart scan for wifi's\n"));
    WiFi.scanNetworks(true);
  }
  scanNetworksResult();
}