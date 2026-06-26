#ifndef CONFIG_H
#define CONFIG_H

// Berre Blynk-malen ligg her. Den er IKKJE ein hemmelegheit (ikkje auth-token),
// men Blynk-biblioteket krev at desse er #define-a på kompileringstid.
// Alle ekte secrets (WiFi, auth-token, SMTP) ligg i data/config.json på
// einingas LittleFS - dei hamnar difor ALDRI i den kompilerte .bin-fila.
#define BLYNK_TEMPLATE_ID   "TMPL4dpuPH2pm"        // Device Info -> Template ID
#define BLYNK_TEMPLATE_NAME "Vasstank template"     // namnet på malen

// mDNS-namn for lokal ArduinoOTA (vasstank.local). Ikkje hemmeleg.
#define OTA_HOSTNAME        "vasstank"

#endif // CONFIG_H
