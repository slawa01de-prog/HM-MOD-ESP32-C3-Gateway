# Backup, Wiederherstellung und Wartung

## Was sichern?

Besonders wichtig:

```text
firmware/stable/HM_MOD_ESP32C3_Gateway_v3_1_1.ino
docs/
PROJECT_MANIFEST.json
```

## ESP-Einstellungen

WLAN-Zugangsdaten liegen im NVS des ESP32-C3 und sind nicht im Repository enthalten.

## OpenCCU

OpenCCU separat über die eingebaute Backup-Funktion sichern.

Die HB-RF-ETH-Gateway-IP muss nach einer Wiederherstellung ggf. erneut unter:

```text
Systemsteuerung -> Erweiterte Einstellungen -> IP-Adresse (HB-RF-ETH)
```

gesetzt werden.

## Feste IP

Empfohlen wird eine DHCP-Reservierung für die MAC-Adresse des ESP32-C3.

## ESP-Firmware-OTA

Die WebUI besitzt einen ESP-Firmwareupdate-Bereich.

Nur passende ESP32-C3-Binaries verwenden. Ein `.eq3`-Funkmodul-Update gehört dagegen in den
HM-Firmware-Bereich.

## Sicherheitsaspekt

Die WebUI ist aktuell nicht durch Benutzername/Passwort geschützt.

Deshalb:

- nur im internen LAN betreiben
- keine direkte Portweiterleitung aus dem Internet
- keine öffentliche Freigabe von HTTP Port 80 / UDP 3008
