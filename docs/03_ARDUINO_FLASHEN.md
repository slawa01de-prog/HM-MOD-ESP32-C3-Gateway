# ESP32-C3 mit Arduino IDE flashen

## Arduino-IDE-Einstellungen

Getestete Einstellungen:

```text
Board:             ESP32C3 Dev Module
USB CDC On Boot:   Enabled
CPU Frequency:     160 MHz
Flash Frequency:   80 MHz
Flash Size:        4 MB
Partition Scheme:  Default 4MB with SPIFFS
Upload Speed:      921600
```

Verwendeter Arduino-ESP32-Core im Test:

```text
esp32 3.3.11
```

## Flashen

1. Datei öffnen:

```text
firmware/stable/HM_MOD_ESP32C3_Gateway_v3_1_4.ino
```

2. ESP32-C3 per USB anschließen.
3. Passenden COM-Port auswählen.
4. Kompilieren und hochladen.
5. Seriellen Monitor mit **115200 Baud** öffnen.

Ein normaler Start enthält ungefähr:

```text
HM-MOD ESP32-C3 GATEWAY v3.1.4 PRO
HM reset: Startup
HM boot mode: Co_CPU_BL
HM application: DualCoPro_App
HM info: HM-MOD-RPI-PCB FW 2.8.6
WiFi connected: ...
WebUI started
WiFi power save: OFF (HB-RF-ETH low latency)
Gateway ready: HB-RF-ETH UDP/3008
```

## WLAN

Die Zugangsdaten werden im ESP-NVS unter dem Namespace `hmc3` gespeichert.

Nach einem Firmwareupdate bleiben die WLAN-Daten normalerweise erhalten.

Für einen produktiven Betrieb sollte die Gateway-IP im Router per DHCP-Reservation fest vergeben
werden.
