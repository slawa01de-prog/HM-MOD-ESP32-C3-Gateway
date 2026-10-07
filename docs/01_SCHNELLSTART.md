# Schnellstart

## Hardware

```text
ESP GPIO5  TX -> HM Pin 8 RX
ESP GPIO6  RX <- HM Pin 10 TX
ESP GPIO7     -> HM Pin 12 RESET
ESP 3V3      -> HM Pin 1 3V3
ESP GND      -> HM GND
```

## ESP flashen

```text
Board: ESP32C3 Dev Module
USB CDC On Boot: Enabled
Flash: 4 MB
Partition: Default 4MB with SPIFFS
```

Sketch:

```text
firmware/stable/HM_MOD_ESP32C3_Gateway_v3_1_1.ino
```

## OpenCCU

```text
Systemsteuerung -> Erweiterte Einstellungen
IP-Adresse (HB-RF-ETH) = IP des ESP
```

OpenCCU neu starten.

## Erfolg prüfen

```text
1 verbunden / 1 gestartet
DualCoPro_App
Firmware 2.8.6
CRC-Fehler 0
Drops 0
```

Dann ein Homematic-Gerät bedienen und prüfen, ob beide Traffic-Zähler steigen.
