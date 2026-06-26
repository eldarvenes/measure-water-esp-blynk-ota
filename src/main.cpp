// Vasstank-overvaking - Blynk + HTTP-pull OTA
// Sensor-, varsel-, rele- og e-postlogikk frå InfluxDB-varianten.
// Alle secrets (WiFi, Blynk-token, SMTP) lastast frå data/config.json paa
// LittleFS ved oppstart - dei ligg difor ALDRI i den kompilerte .bin-fila,
// som dermed trygt kan leggjast som offentleg GitHub-release for OTA.

#include "config.h"                 // berre Blynk-mal (ikkje-hemmeleg)
#define BLYNK_PRINT Serial

#include <ESP8266WiFiMulti.h>
#include <BlynkSimpleEsp8266.h>
#include <ESP_Mail_Client.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>

#define DEVICE      "ESP8266"
#define FW_VERSION  "2026-06-26.3"   // bump for kvar release - berre for logging

#define VPIN_LEVEL  V5               // nivaverdi (liter) -> Blynk
#define VPIN_OTA    V10              // knapp i Blynk-appen som utloyser OTA

// ---------- config frå LittleFS (data/config.json) ----------
String cfgWifiSsid, cfgWifiPass, cfgBlynkToken;
String cfgSmtpHost, cfgAuthorEmail, cfgAuthorPass, cfgRecip1, cfgRecip2;
String cfgFirmwareUrl;
String cfgHomeyUrl;             // Homey sky-webhook (tom = av)
int    cfgSmtpPort = 465;

// ---------- pinnar ----------
const int criticalLed = D1;
const int relayPin    = D0;
const int sensorPin   = A0;
const int wifiLed     = D2;

// ---------- niva/varsel ----------
float sensorValue = 0;
// Terskelar i liter på 0–2000-skalaen (2 × 1000 L tankar med felles nivå).
const float warningResetLimit = 1800;
const float warningLimitLow   = 1600;
const float criticalLimitLow  = 400;
boolean warningSent = false;
boolean criticalWarningSent = false;

ESP8266WiFiMulti wifiMulti;
SMTPSession smtp;
BlynkTimer timer;

bool loadConfig();
void readAndSend();
void doOtaUpdate();
void checkIfCriticalLevel(float level);
void checkIfWarningShouldBeSent(float level);
void resetWarning(float level);
void printDebug();
void sendEmail(float msg, String textMsg);
void sendHomey(float level, String textMsg);
String urlEncode(const String& s);

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Vasstank fw " FW_VERSION);
  //smtp.debug(1);

  pinMode(relayPin, OUTPUT);
  pinMode(criticalLed, OUTPUT);
  pinMode(wifiLed, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);     // innebygd LED = Blynk-status (aktiv lag)
  digitalWrite(LED_BUILTIN, HIGH);  // av til vi er kopla

  if (!loadConfig()) {
    Serial.println("ADVARSEL: config.json mangla/ugyldig - sjekk 'pio run -t uploadfs'.");
  }

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  Serial.print("Connecting to wifi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    digitalWrite(wifiLed, LOW);
    delay(100);
  }
  digitalWrite(wifiLed, HIGH);
  Serial.println();

  // OTA (lokalnett) fyrst - uavhengig av Blynk.
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.begin();   // utan passord - privat nett

  // Blynk: berre konfigurer. Tilkopling skjer ikkje-blokkerande i loop()
  // via Blynk.run(), so manglande/feil token blokkerer ALDRI WiFi/OTA.
  // For EU-region: Blynk.config(cfgBlynkToken.c_str(), "fra1.blynk.cloud", 80);
  Blynk.config(cfgBlynkToken.c_str());

  timer.setInterval(60000L, readAndSend);
  readAndSend();  // ei forste maling med ein gong
}

void loop() {
  ArduinoOTA.handle();   // lokalnett-OTA
  Blynk.run();
  timer.run();

  // Bench-test: send 'u' over seriell for å utløyse HTTP-pull OTA (utan Blynk).
  if (Serial.available() && Serial.read() == 'u') {
    Serial.println("OTA utloyst frå seriell");
    doOtaUpdate();
  }
}

// Blynk-knapp (V10) som utloyser HTTP-pull OTA fraa GitHub-releasen.
BLYNK_WRITE(VPIN_OTA) {
  if (param.asInt() == 1) {
    Serial.println("OTA utloyst frå Blynk");
    doOtaUpdate();
  }
}

bool loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount feila");
    return false;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println("/config.json finst ikkje");
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("config.json parse-feil: ");
    Serial.println(err.c_str());
    return false;
  }
  cfgWifiSsid    = (const char*)(doc["wifi_ssid"]     | "");
  cfgWifiPass    = (const char*)(doc["wifi_password"] | "");
  cfgBlynkToken  = (const char*)(doc["blynk_token"]   | "");
  cfgSmtpHost    = (const char*)(doc["smtp_host"]     | "");
  cfgSmtpPort    = doc["smtp_port"] | 465;
  cfgAuthorEmail = (const char*)(doc["author_email"]    | "");
  cfgAuthorPass  = (const char*)(doc["author_password"] | "");
  cfgRecip1      = (const char*)(doc["recipient_email"]  | "");
  cfgRecip2      = (const char*)(doc["recipient_email2"] | "");
  cfgFirmwareUrl = (const char*)(doc["firmware_url"]     | "");
  cfgHomeyUrl    = (const char*)(doc["homey_webhook_url"] | "");
  Serial.println("config.json lasta OK");
  return true;
}

void doOtaUpdate() {
  if (cfgFirmwareUrl.length() == 0) {
    Serial.println("OTA: ingen firmware_url i config.json");
    return;
  }
  Serial.println("OTA: hentar " + cfgFirmwareUrl);

  WiFiClientSecure client;
  client.setInsecure();   // hoppar over sertifikatvalidering (hobby/privat)
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub redirectar
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, cfgFirmwareUrl);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA feila (%d): %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: ingen oppdatering");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA OK - rebootar");  // rebootar automatisk
      break;
  }
}

void readAndSend() {
  float measuredValue = 0;
  for (int i = 0; i < 15; i++) {
    measuredValue = measuredValue + analogRead(sensorPin);
    delay(25);
  }
  sensorValue = measuredValue / 15;
  sensorValue = sensorValue - 247;        // ~4 mA-nullpunkt (rastall ved tom tank)
  sensorValue = sensorValue / 777 * 2000; // 777 = spennet til ~20 mA -> 0..2000 liter (2 x 1000 L, felles nivå)

  Serial.print("Writing: ");
  Serial.println(sensorValue);
  Blynk.virtualWrite(VPIN_LEVEL, sensorValue);

  // Mist WiFi? Prov a koble til igjen.
  if ((WiFi.RSSI() == 0) && (wifiMulti.run() != WL_CONNECTED)) {
    Serial.println("Wifi connection lost");
    digitalWrite(wifiLed, LOW);
  } else {
    digitalWrite(wifiLed, HIGH);
  }

  // Innebygd LED lyser nar Blynk er kopla til (aktiv lag).
  digitalWrite(LED_BUILTIN, Blynk.connected() ? LOW : HIGH);

  resetWarning(sensorValue);
  checkIfWarningShouldBeSent(sensorValue);
  checkIfCriticalLevel(sensorValue);
  //printDebug();
}

void checkIfCriticalLevel(float level) {
  if (level < criticalLimitLow && criticalWarningSent == false) {
    sendEmail(level, "Vanntanknivaa er kritisk lavt!");
    Serial.println("send email critical");
    criticalWarningSent = true;
    digitalWrite(criticalLed, HIGH);
    digitalWrite(relayPin, HIGH);
  }
}

void checkIfWarningShouldBeSent(float level) {
  if (level < warningLimitLow && warningSent == false) {
    sendEmail(level, "Vanntanknivaa er lavt");
    Serial.println("send email lavt");
    warningSent = true;
  }
}

void resetWarning(float level) {
  if (level > warningResetLimit && warningSent == true) {
    sendEmail(level, "Vanntanknivaa er normalt");
    Serial.println("send email normalt");
    digitalWrite(criticalLed, LOW);
    digitalWrite(relayPin, LOW);
    warningSent = false;
    criticalWarningSent = false;
  }
}

void printDebug() {
  Serial.print("warningsent: ");
  Serial.println(warningSent);
  Serial.print("criticalWarningsent: ");
  Serial.println(criticalWarningSent);
}

void sendEmail(float msg, String textMsg) {
  // Same varsel til Homey, uavhengig av om SMTP er sett opp.
  sendHomey(msg, textMsg);

  if (cfgSmtpHost.length() == 0) {
    Serial.println("SMTP ikkje konfigurert - hoppar over e-post");
    return;
  }

  char result[8];
  dtostrf(msg, 6, 2, result);

  ESP_Mail_Session session;

  session.server.host_name = cfgSmtpHost;
  session.server.port      = cfgSmtpPort;
  session.login.email      = cfgAuthorEmail;
  session.login.password   = cfgAuthorPass;
  session.login.user_domain = "";

  SMTP_Message message;

  message.sender.name  = "Vasstank";
  message.sender.email = cfgAuthorEmail;
  message.subject      = result;
  message.addRecipient("Mottakar 1", cfgRecip1);
  if (cfgRecip2.length() > 0)            // tom mottakar-2 kan få Gmail til å avvise heile meldinga
    message.addRecipient("Mottakar 2", cfgRecip2);

  message.text.content           = textMsg.c_str();
  message.text.charSet           = "us-ascii";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success |
                            esp_mail_smtp_notify_failure |
                            esp_mail_smtp_notify_delay;

  if (!smtp.connect(&session))
    return;

  if (!MailClient.sendMail(&smtp, &message))
    Serial.println("Error sending Email, " + smtp.errorReason());
}

// Prosent-enkodar alt som ikkje er bokstav/tal, så meldinga trygt kan stå i ?tag=
// (mellomrom, æøå og emoji blir korrekt UTF-8-enkoda byte for byte).
String urlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c)) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Varsel til Homey via sky-webhook (Logic "webhook event mottatt"). Meldinga
// går på ?tag=, tilgjengeleg som token i Homey-flowen. Feilar stille (loggar
// berre) så Homey-trøbbel aldri stoppar resten av varslinga.
void sendHomey(float level, String textMsg) {
  if (cfgHomeyUrl.length() == 0) {
    Serial.println("Homey-webhook ikkje konfigurert - hoppar over");
    return;
  }
  char lvl[8];
  dtostrf(level, 0, 0, lvl);                  // heiltal liter
  String tag = textMsg + " (" + lvl + " L)";
  String url = cfgHomeyUrl + "?tag=" + urlEncode(tag);

  WiFiClientSecure client;
  client.setInsecure();                       // hobby/privat - hopp over cert-validering
  HTTPClient https;
  if (https.begin(client, url)) {
    int code = https.GET();
    Serial.printf("Homey-webhook: HTTP %d\n", code);
    https.end();
  } else {
    Serial.println("Homey-webhook: begin() feila");
  }
}
