/*
  ESP8266/32 to interface a LJ18A3-8-Z/BX inductive
  sensor to count impulses from a water meter.
  (C) 2020 Stefan Seyfried, License: WTFPL-2.0

  * This program is free software. It comes without any warranty, to
  * the extent permitted by applicable law. You can redistribute it
  * and/or modify it under the terms of the Do What The Fuck You Want
  * To Public License, Version 2, as published by Sam Hocevar. See
  * http://www.wtfpl.net/ for more details.

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

  http update either via browser: http://hostname/update
  or via console: curl -F "image=@firmware.bin" http://hostname/update
*/

#if __has_include("wasserzaehler-version.h") && __has_include(<stdint.h>)
#include "wasserzaehler-version.h"
#else
#define WASSER_VERSION "unknown"
#endif

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
  char sig[8];  /* WATER */
  int pulses;   /* 4 bytes */
  int pulses_sent;
  char vzhost[48]; /* hostname */
  char vzurl[192]; /* total: 256 */
};

int state = STATE_DISC;

#include <EEPROM.h>

#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#define LED_ON LOW
#define LED_OFF HIGH
#else
#include "WiFi.h"
#include "esp_wps.h"
#include <WebServer.h>
#include <HTTPUpdateServer.h>
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
ESP8266HTTPUpdateServer httpUpdater;
#else
WebServer server(80);
HTTPUpdateServer httpUpdater;
#endif
WiFiClient client;

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
    Serial.println(".");
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
volatile int hilo;
volatile bool last_state;
int last_pulse = 0;
volatile unsigned long last_debounce = 0;
unsigned long last_commit = 0;
unsigned long last_push = 0;
eeprom_state persist;

const String sysinfo("Software version: " WASSER_VERSION ", built at: " __DATE__ " " __TIME__);

unsigned int debounce_delay = 100; // milliseconds

void ICACHE_RAM_ATTR isr(void) {
  int in = digitalRead(inputPin);
  if (hilo == in)
    return;
  boolean debounce = false;
  if (millis() - last_debounce < debounce_delay)
    debounce = true;

  last_debounce = millis();

  if (debounce)
    return;

  hilo = in;
  if (hilo)
    pulses++;
}

uint32_t uptime_sec()
{
#ifdef ESP8266
  return (micros64()/(int64_t)1000000);
#else
  return (esp_timer_get_time()/(int64_t)1000000);
#endif
}

String time_string(void)
{
  uint32_t now = uptime_sec();
  char timestr[10];
  String ret = "";
  if (now >= 24*60*60)
      ret += String(now / (24*60*60)) + "d ";
  now %= 24*60*60;
  snprintf(timestr, 10, "%02d:%02d:%02d", now / (60*60), (now % (60*60)) / 60, now % 60);
  ret += String(timestr);
  return ret;
}

bool check_vzserver() {
  return (persist.vzhost[0] != '\0' && persist.vzurl[0] != '\0');
}

void handle_index() {
  // TODO: use server.hostHeader()?
  unsigned long uptime = millis();
  String IP = WiFi.localIP().toString();
  String index =
    "<!DOCTYPE HTML><html lang=\"en\">\n<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<meta name=\"description\" content=\"my water meter\">\n"
    "<title>Wasserzaehler</title>\n"
    "</head>\n<body>\n"
    "<H1>Wasserzaehler</H1>\n"
    "<pre>";
  index += "Pulse:  " + String(pulses) + "\n";
  index += "Uptime: " + time_string() + "\n";
  index += "http://" + IP + "/pulses for plain pulse count\n";
  index += "http://" + IP + "/pulses?set=xxxx to set pulse count\n";
  index += "http://" + IP + "/vz?host=xxxx to set volkszaehler middleware host\n";
  index += "http://" + IP + "/vz?url=xxxx to set volkszaehler middleware url\n";
  if (check_vzserver()) {
    index += "\ncurrent volkszaehler URL:\n";
    index += "http://" + String(persist.vzhost) + String(persist.vzurl) + "\n";
    index += "Last push: " + String(last_push) + " (" + String(uptime - last_push) + "ms ago)\n";
    index += "Last value: " + String(persist.pulses_sent) + "\n";
  }
  index += "</pre>\n"
    "<br>\n<a href=\"/config.html\">Configuration page</a>\n"
    "<p>" + sysinfo + "\n"
    "</body>\n";
  server.send(200, "text/html", index);
}

void handle_config() {
  String resp =
    "<!DOCTYPE HTML><html lang=\"en\">\n<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<meta name=\"description\" content=\"water meter configuration\">\n"
    "<title>Wasserzaehler Configuration</title>\n"
    "</head>\n<body>\n"
    "<H1>Wasserzaehler Configuration</H1>\n"
    "<table>\n"
    "<form action=\"/vz\">"
      "<tr>"
        "<td>Volkszaehler Hostname:</td><td><input name=\"host\" value=\"";
  resp += String(persist.vzhost);
  resp += "\"></td>"
      "</tr>\n<tr>"
        "<td>Volkszaehler URL:</td><td><input name=\"url\" value=\"";
  resp += String(persist.vzurl);
  resp += "\"></td>"
        "<td><button type=\"submit\">Submit</button></td>"
      "</tr>\n"
    "</form>\n"
    "<tr><td><h2>Pulse correction</h2></td></tr>\n"
    "<form action=\"/pulses.html\">"
      "<tr>"
        "<td>Pulses:</td><td><input name=\"set\" value=\"";
  resp += String(pulses);
  resp += "\"></td>"
        "<td><button type=\"submit\">Submit</button></td>"
      "</tr>"
    "</form>\n"
    "</table>\n"
    "<p><a href=\"/update\">Software Update</a>\n"
    "<p>" + sysinfo + "\n"
    "</body>\n</html>\n";
  server.send(200, "text/html", resp);
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
      if (p != pulses) {
        message = "pulses value set to " + String(p);
        pulses = p;
      } else {
        message = "pulse value unchanged " + String(p);
      }
    } else {
      ret = 500;
      message = "invalid pulse value '" + server.arg("set") + "'";
    }
  } else {
    message = String(pulses);
  }
  server.send(ret, "text/plain", message + "\n");
}

void handle_pulses_html() {
  String message =
    "<!DOCTYPE HTML><html><head>"
    "<title>Pulses set</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "</head><body>"
    "<H1>Pulses set</H1>"
    "<pre>";
  int ret = 200;
  if (server.hasArg("set")) {
    long p = server.arg("set") . toInt();
    if (p != 0) {
      if (p != pulses) {
        message += "pulses value set to " + String(p);
        pulses = p;
      } else {
        message += "pulse value unchanged " + String(p);
      }
    } else {
      ret = 500;
      message += "invalid pulse value '" + server.arg("set") + "'";
    }
  } else {
    message += String(pulses);
  }
  message += "</pre>"
    "<br><a href=\"/index.html\">Main page</a>"
    "</body>";
  server.send(ret, "text/html", message);
}

bool checkhost(const char *host, int len) {
  int i;
  const char *c = host;
  if (*c == '-' || *c == '.')
    return false;
  for (i = 0; i < len; i++, c++) {
    if ((*c < '0' || *c > '9') &&
        (*c < 'a' || *c > 'z') &&
        (*c < 'A' || *c > 'Z') &&
        (*c != '-') && (*c != '.'))
      return false;
  }
  return true;
}

void handle_vz() {
  String message = "";
  int ret = 200;
  bool change = false;
  if (server.hasArg("host")) {
    String host = server.arg("host");
    if (host.length() > sizeof(persist.vzhost) -1) {
      message = "hostname too long (max " + String(sizeof(persist.vzhost)-1) + " bytes)\n";
    } else if (!checkhost(host.c_str(), host.length())) {
      message = "invalid hostname (only a-z,A-Z,- allowed)\n";
    } else {
      const char *chost = host.c_str();
      if (strcmp(persist.vzhost, chost)) {
        message = "Set hostname to " + host + "\n";
        strcpy(persist.vzhost, chost);
        change = true;
      } else {
        message = "hostname not changed\n";
      }
    }
    message += "\n";
  }
  if (server.hasArg("url")) {
    String url = server.arg("url");
    if (url.length() > sizeof(persist.vzurl) -1) {
      message = "URL path too long (max " + String(sizeof(persist.vzurl)-1) + " bytes)\n";
    } else if (url.length() > 0 && url[0]!= '/') {
      message += "url path must start with '/'\n";
    } else {
      const char *curl = url.c_str();
      if (strcmp(persist.vzurl, curl)) {
        message += "Set URL to " + url + "\n";
        strcpy(persist.vzurl, curl);
        change = true;
      } else {
        message += "URL not changed\n";
      }
    }
    message += "\n";
  }
  message += "VZ host: " + String(persist.vzhost) + "\n";
  message += "VZ URL:  " + String(persist.vzurl) + "\n";
  server.send(ret, "text/plain", message);
  if (change) {
    EEPROM.put(0, persist);
    EEPROM.commit();
  }
}

long code_from_str(const String s) {
  if (!s.startsWith("HTTP/1."))
    return -1;
  String c = s.substring(9);
  if (c.length() < 3)
    return -1;
  return c.toInt();
}

bool vz_push(int count) {
  String response = "";
  if (!check_vzserver())
    return false;
  if (! client.connect(persist.vzhost, 80)) {
    Serial.println("Connect to volkszaehler server failed");
    client.stop();
    return false;
  }
  int p = persist.pulses_sent;
  if (count > p) {
    p = count;
  }
  String cmd = "GET " + String(persist.vzurl);
  cmd += "?operation=add&value=" + String(p);
  Serial.println(cmd);
  cmd += " HTTP/1.1\r\nHost: " + String(persist.vzhost);
  cmd += "\r\n";
  cmd += "Content-Type: application/json\r\n";
  cmd += "Connection: keep-alive\r\nAccept: */*\r\n\r\n";
  client.print(cmd);
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println(">>> Client Timeout !");
      break;
    }
  }
  while (client.available()) {
    char c = static_cast<char>(client.read());
    /* HTTP/1.1 200 OK */
    if (response.length() < 13) {
      response += c;
    }
    // Serial.printf("%c", c);
  }
  client.stop();
  Serial.println("Return line: " + response);
  long ret = code_from_str(response);
  if (ret > 0) {
    Serial.println("HTTP return code: " + String(ret));
  }
  if (ret == 200) {
    persist.pulses_sent = p;
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  // Serial.setDebugOutput(true); // send additional debug infos to serial
  for (int i=0; i < 10; i++)
    Serial.println("xxxxxxxxxxxxxxxxxxxxxxxxxxxx"); // bootloader has clobbered serial monitor
  delay(100);
  EEPROM.begin(512);
  EEPROM.get(0, persist);
  if (persist.sig[0] == 0xff || memcmp(persist.sig, "WATER", 5)) {
    Serial.println("Clearing eeprom...");
    Serial.println(persist.sig);
    memset(&persist, 0, sizeof(persist));
    strcpy(persist.sig, "WATER1");
    persist.pulses = 0;
    persist.pulses_sent = 0;
    EEPROM.put(0,persist);
    EEPROM.commit();
  } else {
    Serial.print("Read pulses from eeprom: ");
    Serial.println(persist.pulses);
    pulses = persist.pulses;
    if (persist.sig[5] < '1') { // update
      Serial.println("updating config from " +String(persist.sig)+ " to WATER1\n");
      strcpy(persist.sig, "WATER1");
      persist.pulses_sent = pulses;
      memset(persist.vzhost, 0, sizeof(persist.vzhost));
      memset(persist.vzurl, 0, sizeof(persist.vzurl));
      EEPROM.put(0,persist);
      EEPROM.commit();
    }
    Serial.print("VZhost: ");
    Serial.println(persist.vzhost);
    Serial.print("VZurl:  ");
    Serial.println(persist.vzurl);
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
  server.on("/index.html", handle_index);
  server.on("/pulses", handle_pulses);
  server.on("/pulses.html", handle_pulses_html);
  server.on("/uptime", handle_uptime);
  server.on("/vz", handle_vz);
  server.on("/config.html", handle_config);
  httpUpdater.setup(&server);
  server.begin();
  attachInterrupt(digitalPinToInterrupt(inputPin), isr, CHANGE);
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
  
  bool update_push = false;
  if (check_vzserver() && millis() - last_push > 60000)
    update_push = true;
  if (persist.pulses != pulses || update_push) {
    Serial.printf("Pulse update: %d\n", pulses);
    if (vz_push(pulses))
      last_push = millis();
  }
  persist.pulses = pulses;
  if (millis() - last_commit > 60000) {
    Serial.printf("COMMIT! %lu last_p: %d, pulse: %d\n", millis() - last_commit, last_pulse, persist.pulses);
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
