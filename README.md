# measure-water-esp-blynk-ota

Vasstank-overvaking på ein WeMos D1 mini (ESP8266): 4–20 mA trykksensor → 0–1000
liter → Blynk. E-post- og Homey-varsel + relé (pumpevern) ved låge nivå. **Oppdatering på
avstand via HTTP-pull OTA**, utløyst frå ein knapp i Blynk-appen.

> Status: OTA-flyten (Blynk-knapp → GitHub-release → self-flash) er **verifisert
> ende-til-ende** på ESP8266-maskinvare.

## Korleis det heng saman
- **Secrets på eininga, ikkje i binæren.** WiFi, Blynk-token og SMTP ligg i
  `data/config.json` på LittleFS og lastast ved oppstart. Den kompilerte
  `firmware.bin` inneheld difor ingen hemmelegheiter og kan liggje som
  **offentleg** GitHub-release.
- **OTA går utgåande** frå eininga (ESP → GitHub over HTTPS), så ho oppdaterer
  seg sjølv frå kvar som helst — du treng ikkje vere på same nett.

> Repoet **må vere offentleg** — GitHub serverer ikkje release-filer frå private
> repo anonymt, og ESP-en lastar ned utan innlogging.

## Secrets — `data/config.json`
Kopier `config.json.example` → `data/config.json` (gitignorert) og fyll inn:
- `wifi_ssid` / `wifi_password`
- `blynk_token` — Auth Token frå **device-en** (Device Info)
- `smtp_*` — valfritt; tomt `smtp_host` hoppar over e-post
- `homey_webhook_url` — valfritt; Homey sky-webhook (Flow → NÅR: Logikk
  «webhook event mottatt» → kopier ⓘ-URL-en). Tomt = av. Eininga legg sjølv
  på `?tag=<melding>`, t.d. `Vanntanknivaa er lavt (1540 L)`.
- `firmware_url` — peikar på «latest release» i dette repoet

`include/config.h` har berre `BLYNK_TEMPLATE_ID`/`NAME` (kompileringskrav, ikkje
hemmeleg) + `OTA_HOSTNAME`, og vert committa.

## Blynk-oppsett
Lag **éin Template** (her: `Vasstank template`, ID `TMPL4dpuPH2pm`) og **éin
Device** under den (Auth Token → `data/config.json`). Datastraumar lagast på
**mal-nivå**:

| Pin | Type | Bruk |
|-----|------|------|
| V5  | Double (0–1000, `L`) | vassnivå — Gauge/Chart-widget |
| V10 | Integer (0–1) | OTA-trigger — **Button-widget, Mode: Push** |

> Hugs: ein datastraum åleine er ikkje trykkbar — du må leggje ein **Button-widget**
> på dashbordet kopla til V10 (Push) for å kunne utløyse OTA.

## Første flash (USB)
```bash
pio run -e d1_mini -t uploadfs   # secrets (config.json) → LittleFS
pio run -e d1_mini -t upload     # firmware
pio device monitor               # (valfritt) sjå at WiFi/Blynk koplar ("Ready")
```
Filsystemet overlever OTA, så `uploadfs` treng du berre på nytt om du endrar
`config.json` (t.d. nytt WiFi/token).

## Oppdatere på avstand (OTA via Blynk-knapp)
1. Bump `FW_VERSION` i `src/main.cpp`, og bygg: `pio run -e d1_mini`
2. Lag ein GitHub-release med firmware-fila:
   ```bash
   gh release create <tag> .pio/build/d1_mini/firmware.bin -t "<tittel>"
   ```
3. Trykk **V10**-knappen i Blynk-appen → eininga hentar «latest release» og
   flashar seg sjølv (rebootar automatisk). TLS-sertifikat hoppast over
   (`setInsecure`); GitHub-redirect følgjast.

## Bench-test
`[env:nodemcu_test]` byggjer for ein vanleg NodeMCU 8266 (same chip) til testing.
I tillegg til V10-knappen kan OTA utløysast over **seriell**: send `u` i
seriell-monitoren → `doOtaUpdate()` køyrer. Nyttig for å prøve OTA utan Blynk.

## Lokal OTA (same WiFi som eininga)
`pio run -e d1_mini_ota -t upload` (via `vasstank.local`).

## Står att
- Relé fail-safe-logikk (energiser-for-å-køyre) når 230 V-delen koblast.
- Kalibrering av `247`/`777` + terskelane (200/800/900 l) mot reelle målingar.
