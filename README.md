# ESPNow-Gateway and Sensors
ESPNow to MQTT Gateway and Sensors

## Bridge between espnow and MQTT
The ESPNow gateway can run on any ESP32 board/chip. 
There are no IO-pins used. The gateway connects the WiFi and MQTT server.

## Gateway Operation
Beside being a WiFi client the gateway broadcast a SSID. 
This SSID holds het mac-address of the gateway.
Format: ESPNOW001122334455.
The mac-address of any newly heard client will be added in the "peer" list. 

### Receiving espnow packets and sending them
The received packet from a sensor will hold a JSON string with data.
* This data is forwarded in the following format:
    * Topic: macadressOffClient/JSON-item
    * Payload: JSON-item-payload
So if the gateway receives a packet from a client with mac-address 001122334455, with this payload: {"Temperature":"21.3","Humidity":"45"}, it will three two MQTT messages:
* Message1: Topic 001122334455/Temperature  Payload: 21.3
* Message2: Topic 001122334455/Humidity  Payload: 45
* Message3: Topic 001122334455/status Payload: current time (this will always be added by the gateway)

### Special message
There is a special messages item:
* "updatefreq": This holds the sleep timer of the sensor in seconds. Multiplied by 5, the gateway determons if the sensor is still alive. "Connection lost" will be published in the status payload if 5 updates are missed

### Sending messages to the sensor
You can send messages to a sensor. Messages are stored in the gateway and send when a sensor is awake. The messages is send in the following MQTT format. Topic: "espnowgw/001122334455/PARAMETER" Payload: "XX". This will result in a JSON string queued at the sensor: {"PARAMETER":"XX"}. Example:
* Topic: espnowgw/001122334455/updatefreq  Payload: 600  Will result in sending {"updatefreq":"600"} to a sensor with mac-address 001122334455. 
This wil set the updatefrequency to 600 seconds in the gateway and sensor.
* Commands, Topic: espnowgw/001122334455/cmd Payload 103. The "cmd" is a special and can hold the following:
    * 101: sensor to return to normal operation
    * 102: activate WiFi, webserver and stay active
    * 103: activate WiFi, webserver, OTA and stay active


## Client Operation
* A sensor will scan for SSID's starting with "ESPNOW" and select the strongest.
It will only do this during boot. It will then store mac-address of the gateway and WiFi channel in RTC memory.
If no gateway is found the sensor will go to sleep and will multiply his wakeup time by 2 (with a maximum of 7200 sec). This is prevent battery drainage if no gateway is found.

* Based on the "updatefreq" counter, the sensor will go to sleep. When waking up it will send his sensor data combined with battery voltage and updatefrequency. All data will be put in a JSON string. After sending the data the sensor will check if the data is delivered to the gateway. If the data is not delivered 10 times in a row, the sensor assumes the gateway is down and will start the gateway selection at the next wakeup.

* A PIR sensor will wakeup after the "updatefreq" counter or if motion is detected (hardware based wakeup). To prevent continuous waking up the sensor will not wake up for "montiontime" time even if motion is detected.

### Receiving commands
After sending data sensor will wait (50msec) to see if there is data to receive. If there is data the commands will executed.

# Limits
* A gateway supports a maximum of 20 sensors. You can have multiply gateways.
* The range of espnow is very good. Because it used a low bandwith. It is much better then WiFi.
* Battery live with a 1500mah battery is around 1.5 years.

![image](https://github.com/BartGijsbers/ESPNow-PIRsensor/blob/main/images/PIRsensor.jpg)
![image](https://github.com/BartGijsbers/ESPNow-PIRsensor/blob/main/images/Tempsensor.jpg)
