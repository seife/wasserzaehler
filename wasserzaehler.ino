/*
  ESP8266/32 to interface a LJ18A3-8-Z/BX inductive
  sensor to count impulses from a water meter.
  ...and from a gas meter reed switch contact :-)
  Now also measures temperature with an DS18B20 sensor
      and publishes it via MQTT.
  (C) 2020-2022 Stefan Seyfried, License: WTFPL-2.0

  * This program is free software. It comes without any warranty, to
  * the extent permitted by applicable law. You can redistribute it
  * and/or modify it under the terms of the Do What The Fuck You Want
  * To Public License, Version 2, as published by Sam Hocevar. See
  * http://www.wtfpl.net/ for more details.

  GPIO0 (built in button): Trigger WPS for WIFI connect
  GPIO5: open-collector input from inductive sensor
  GPIO4: reed switch contact for gas meter, connected to ground
  GPIO14: DS18B20 one-wire temperature sensor

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
int inputPin[2] = { 5, 4 };  /* water, gas */
#define WATER 0
#define GAS 1
String label[2] = { "Water", "Gas" };
String unit[2] = { "l", "m³" };
/* how much "unit" is one pulse? Should be made configurable... */
double unit_factor[2] = { 1, 0.01 };
int unit_frac[2] = { 0, 3 }; /* how many decimal digits */

int tempPin = 14;  /* GPIO14 for DS18B20 */

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

/* old */
struct eeprom_state {
  char sig[8];  /* WATER */
  int pulses;   /* 4 bytes */
  int pulses_sent;
  char vzhost[48]; /* hostname */
  char vzurl[192]; /* total: 256 */
};

/* new */
uint32_t g_pulses[2] = { 0, 0 };
uint32_t g_pulses_sent[2] = { 0, 0 };
String g_vzhost;
String g_vzurl[2] = { "", "" };

const char* _pulses[2] = { "pulses0", "pulses1" };
const char* _pulses_sent[2] = { "pulses_sent0", "pulses_sent1" };
const char* _vzurl[2] = { "vzurl0", "vzurl1" };

/* temperature measurement stuff */
String g_mqtthost;
uint16_t g_mqttport = 1883; /* default */
String g_mqtttopic;
String g_mqttid = "no sensor";
bool mqtt_changed = false;    /* config change */
bool mqtt_server_set = false; /* complete config available */
float g_temp = -127.0;

int state = STATE_DISC;

#include <EEPROM.h>
#include <Preferences.h>
#include <Ticker.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>

DeviceAddress DS18B20_Address;
OneWire oneWire(tempPin);
DallasTemperature sensor(&oneWire);

/* helper */
void log_time() {
  /* maybe extend later */
  Serial.printf("%lu ", millis());
}

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

/*
 * FIXME: THIS IS WRONG, ENUM VALUES ARE NOW ARDUINO_EVENT_STA...
 * commented out to intentionally break ESP32 build until this is fixed...
 *
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
*/
#endif


#ifdef ESP8266
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater(true);
#else
WebServer server(80);
HTTPUpdateServer httpUpdater;
#endif
WiFiClient client;
WiFiClient mqtt_conn;
PubSubClient mqtt_client(mqtt_conn);

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

volatile uint32_t pulses[] = { 0, 0};
volatile int hilo[2];
volatile bool last_state;
uint32_t last_pulse[] = { 0, 0 };
volatile unsigned long last_debounce[] = { 0, 0 };
bool update_push[] = { false, false };
unsigned long last_push[] = { 0, 0 };
unsigned long last_temp = 0;
Preferences pref;

const String sysinfo("Software version: " WASSER_VERSION ", built at: " __DATE__ " " __TIME__);

unsigned int debounce_delay = 100; // milliseconds

void IRAM_ATTR isr(int i) {
  int in = digitalRead(inputPin[i]);
  if (hilo[i] == in)
    return;
  boolean debounce = false;
  if (millis() - last_debounce[i] < debounce_delay)
    debounce = true;

  last_debounce[i] = millis();

  if (debounce)
    return;

  hilo[i] = in;
  if (hilo[i])
    pulses[i]++;
}

void IRAM_ATTR isr0(void) {
  isr(0);
}

void IRAM_ATTR isr1(void) {
  isr(1);
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

bool check_vzserver(int i) {
  return (!g_vzhost.isEmpty() && !g_vzurl[i].isEmpty());
}

bool check_mqserver() {
  return (!g_mqtthost.isEmpty() && !g_mqtttopic.isEmpty() && g_mqttport);
}

void handle_index() {
  // TODO: use server.hostHeader()?
  int i;
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
  for (i = 0; i < 2; i++)
    index += "Pulse:  " + String(pulses[i]) + " (" + label[i] +"), " +
             String(pulses[i] * unit_factor[i], unit_frac[i])+ unit[i] + "\n";
  index += "Uptime: " + time_string() + "\n";
  index += "MQTTid: " + g_mqttid + "\n";
  index += "Temp:   " + String(g_temp,4) + "°C (last_published " +String(uptime - last_temp)+ "ms ago)\n";
  index += "http://" + IP + "/pulses for plain pulse count\n";
  index += "\n";
  for (i = 0; i < 2; i++) {
    if (check_vzserver(i)) {
      index += "current volkszaehler URL (" + label[i] +")\n";
      index += "http://" + g_vzhost + g_vzurl[i] + "\n";
      index += "Last push: " + String(last_push[i]) + " (" + String(uptime - last_push[i]) + "ms ago)\n";
      index += "Last value: " + String(g_pulses_sent[i]) + " (" + label[i]+ ")\n";
    }
  }
  if (check_mqserver()) {
    index += "\nMQTT server name:" + g_mqtthost +
             "\nMQTT server port:" + String(g_mqttport) +
             "\nMQTT topic:      " + g_mqtttopic + "\n";
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
  resp += g_vzhost;
  resp += "\"></td>"
      "</tr>\n<tr>";
  for (int i = 0; i < 2; i++) {
     resp += "<td>Volkszaehler URL " + label[i] + ":</td><td><input name=\"url" + String(i) +"\" value=\"";
    resp += g_vzurl[i];
    resp += "\"></td>";
    if (i > 0)
      resp += "<td><button type=\"submit\">Submit</button></td>";
    resp += "</tr>\n";
  }
  resp +=
    "</form>\n"
    "<tr><td><h2>Pulse correction</h2></td></tr>\n";
  for (int i = 0; i < 2; i++) {
    resp +=
      "<form action=\"/pulses.html\">"
        "<tr>"
          "<td>Pulses " + label[i] + " (multiplied with " + String(unit_factor[i], unit_frac[i]) + unit[i] +
          "):</td><td><input name=\"set" + String(i)+ "\" value=\"";
    resp += String(pulses[i]);
    resp += "\"></td>"
          "<td><button type=\"submit\">Submit</button></td>"
        "</tr>"
      "</form>\n";
  }
  resp +=
    "<tr><td><h1>Temperature Sensor Configuration</h2></td></tr>\n"
    "<form action=\"/vz\">"
      "<tr>"
        "<td>MQTT Server Hostname:</td><td><input name=\"mqhost\" value=\"";
  resp += g_mqtthost;
  resp += "\"></td>"
      "</tr>\n<tr>"
        "<td>MQTT Server Port:</td><td><input name=\"mqport\" value=\"";
  resp += g_mqttport;
  resp += "\"></td>"
      "</tr>\n<tr>"
        "<td>MQTT Topic:</td><td><input name=\"mqtopic\" value=\"";
  resp += g_mqtttopic;
  resp += "\"></td>"
        "<td><button type=\"submit\">Submit</button></td>"
      "</tr>\n"
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

/* helper to reduce duplicate code */
bool getArg(const char *name, String &arg) {
  if (!server.hasArg(name))
    return false;
  arg = server.arg(name);
  return true;
}

void handle_pulses() {
  String message;
  int ret = 200;
  String arg;
  const char *args[2] = { "set0", "set1" };
  bool noargs = true;
  for (int i = 0; i < 2; i++) {
    if (!getArg(args[i], arg))
      continue;
    noargs = false;
    uint32_t p = arg . toInt();
    if (p != 0) {
      if (p != pulses[i]) {
        message += label[i] + " pulses value set to " + String(p);
        pulses[i] = p;
      } else {
        message = "pulse value unchanged " + String(p);
      }
    } else {
      ret = 500;
      message += "invalid pulse value '" + arg + "'";
    }
  }
  if (noargs) {
    for (int i = 0; i < 2; i++)
      message += label[i] + ": " + String(pulses[i]) + "\n";
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
  String arg;
  const char *args[2] = { "set0", "set1" };
  bool noargs = true;
  for (int i = 0; i < 2; i++) {
    if (!getArg(args[i], arg))
      continue;
    noargs = false;
    uint32_t p = arg . toInt();
    if (p != 0) {
      if (p != pulses[i]) {
        message += label[i] + " pulses value set to " + String(p);
        pulses[i] = p;
      } else {
        message += label[i] + " pulse value unchanged " + String(p);
      }
    } else {
      ret = 500;
      message += "invalid pulse value '" + arg + "'";
    }
  }
  if (noargs) {
    for (int i = 0; i < 2; i++)
      message += label[i] + ": " + String(pulses[i]) + "\n";
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

#define PREF_ISSUE1 1
#if PREF_ISSUE1
  /* workaround for
   * https://github.com/vshymanskyy/Preferences/issues/1
   * until https://github.com/vshymanskyy/Preferences/pull/2
   * is merged or the issue is fixed otherwise */
void pref_putString(const char *key, String value) {
  if (value.isEmpty())
    pref.putChar(key, 0);
  else
    pref.putString(key, value);
}
#endif

void prefs_save() {
  pref.begin("wasserzaehler", false);
  pref.putInt("version", 2);
  for (int i = 0; i < 2; i++) {
    pref.putUInt(_pulses[i], g_pulses[i]);
    pref.putUInt(_pulses_sent[i], g_pulses_sent[i]);
  }
#if PREF_ISSUE1
  pref_putString("vzhost", g_vzhost);
  for (int i = 0; i < 2; i++)
    pref_putString(_vzurl[i], g_vzurl[i]);
  pref_putString("mqtthost", g_mqtthost);
  pref_putString("mqtttopic", g_mqtttopic);
#else
  pref.putString("vzhost", g_vzhost);
  for (int i = 0; i < 2; i++)
    pref.putString(_vzurl[i], g_vzurl[i]);
  pref.putString("mqtthost", g_mqtthost);
  pref.putString("mqtttopic", g_mqtttopic);
#endif
  pref.putUShort("mqttport", g_mqttport);
  pref.end();
}

void handle_vz() {
  String message = "";
  int ret = 200;
  bool change = false;
  String arg;
  if (getArg("host", arg)) {
    if (!checkhost(arg.c_str(), arg.length())) {
      message = "invalid hostname (only a-z,A-Z,- allowed)\n";
    } else {
      if (g_vzhost.compareTo(arg)) {
        message = "Set hostname to " + arg + "\n";
        g_vzhost = arg;
        change = true;
      } else
        message = "hostname not changed\n";
    }
  }
  int i = 0;
  static const char *args[] = { "url0", "url1", "url" };
  for (int j = 0; j < 3; j++) {
    if (!getArg(args[j], arg))
      continue;
    if (arg.length() > 0 && arg[0]!= '/') {
      message += "url path must start with '/'\n";
    } else {
      i = (j == 1); /* 0 and 2 => 0, 1 => 1 */
      if (g_vzurl[i].compareTo(arg)) {
        message += "Set " + label[i] + " URL to " + arg + "\n";
        g_vzurl[i] = arg;
        change = true;
      } else
        message += label[i] + " URL not changed\n";
    }
  }
  if (getArg("mqhost", arg)) {
    if (!checkhost(arg.c_str(), arg.length())) {
      message = "invalid MQTT hostname (only a-z,A-Z,- allowed)\n";
    } else {
      if (g_mqtthost.compareTo(arg)) {
        message += "Set MQTT hostname to " + arg + "\n";
        g_mqtthost = arg;
        change = true;
        mqtt_changed = true;
      } else
        message += "MQTT hostname not changed\n";
    }
  }
  if (getArg("mqport", arg)) {
    uint32_t p = arg . toInt();
    if (p != 0) {
      if (p != g_mqttport) {
        message += "MQTT port set to " + String(p) + "\n";
        g_mqttport = p;
        change = true;
        mqtt_changed = true;
      } else {
        message += "MQTT port not changed\n";
      }
    } else {
      ret = 500;
      message += "invalid MQTT port value '" + arg + "'\n";
    }
  }
  if (getArg("mqtopic", arg)) {
    if (g_mqtttopic.compareTo(arg)) {
      message += "Set MQTT Topic to " + arg + "\n";
      g_mqtttopic = arg;
      change = true;
      mqtt_changed = true;
    } else
      message += "MQTT Topic not changed\n";
  }
  if (! message.isEmpty())
    message += "\n";
  message += "VZ host:    " + g_vzhost + "\n";
  for (i = 0; i < 2; i++)
    message += "VZ URL:     " + g_vzurl[i] + " (" + label[i] + ")\n";
  message += "MQTT host:  " + g_mqtthost + "\n";
  message += "MQTT port:  " + String(g_mqttport) + "\n";
  message += "MQTT topic: " + g_mqtttopic + "\n";
  message += "VZ host: " + g_vzhost + "\n";
  server.send(ret, "text/plain", message);
  if (change)
    prefs_save();
}

long code_from_str(const String s) {
  if (!s.startsWith("HTTP/1."))
    return -1;
  String c = s.substring(9);
  if (c.length() < 3)
    return -1;
  return c.toInt();
}

bool vz_push(int count, int i = 0) {
  String response = "";
  if (!check_vzserver(i))
    return false;
  if (! client.connect(g_vzhost, 80)) {
    Serial.println("Connect to volkszaehler server failed");
    client.stop();
    return false;
  }
  int p = g_pulses_sent[i];
  if (count > p) {
    p = count;
  }
  String cmd = "GET " + g_vzurl[i];
  cmd += "?operation=add&value=" + String(p * unit_factor[i], unit_frac[i]);
  log_time();
  Serial.println(cmd);
  cmd += " HTTP/1.1\r\nHost: " + g_vzhost;
  cmd += "\r\n";
  cmd += "Content-Type: application/json\r\n";
  cmd += "Connection: keep-alive\r\nAccept: */*\r\n\r\n";
  client.print(cmd);
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      log_time();
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
  log_time();
  Serial.print("Return line: " + response);
  long ret = code_from_str(response);
  if (ret > 0)
    Serial.println(" => code: " + String(ret));
  else
    Serial.println(" => invalid return code?");
  if (ret == 200) {
    g_pulses_sent[i] = p;
    return true;
  }
  return false;
}

bool temp_request = false;
void update_temp() {
#if 0
  log_time();
  Serial.println(String(__func__));
#endif
  sensor.requestTemperatures();
  temp_request = true;
}

void commit_config() {
  log_time();
  Serial.printf("COMMIT! last_p: (%d, %d), pulse: (%d, %d)\r\n",
                last_pulse[0], last_pulse[1], g_pulses[0], g_pulses[1]);
  bool doit = false;
  for (int i = 0; i < 2; i++) {
    if (g_pulses[i] != last_pulse[i])
      doit = true;
    last_pulse[i] = g_pulses[i];
  }
  if (doit)
    prefs_save();
}

void trigger_push(int i) {
  log_time();
  Serial.println(String(__func__)+ " " + String(i));
  update_push[i] = true;
}

Ticker temp_timer;
Ticker commit_timer;
Ticker push_timer[2];
void setup() {
  Serial.begin(115200);
  delay(10);
  // Serial.setDebugOutput(true); // send additional debug infos to serial
  for (int i=0; i < 10; i++)
    Serial.println("xxxxxxxxxxxxxxxxxxxxxxxxxxxx"); // bootloader has clobbered serial monitor
  delay(100);
  /* works good enough for me without external pullup */
  pinMode(tempPin, INPUT_PULLUP);
  sensor.begin();
  if (sensor.getAddress(DS18B20_Address, 0)) {
    char tmp[10];
    sensor.setResolution(DS18B20_Address, 12); /* 12 bits, 0.0625°C, 750ms/conversion */
    g_mqttid = "DS18B20-";
    Serial.print("Sensor addr: ");
    for (byte j = 0; j < 8; j++) {
      sprintf(tmp, "%02X", DS18B20_Address[j]);
      g_mqttid += tmp;
      Serial.print(tmp);
      if (j < 7)
        Serial.print(":");
    }
    Serial.println();
    sensor.setWaitForConversion(false);
    sensor.requestTemperatures();
    temp_timer.attach_scheduled(20, update_temp);
  } else {
    Serial.println("Sensor Address failed?");
  }

  pref.begin("wasserzaehler", false);
  int version = pref.getInt("version", -1);
  if (version == -1) {
    /* check if old config in EEPROM class exists */
    pref.end();
    Serial.println("Checking for old config...");
    eeprom_state persist;
    EEPROM.begin(512);
    EEPROM.get(0, persist);
    EEPROM.end();
    if (persist.sig[0] == 0xff || memcmp(persist.sig, "WATER1", 6)) {
      Serial.println("No old config...");
      /* all initialized globally */
    } else {
      Serial.print("Read pulses from eeprom: ");
      Serial.println(persist.pulses);
      g_pulses[0] = persist.pulses;
      g_pulses_sent[0] = persist.pulses_sent;
      g_vzhost = String(persist.vzhost);
      g_vzurl[0] = String(persist.vzurl);
    }
    prefs_save();
  } else {
    Serial.println("reading Preferences...");
    g_vzhost = pref.getString("vzhost");
    g_mqttport = pref.getUShort("mqttport", g_mqttport);
    g_mqtthost = pref.getString("mqtthost");
    g_mqtttopic = pref.getString("mqtttopic");
    mqtt_changed = check_mqserver();
    if (version < 2) {
      g_pulses[0] = pref.getUInt("pulses", 0);
      g_pulses_sent[0] = pref.getUInt("pulses_sent", 0);
      g_vzurl[0] = pref.getString("vzurl");
      pref.remove("pulses");
      pref.remove("pulses_sent");
      pref.remove("vzurl");
      pref.end();
      prefs_save();
    } else {
      for (int i = 0; i < 2; i++) {
        g_vzurl[i] = pref.getString(_vzurl[i]);
        g_pulses[i] = pref.getUInt(_pulses[i], 0);
        g_pulses_sent[i] = pref.getUInt(_pulses_sent[i], 0);
      }
      pref.end();
    }
  }
  Serial.print("VZhost: ");
  Serial.println(g_vzhost);
  for (int i = 0; i < 2; i ++) {
    pulses[i] = g_pulses[i];
    Serial.println("=== " + label[i] + " ===");
    Serial.print("VZurl:  ");
    Serial.println(g_vzurl[i]);
    Serial.print("Pulses: ");
    Serial.println(g_pulses[i]);
    Serial.print("Sent:   ");
    Serial.println(g_pulses_sent[i]);
  }
  Serial.print("MQTTID: ");
  Serial.println(g_mqttid);
  Serial.print("MQhost: ");
  Serial.println(g_mqtthost);
  Serial.print("MQport: ");
  Serial.println(g_mqttport);
  Serial.print("MQtopic:");
  Serial.println(g_mqtttopic);

  Serial.println();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(inputPin[0], INPUT_PULLUP);
  pinMode(inputPin[1], INPUT_PULLUP);
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
  attachInterrupt(digitalPinToInterrupt(inputPin[0]), isr0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(inputPin[1]), isr1, CHANGE);
  /* attach_scheduled to avoid running in SYS CTX which might cause
   * LITTLEFS to trigger watchdog timeout resets */
  commit_timer.attach_scheduled(60, commit_config);
  for (int i = 0;i < 2; i++)
    push_timer[i].attach(60, trigger_push, i);
}

/* milliseconds for blinking patterns */
#define BLINKPERIOD 100
/* delay in the loop() function */
#define BLINKDELAY 10
const int loop_len = strlen(_s[0]) * (BLINKPERIOD/BLINKDELAY);
unsigned long last_mqtt_reconnect = 0;
int i = 0;
void loop() {
  WiFiStatusCheck();
  if (_s[state][i/(BLINKPERIOD/BLINKDELAY)] == '1')
    digitalWrite(LED_BUILTIN, LED_ON);   // turn the LED on (HIGH is the voltage level)
  else
    digitalWrite(LED_BUILTIN, LED_OFF);    // turn the LED off by making the voltage LOW
  i++;
  // i %= loop_len;
  if (i >= loop_len)
    i = 0;
  if (digitalRead(buttonPin) == LOW) {
    Serial.println("Triggering WPS!");
    start_WPS();
  }
  
  for (int j = 0; j < 2; j++) {
    if (g_pulses[j] != pulses[j] || update_push[j]) {
      log_time();
      Serial.printf("Pulse update: %d (%d %s)\r\n", pulses[j], j, label[j].c_str());
      if (vz_push(pulses[j], j)) {
        if (!update_push[j])
          push_timer[j].attach(60, trigger_push, j); /* re-arm if not triggered by timer */
        last_push[j] = millis();
      }
      update_push[j] = false;
    }
    g_pulses[j] = pulses[j];
  }
  /* mqtt stuff */
  if (mqtt_changed) {
    Serial.println("MQTT config changed. Dis- and reconnecting...");
    mqtt_changed = false;
    mqtt_client.disconnect();
    if (check_mqserver())
      mqtt_client.setServer(g_mqtthost.c_str(), g_mqttport);
    else
      Serial.println("MQTT server name not configured");
    mqtt_client.setKeepAlive(60); /* same as python's paho.mqtt.client */
    Serial.print("MQTT SERVER: "); Serial.println(g_mqtthost);
    Serial.print("MQTT PORT:   "); Serial.println(g_mqttport);
    last_mqtt_reconnect = 0; /* trigger connect() */
  }
  unsigned long now = millis();
  if (!mqtt_client.connected() && now - last_mqtt_reconnect > 5 * 1000) {
    if (check_mqserver()) {
      Serial.print("MQTT RECONNECT...");
      if (mqtt_client.connect(g_mqttid.c_str()))
        Serial.println("OK!");
      else
        Serial.println("FAILED");
    }
    last_mqtt_reconnect = now;
  }
  // mqtt_ok = mqtt_client.connected();
  /* temp_request is set by temp_timer/update_temp() */
  if (temp_request) {
    if (sensor.isConversionComplete()) {
      int16_t _T = sensor.getTemp(DS18B20_Address);
      g_temp = _T / 128.0f;
      log_time();
      Serial.printf("T: %.4f °C ", g_temp);
      Serial.println(String(g_temp, 1));
      temp_request = false;
      if (_T != DEVICE_DISCONNECTED_RAW && check_mqserver()) {
        last_temp = millis();
        mqtt_client.publish((g_mqtttopic).c_str(), String(g_temp, 1).c_str());
      }
    }
  }

  server.handleClient();
  delay(BLINKDELAY);
}
