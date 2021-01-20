/*
   
   
   24-02-2020 After testing noticed i forgot the LED blink code

   23-02-2020 After BME sensor now update the PIR code for the new version

   15-02-2020 Major upgrade on Gateway and sensors
   Implementing: automatically find a gateway and channel independant
   The gateway will broadcast a WIFI SSID. The sensor will search for this and remember macaddr and channel in RTC memory
   Remove the ":" in the MQTT topic name of a sensor. This will make it better to read in the mcsMQTT plugin
   The broadcast SSID will be ESPNOW-01 (and numbering up if you have more gateways)
   The sensor will check if bootCount = 0. If that is the case a search for the best gateway will be started
   The sending result of a packet will be recorded: succesCount = 0 and incremented on a miss
   If the succesCount > 5 then a search for the gateway will be started.
   This will increace the battery drainage so you have to monitor this

   04-09-2019
   Replaced yield() with delay(1)
   delay will put esp32 in lower power mode
   29-09-2018
   Added that the updatefreq is sendback so we know what the status is in HomeSeer. Handy for new mcsMQTT plugin 
   02-01-2018
   ESPNow_mqtt_json_sensor_esp32_v1.1

   This is a sensor that works with ESPNow.
   Designed for battery use. If you have a power source then use WiFi direct
   It sends a json string to a gateway
   example: {"bootcount"="100":"voltage"="3912"}
   The gateway is statically assigned in the program (remoteMac[])
   After sending a packet it wait 25msec for a return command and then deep sleeps

   The gateway will take each item of the json string and publish it to MQTT: mac_addr_sensor/jsonitem
   example: MQTT publish: 30:AE:A4:04:4B:A0/bootcount 100
                          30:AE:A4:04:4B:A0/voltage 3912

   return commands:
   {"cmd"="101"} normal sleep operation
   {"cmd"="102") activate wifi and webserver
   {"cmd"="103"} activates wifi webserver and OTA
   {"cmd"="104"} update screen
   {"updatefreq"="60"} sets the sleeptimer to 60 seconds

   Add both send and receive commands as you wish.
   limit: the maximum size of an ESP-now packet is 250bytes. So your json string must be smaller

   This sketch is running on a WEMOS LOLIN32 board.
   It has a voltage divider (2x 100kOhm) to measure the battery voltage
   It has an Waveshare e-paper 1,54 inch display connected
   It will have an BME280 temp, hum, press sensor connected

   It only seems to work good if the WiFi channel of the gateway is 1 (don't ask me why)

*/

#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_deep_sleep.h>
#include <driver/rtc_io.h>
#include <ArduinoJson.h>
#include <esp_adc_cal.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include "..\..\passwords.h"

// Global copy of slave
#define V_REF 1100 // ADC reference voltage

StaticJsonBuffer<250> jsonBuffer; // for sending
StaticJsonBuffer<250> jsonBufferRecv;
WebServer server(80);
esp_adc_cal_characteristics_t characteristics;
esp_deep_sleep_wakeup_cause_t wakeup_reason;

const char *ssid = SSID;
const char *password = WIFI_PASSWORD;
const char *sensorName = "espnowpirsensor";
const char *homeseer = HOMESEER_IP;

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int sleepTime = 1200; //updateble via MQTT
RTC_DATA_ATTR int cmd = 101;        //updateble via MQTT
RTC_DATA_ATTR int motionTime = 30;  //updateble via MQTT
RTC_DATA_ATTR esp_now_peer_info_t slave;
RTC_DATA_ATTR int failCount = 0;    //counts the number of unsuccessfull transmits
RTC_DATA_ATTR int backoffTimer = 1; //increaces the reboot time when no gateway is found

volatile boolean readingSent;
long timeCounter = 0;
int sensorReading = 1;
#define uS_TO_S_FACTOR 1000000
#define LED_BUILTIN 5
#define MOTION_SENSOR 27
boolean cmdReceived = false;
long lastMsg = 0;
boolean initWiFiOnce = false;
boolean flagOTA = false;
char bs[250]; // receiving cmd databuffer

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.begin(115200);
  Serial.println("Bootcount: " + String(bootCount));

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != 0)
  {
    Serial.println("*** ESP_Now init failed");
  }
  delay(2); // This delay seems to make it work more reliably???
  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  delay(2);
  if (failCount > 5)
  { // we missed the gateway 5 times. search for new gateway
    failCount = 0;
    bootCount = 0;
  }
  if (bootCount == 0)
  {
    Serial.print("This node mac: ");
    Serial.println(WiFi.macAddress());
    ScanForGateways();
    backoffTimer = backoffTimer * 2;
    if (backoffTimer > 7200)
      backoffTimer = 7200;
    esp_deep_sleep_enable_timer_wakeup(backoffTimer * uS_TO_S_FACTOR);
    Serial.print("Going to sleep now backofftimer is: ");
    Serial.println(backoffTimer);
    delay(10);
    esp_deep_sleep_start();
    Serial.println("This will never be printed");
  }
  addPeer();
  ++bootCount;
  // Configure ADC
  adc1_config_width(ADC_WIDTH_12Bit);
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_11db);
  // Calculate ADC characteristics i.e. gain and offset factors
  esp_adc_cal_get_characteristics(V_REF, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, &characteristics);
  sendData();
  timeCounter = millis();
} // end setup
void loop()
{
  if ((cmd == 101) and ((millis() - timeCounter) > 20))
  { // sleepcommand and wait 50ms
    esp_deep_sleep_enable_timer_wakeup(sleepTime * uS_TO_S_FACTOR);
    // Serial.println("Setup ESP32 to sleep for " + String(sleepTime) + " Seconds");
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 or sensorReading == 1)
    {
      esp_deep_sleep_enable_timer_wakeup(motionTime * uS_TO_S_FACTOR);
      Serial.println("Setup ESP32 to sleep for " + String(motionTime) + " Seconds");
    }
    else
    { //      esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 1); //1 = High, 0 = Low
      esp_sleep_enable_ext1_wakeup(GPIO_SEL_27, ESP_EXT1_WAKEUP_ANY_HIGH);
      Serial.println("Setup ESP32 to sleep for " + String(sleepTime) + " Seconds");
    }
    Serial.print("Going to sleep now. millis= ");
    Serial.println(millis());
    delay(10);
    esp_deep_sleep_start();
    Serial.println("This will never be printed");
  }
  if ((cmd == 102 or cmd == 103) and (initWiFiOnce == false))
  {
    initWifi();
    initWebServer();
    initOTA();
    initWiFiOnce = true;
    flagOTA = true;
    Serial.println("Wifi, WebServer and OTA are initialzed");
  }
  if (cmd == 102)
    server.handleClient();
  if ((cmd == 103) or (flagOTA == true))
  {
    ArduinoOTA.handle();
    server.handleClient();
  }
  if (cmdReceived)
  {
    cmdReceived = false;
    JsonObject &root = jsonBufferRecv.parseObject(bs);
    if (!root.success())
    {
      Serial.println("parseObject() failed");
      return;
    }
    const char *updateFreq = root["updatefreq"]; // returns NULL if not valid
    if (updateFreq)
    {
      sleepTime = atoi(root["updatefreq"]);
      Serial.print("received updateFreq command. updateFreq: ");
      Serial.println(sleepTime);
      cmd = 101;
    }
    const char *updateMotionTime = root["motiontime"]; // returns NULL if not valid
    if (updateMotionTime)
    {
      motionTime = atoi(root["motiontime"]);
      Serial.print("received motiontime command. MotionTime: ");
      Serial.println(motionTime);
      cmd = 101;
    }
    const char *command = root["cmd"];
    if (command)
    {
      cmd = atoi(root["cmd"]);
      Serial.println("We have to do a command");
    }
    jsonBufferRecv.clear();
  }
  if (millis() - lastMsg > 60000)
  { // run each xx seconds
    lastMsg = millis();
    Serial.println("Still awake and serving webserver and OTA");
    //    sendData();
  }
  delay(1);
} // end loop
void initWifi()
{
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to ");
  Serial.print(ssid);
  if (strcmp(WiFi.SSID().c_str(), ssid) != 0)
  {
    WiFi.begin(ssid, password);
  }
  int retries = 20; // 10 seconds
  while ((WiFi.status() != WL_CONNECTED) && (retries-- > 0))
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  if (retries < 1)
  {
    Serial.print("*** WiFi connection failed");
    ESP.restart();
  }
  Serial.print("WiFi connected, IP address: ");
  Serial.println(WiFi.localIP());
} // end initWifi
void ScanForGateways()
{
  Serial.println("Scaning for gateways");
  bool slaveFound = false;
  int currentRSSI = -200;
  int8_t scanResults = WiFi.scanNetworks();
  if (scanResults == 0)
  {
    Serial.println("No WiFi devices in AP Mode found");
    // clean up ram
    WiFi.scanDelete();
    delay(100);
    ESP.restart();
    return;
  }
  /*
     scanResults will contain the number of access points
     found with scanNetworks(), this will include every
     access point the WiFi Chip can see including your
     home network and maybe even your neighbors.

     We will now search through the list and find any
     that have an SSID starting with the ESP_NOW_SSID variable
     stored in credentials.h, if you have more then one
     receiver (I have three) the code will select the one with
     the best RSSI signal which should be the closest and store
     the MAC address and channel number in the slave variable.

  */
  Serial.println(String(scanResults) + " Access Points found");
  for (int i = 0; i < scanResults; ++i)
  {
    Serial.print("SSID ");
    Serial.print(WiFi.SSID(i));
    if (WiFi.SSID(i).indexOf("ESPNOW") == 0)
    {
      Serial.print(" match found.");
      String BSSIDstr = WiFi.BSSIDstr(i);

      if (slaveFound)
      {
        //We have already found a match but is this one better?
        if (currentRSSI < WiFi.RSSI(i))
        {
          Serial.print("Got a better offer ");
          Serial.print(currentRSSI);
          Serial.print(" ");
          Serial.println(WiFi.RSSI(i));
          currentRSSI = WiFi.RSSI(i);
          slave.channel = WiFi.channel(i); // pick a channel
          slave.encrypt = 0;               // no encryption
          int mac[6];
          if (6 == sscanf(BSSIDstr.c_str(), "%x:%x:%x:%x:%x:%x%c", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]))
          {
            for (int ii = 0; ii < 6; ++ii)
            {
              slave.peer_addr[ii] = (uint8_t)mac[ii];
            }
          }
        }
      }
      else
      {
        //We have found our first match.
        slaveFound = true;
        currentRSSI = WiFi.RSSI(i);
        slave.channel = WiFi.channel(i); // pick a channel
        slave.encrypt = 0;               // no encryption
        int mac[6];
        if (6 == sscanf(BSSIDstr.c_str(), "%x:%x:%x:%x:%x:%x%c", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]))
        {
          for (int ii = 0; ii < 6; ++ii)
          {
            slave.peer_addr[ii] = (uint8_t)mac[ii];
          }
        }
      }
    }
    Serial.println(" " + String(WiFi.channel(i)));
  }
  if (slaveFound)
    ++bootCount;
  /*
  Serial.print("GatewayMac: ");
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           slave.peer_addr[0], slave.peer_addr[1], slave.peer_addr[2], slave.peer_addr[3], slave.peer_addr[4], slave.peer_addr[5]);
  Serial.println(macStr);
  Serial.print("GatewayChannel: ");
  Serial.println(slave.channel);
  */
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{ // callback when data is sent from Master to Slave
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print("Last Packet Sent to: ");
  Serial.println(macStr);
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  failCount = 0;
  backoffTimer = 1;
  readingSent = true;
  if (status != 0)
  {
    readingSent = false;
  }
}
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{ // callback when receiving data
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print("Last Packet Recv from: ");
  Serial.println(macStr);
  Serial.print("Last Packet Recv Data: ");
  for (int i = 0; i < (data_len - 1); i++)
  {
    bs[i] = data[i];
    Serial.print(bs[i]);
  }
  Serial.println();
  cmdReceived = true;
}
void addPeer()
{
  esp_wifi_set_promiscuous(true); // we need to set promiscuous first in order to set the channel (do't ask me why)
  esp_wifi_set_channel(slave.channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  slave.encrypt = 0;
  const esp_now_peer_info_t *peer = &slave;
  esp_err_t addStatus = esp_now_add_peer(peer);
  if (addStatus == ESP_OK)
  {
    // Pair success
    // Serial.println("Pair success");
  }
  else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT)
    Serial.println("ESPNOW Not Init");
  else if (addStatus == ESP_ERR_ESPNOW_ARG)
    Serial.println("Invalid Argument");
  else if (addStatus == ESP_ERR_ESPNOW_FULL)
    Serial.println("Peer list full");
  else if (addStatus == ESP_ERR_ESPNOW_NO_MEM)
    Serial.println("Out of memory");
  else if (addStatus == ESP_ERR_ESPNOW_EXIST)
    Serial.println("Peer Exists");
  else
    Serial.println("Not sure what happened");
}
void sendData()
{ // send data
  int samples = 5;
  int voltage = 0;
  for (unsigned int x = 0; x < samples; x++)
  {
    voltage += adc1_to_voltage(ADC1_CHANNEL_4, &characteristics);
  }
  voltage = voltage / samples;

  pinMode(MOTION_SENSOR, INPUT);
  sensorReading = digitalRead(MOTION_SENSOR);
  pinMode(MOTION_SENSOR, INPUT_PULLDOWN); //try pulling it down a little faster

  // create json string
  JsonObject &root = jsonBuffer.createObject();
  root["bootcount"] = bootCount;
  root["voltage"] = voltage * 2.098;
  root["updatefreq"] = sleepTime;
  root["failCount"] = failCount;
  root["motion"] = sensorReading;

  char jsonStr[root.measureLength() + 1];
  root.printTo((char *)jsonStr, root.measureLength() + 1);
  jsonStr[root.measureLength() + 1] = '\0';
  const uint8_t *peer_addr = slave.peer_addr;
  Serial.print("Sending: ");
  Serial.println(jsonStr);
  esp_err_t result = esp_now_send(peer_addr, (uint8_t *)jsonStr, sizeof(jsonStr));
  // Serial.print("Send Status: ");
  if (result == ESP_OK)
  {
    // Serial.println("Success");
    ++failCount;
  }
  else if (result == ESP_ERR_ESPNOW_NOT_INIT)
  {
    // How did we get so far!!
    Serial.println("ESPNOW not Init.");
  }
  else if (result == ESP_ERR_ESPNOW_ARG)
  {
    Serial.println("Invalid Argument");
  }
  else if (result == ESP_ERR_ESPNOW_INTERNAL)
  {
    Serial.println("Internal Error");
  }
  else if (result == ESP_ERR_ESPNOW_NO_MEM)
  {
    Serial.println("ESP_ERR_ESPNOW_NO_MEM");
  }
  else if (result == ESP_ERR_ESPNOW_NOT_FOUND)
  {
    Serial.println("Peer not found.");
  }
  else
  {
    Serial.println("Not sure what happened");
  }
  jsonBuffer.clear();
} // end sendData

void initOTA()
{
  ArduinoOTA.setHostname(sensorName);
  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
      })
      .onEnd([]() {
        Serial.println("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
      });

  ArduinoOTA.begin();
}
//Webserver routines
void initWebServer()
{
  server.on("/", handleRoot);
  server.on("/restart", restart);
  server.on("/updateOTA", updateOTA);
  server.on("/cancelOTA", cancelUpdateOTA);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  delay(100);
}
void updateOTA()
{
  server.send(200, "text/plain", "Going into OTA programming mode");
  flagOTA = true;
}
void cancelUpdateOTA()
{
  server.send(200, "text/plain", "Canceling OTA programming mode");
  flagOTA = false;
}
void handleRoot()
{
  String message = "Hello from sensor 2.0";
  message += "\n\nProgram: ESPNow32_sensor_PIR_V2.0\n";
  message += "\nUsages:\n";
  message += "/                  - This messages\n";
  message += "/updateOTA         - Put device in OTA programming mode\n";
  message += "/cancelOTA         - Cancel OTA programming mode\n";
  message += "/restart           - Restarts the ESPNow gateway\n\n";
  message += "Voltage: ";
  message += adc1_to_voltage(ADC1_CHANNEL_4, &characteristics) * 2;
  message += "\nWiFi channel: ";
  message += WiFi.channel();
  message += "\nRSSI: ";
  message += WiFi.RSSI();
  message += "\nCommand flag: ";
  message += cmd;
  message += "\nOTA flag: ";
  message += flagOTA;
  message += "\n\n";
  server.send(200, "text/plain", message);
}
void restart()
{
  server.send(200, "text/plain", "OK restarting");
  delay(2000);
  ESP.restart();
}
void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}
