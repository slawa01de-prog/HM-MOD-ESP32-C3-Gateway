# HM-MOD-RPI-PCB + ESP32-C3 Netzwerk-Gateway

## Projektstand

**Stand: 07.10.2026**

Dieses Archiv dokumentiert den erfolgreich getesteten Aufbau eines WLAN-Netzwerk-Gateways für ein
**eQ-3 HM-MOD-RPI-PCB** mit einem **ESP32-C3 SuperMini**.

Der getestete Datenpfad lautet:

```text
OpenCCU / RaspberryMatic
        |
        | HB-RF-ETH / UDP 3008
        v
ESP32-C3 Gateway
        |
        | UART 115200 8N1
        v
HM-MOD-RPI-PCB Firmware 2.8.6
        |
        +--> HomeMatic-RF / BidCos-RF
        +--> HomeMatic IP
```

Der Aufbau wurde praktisch mit OpenCCU getestet. Ein BidCos-Heizkörperthermostat ließ sich nach der
Stabilisierung wieder bedienen. Der Gateway-Verkehr lief bidirektional mit **CRC-Fehler 0** und
**Drops 0**.

## Empfohlene Firmware

Die aktuell praktisch getestete Version liegt hier:

```text
firmware/stable/HM_MOD_ESP32C3_Gateway_v3_1_1.ino
```

Die älteren Entwicklungsstände sind nur zur Dokumentation unter `firmware/archive/` enthalten.

## Wichtigste Eckdaten

- ESP32-C3 UART RX: **GPIO6**
- ESP32-C3 UART TX: **GPIO5**
- ESP32-C3 RESET für HM-MOD: **GPIO7**
- UART: **115200 8N1**
- HB-RF-ETH: **UDP Port 3008**
- HM-MOD-RPI-PCB Firmware: **2.8.6**
- Beispiel-Gateway-IP: **192.168.1.50**
- Beispiel-OpenCCU-Client: **192.168.1.10**
- feste DHCP-Zuordnung für den ESP ist empfohlen

Die IP-Adressen sind Beispiele aus dem getesteten Netz und können im eigenen Netz anders sein.

## Archive-Inhalt

- `firmware/stable/` – getesteter aktueller Sketch
- `firmware/archive/` – ältere Entwicklungsstände v1 bis v3.1.1
- `docs/` – komplette Inbetriebnahme, Verdrahtung und Fehlerbehebung
- `screenshots/` – Screenshots vom erfolgreichen Betrieb
- `PROJECT_MANIFEST.json` – Dateiliste, Versionen und SHA256-Prüfsummen

## Bekannte Besonderheiten

1. In einigen v3.1/v3.1.1-WebUI-Seiten steht im Titel weiterhin `v3.1`. Der getestete Hotfix ist
   trotzdem v3.1.1.
2. Die im Gateway passiv dekodierte **HmIP-Adresse kann falsch angezeigt werden**. Für die korrekte
   HmIP-Adresse ist die OpenCCU-Hilfeseite maßgeblich.
3. Die Buttons `HM-Modul Reset` und `Modulinfo neu lesen` sollten im normalen Betrieb nicht benutzt
   werden, solange OpenCCU aktiv arbeitet.
4. Der Webserver besitzt derzeit keine Anmeldung. Das Gateway daher nur in einem vertrauenswürdigen
   LAN betreiben und **nicht direkt ins Internet freigeben**.
5. Die offizielle eQ-3-Firmwaredatei ist aus Lizenz-/Distributionsgründen nicht in diesem Archiv
   enthalten. Download und Update sind in `docs/05_HM_MOD_FIRMWARE_2_8_6.md` beschrieben.
