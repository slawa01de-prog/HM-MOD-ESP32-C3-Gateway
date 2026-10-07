# Projekt-Changelog

## v1

- erster ESP32-C3 HB-RF-ETH-Dongle
- UDP 3008
- UART 115200
- einfache WebUI
- erster funktionierender Transport

## v2

- vollständige HM-UART-Frames statt beliebiger Byte-Chunks
- näher am originalen HB-RF-ETH-Verhalten
- bessere Diagnose

## v3

- integrierter HM-MOD `.eq3`-Firmware-Updater
- erfolgreiches Schreiben der offiziellen Firmware 2.8.6
- erste Nachprüfung erkannte `DualCoPro_App` noch nicht korrekt

## v3.1 Pro

- DualCoPro-Unterstützung
- Firmware 2.8.6-Erkennung
- Seriennummer / SGTIN / RF-Adressen
- modernes Dashboard
- Funk-&-Relay-Diagnose
- System-Log
- ESP-OTA
- Einstellungen

## v3.1.1 Hotfix – praktisch getesteter Stand

- WiFi Powersave deaktiviert
- CONNECT-Antwort auf HB-RF-ETH priorisiert
- UDP-Rebind derselben OpenCCU-IP verbessert
- WLAN-Reconnect ohne aktives Disconnect
- HB-RF-ETH-Bearbeitung priorisiert

Ergebnis im Test:

```text
1 verbunden / 1 gestartet
DualCoPro_App
Firmware 2.8.6
CRC Fehler 0
Drops 0
BidCos-Gerät erfolgreich geschaltet
```

## Geplante nächste Verbesserungen

- HmIP-Adresse in der WebUI nur aus eindeutig passendem Antwortframe übernehmen
- `HM-Modul Reset` und `Modulinfo neu lesen` sperren, wenn OpenCCU aktiv ist
- eindeutige Versionsanzeige überall
- optional Web-Login
- optional MQTT / Home Assistant Auto-Discovery
- Backup/Restore der Gateway-Einstellungen
