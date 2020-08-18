
/*
  ESP8266/32 to interface a LJ18A3-8-Z/BX inductive
  sensor to count impulses from a water meter.
  (C) 2020 Stefan Seyfried, License: WTFPL-2.0

  GPIO0 (built in button): Trigger WPS for WIFI connect
  GPIO5: open-collector input from inductive sensor

  LED signaling:
  * LED off:       wifi connected
  * solid on:      WPS active
  * slow blinking: wifi disconnected
  * fast blink:    wifi failed

  this has initially been intended for ESP32, but
  is now used with an ESP8266. The ESP32 code paths
  are not really tested and just kept for possible
  future reuse!

  TODO: push values directly to volkszaehler instance
*/


int buttonPin = 0; /* button on GPIO0 */
int inputPin = 5;  /* GPIO5 has a pullup resistor */

/* mostly for LED blinking modes */
enum {
  STATE_DISC = 0,
  STATE_WPS,
  STATE_CONN,
  STATE_FAIL
};

/* LED blinking patterns */
const char *_s[4] = {
  "1111100000", // DISC
  "1111111111", // WPS
  "0000000000", // CONN
  "1010101010"  // FAIL
};

struct eeprom_state {
  char sig[8]; /* WATER */
  int pulses;
};

int state = STATE_DISC;

#include <EEPROM.h>

#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#define LED_ON LOW
#define LED_OFF HIGH
#else
#include "WiFi.h"
#include "esp_wps.h"
#include <WebServer.h>
#define LED_ON HIGH
#define LED_OFF LOW

#define ESP_MANUFACTURER  "ESPRESSIF"
#define ESP_MODEL_NUMBER  "ESP32"
#define ESP_MODEL_NAME    "ESPRESSIF IOT"
#define ESP_DEVICE_NAME   "ESP STATION"

static esp_wps_config_t config;
void wpsInitConfig() {
  config.crypto_funcs = &g_wifi_default_wps_crypto_funcs;
  config.wps_type = WPS_TYPE_PBC;
  strcpy(config.factory_info.manufacturer, ESP_MANUFACTURER);
  strcpy(config.factory_info.model_number, ESP_MODEL_NUMBER);
  strcpy(config.factory_info.model_name, ESP_MODEL_NAME);
  strcpy(config.factory_info.device_name, ESP_DEVICE_NAME);
}

void WiFiEvent(WiFiEvent_t event, system_event_info_t info) {
  switch (event) {
    case SYSTEM_EVENT_STA_START:
      Serial.println("Station Mode Started");
      state = STATE_DISC;
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println("Connected to :" + String(WiFi.SSID()));
      Serial.print("Got IP: ");
      Serial.println(WiFi.localIP());
      state = STATE_CONN;
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("Disconnected from station, attempting reconnection");
      state = STATE_DISC;
      WiFi.reconnect();
      break;
    case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
      Serial.println("WPS Successfull, stopping WPS and connecting to: " + String(WiFi.SSID()));
      Serial.println("WIFI PSK: " +String(WiFi.psk()));
      esp_wifi_wps_disable();
      state = STATE_DISC;
      delay(10);
      WiFi.begin();
      break;
    case SYSTEM_EVENT_STA_WPS_ER_FAILED:
      Serial.println("WPS Failed, retrying normal connect");
      esp_wifi_wps_disable();
      state = STATE_DISC;
      delay(10);
      WiFi.begin();
      break;
    case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
      Serial.println("WPS Timedout, trying normal connect...");
      state = STATE_DISC;
      esp_wifi_wps_disable();
      state = STATE_DISC;
      delay(10);
      WiFi.begin();
      break;
#if 0
    case SYSTEM_EVENT_STA_WPS_ER_PIN:
      Serial.println("WPS_PIN = " + wpspin2string(info.sta_er_pin.pin_code));
      break;
#endif
    default:
      break;
  }
}
#endif


#ifdef ESP8266
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

void start_WPS() {
  Serial.println("Starting WPS");
  state = STATE_FAIL;
  digitalWrite(LED_BUILTIN, LED_ON);
  
#ifdef ESP8266
  WiFi.mode(WIFI_STA);
  String old_ssid = WiFi.SSID();
  String old_psk = WiFi.psk();
  bool wpsSuccess = WiFi.beginWPSConfig();
  Serial.print("wpsSuccess: ");
  Serial.println(wpsSuccess);
  Serial.println("WiFi.SSID: " + WiFi.SSID());
  Serial.println("WiFi.psk:  " + WiFi.psk());
  if (WiFi.SSID().length() == 0 && old_ssid.length() > 0) {
    WiFi.begin(old_ssid, old_psk);
  }
#else
  WiFi.mode(WIFI_MODE_STA);
  wpsInitConfig();
  esp_wifi_wps_enable(&config);
  esp_wifi_wps_start(0);
  while (state == STATE_WPS) {
    delay(500);
    Serial.printl(".");
  }
#endif
  digitalWrite(LED_BUILTIN, LED_OFF);
  Serial.println("end start_WPS()");
}

void WiFiStatusCheck() {
  static wl_status_t last = WL_NO_SHIELD;
  wl_status_t now = WiFi.status();
  if (now == last)
    return;
  Serial.print("WiFI status changed from: ");
  Serial.print(last);
  Serial.print(" to: ");
  Serial.println(now);
  if (now == WL_CONNECTED)
    state = STATE_CONN;
  else
    state = STATE_DISC;
  last = now;
}

volatile int pulses = 0;
volatile int last_intr = 0;
//volatile int hilo = HIGH;
int last_commit = 0;
int last_pulse = 0;
eeprom_state persist;

void ICACHE_RAM_ATTR isr(void) {
  //int in = digitalRead(inputPin);
  //if (hilo != in) {
  if (millis() - last_intr > 20) {
    last_intr = millis();
    //hilo = in;
    pulses++;
#if 0
    Serial.print("IRQ! ");
    //Serial.print(hilo);
    //Serial.print(" ");
    Serial.print(last_intr);
    Serial.print(" ");
    Serial.print(pulses);
    Serial.print(" ");
    Serial.println(millis());
#endif
  }
  //}
}

void handle_index() {
  String index = "Wasserzaehler\n";
  String IP = WiFi.localIP().toString();
  index += "Pulse:  " + String(pulses) + "\n";
  index += "Uptime: " + String(millis()) + "\n";
  index += "http://" + IP + "/pulses for plain pulse count\n";
  index += "http://" + IP + "/pulses?set=xxxx to set pulse count\n";
  server.send(200, "text/plain", index);
}

void handle_uptime() {
  server.send(200, "text/plain", String(millis()) + "\n");
}

void handle_pulses() {
  String message;
  int ret = 200;
  if (server.hasArg("set")) {
    long p = server.arg("set") . toInt();
    if (p != 0) {
      message = "pulses value set to " + String(p);
      pulses = p;
    } else {
      ret = 500;
      message = "invalid pulse value '" + server.arg("set") + "'";
    }
  } else {
    message = String(pulses);
  }
  server.send(ret, "text/plain", message + "\n");
}

void setup() {
  Serial.begin(115200);
  // Serial.setDebugOutput(true); // send additional debug infos to serial
  Serial.println(); // bootloader has clobbered serial monitor
  delay(100);
  EEPROM.begin(512);
  EEPROM.get(0, persist);
  if (persist.sig[0] == 0xff || memcmp(persist.sig, "WATER", 5)) {
    Serial.println("Clearing eeprom...");
    Serial.println(persist.sig);
    memset(persist.sig, 0, sizeof(persist.sig));
    strcpy(persist.sig, "WATER");
    persist.pulses = 0;
    EEPROM.put(0,persist);
    EEPROM.commit();
  } else {
    Serial.print("Read pulses from eeprom: ");
    Serial.println(persist.pulses);
    pulses = persist.pulses;
    last_pulse = pulses;
  }
  last_commit = millis();
  Serial.println();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(buttonPin, INPUT);
  pinMode(inputPin, INPUT_PULLUP);
  digitalWrite(LED_BUILTIN, LED_ON);

  Serial.println();
  Serial.println("Enabling WiFi");
#ifndef ESP8266
  WiFi.onEvent(WiFiEvent);
#endif
  WiFi.begin();

  server.on("/", handle_index);
  server.on("/pulses", handle_pulses);
  server.on("/uptime", handle_uptime);
  server.begin();
  attachInterrupt(digitalPinToInterrupt(inputPin), isr, FALLING);
}

int i = 0;
void loop() {
  WiFiStatusCheck();
  // Serial.print(_s[state][i]);
  if (_s[state][i] == '1') 
    digitalWrite(LED_BUILTIN, LED_ON);   // turn the LED on (HIGH is the voltage level)
  else
    digitalWrite(LED_BUILTIN, LED_OFF);    // turn the LED off by making the voltage LOW
  i++;
  if (i > 9) {
    i = 0;
    // Serial.println();
  }
  if (digitalRead(buttonPin) == LOW) {
    Serial.println("Triggering WPS!");
    start_WPS();
  }
  if (persist.pulses != pulses) {
    Serial.printf("Pulse update: %d\n", pulses);
  }
  persist.pulses = pulses;
  if (millis() - last_commit > 60000) {
    Serial.printf("COMMIT! %d last_p: %d, pulse: %d\n", millis() - last_commit, last_pulse, persist.pulses);
    if (persist.pulses != last_pulse) {
      EEPROM.put(0,persist);
      EEPROM.commit();
    }
    last_commit = millis();
    last_pulse = persist.pulses;
  }
  server.handleClient();
  delay(100);                       // wait for 0.1 seconds
}
