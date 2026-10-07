# Gateway WebUI und Diagnose

## Seiten

Die v3.1-Familie besitzt folgende Bereiche:

```text
Dashboard
Funk & Relay
System-Log
HM-Firmware
ESP-Update
Einstellungen
```

## Dashboard

Wichtige Werte:

- Gateway-IP und RSSI
- OpenCCU/HB-RF-ETH Status
- Funkmodul und Firmware
- Seriennummer / SGTIN
- HmIP-/BidCos-Adresse
- Traffic-Zähler
- CRC-Fehler / Drops
- Uptime / Heap

## Normaler stabiler Zustand

```text
1 verbunden / 1 gestartet
DualCoPro_App
Firmware 2.8.6
CRC-Fehler 0
Drops 0
Traffic in beide Richtungen
```

## System-Log

Ein typischer OpenCCU-Start kann enthalten:

```text
HB CONNECT
HB STARTCONN
HM reset: OpenCCU
```

Mehrere vom OpenCCU angeforderte HM-Resets während der Erkennung sind möglich.

## WiFi-Powersave

Im Hotfix v3.1.1 wurde WiFi-Powersave deaktiviert:

```text
WiFi power save: OFF (HB-RF-ETH low latency)
```

Grund: Der Linux-HB-RF-ETH-Treiber wartet beim Verbindungsaufbau nur sehr kurz auf die
CONNECT-Antwort. WLAN-Energiesparlatenz führte zuvor zu Reconnect-Stürmen.

## Bekannter Anzeige-Bug: HmIP-Adresse

Die passive WebUI-Auswertung kann eine falsche HmIP-Adresse anzeigen. Maßgeblich ist die
OpenCCU-Hardwareinfo.
