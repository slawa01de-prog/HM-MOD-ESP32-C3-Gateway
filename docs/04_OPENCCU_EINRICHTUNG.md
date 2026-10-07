# OpenCCU / RaspberryMatic einrichten

## Gateway-IP eintragen

In OpenCCU/RaspberryMatic:

```text
Systemsteuerung
-> Erweiterte Einstellungen
-> IP-Adresse (HB-RF-ETH)
```

Dort die IP des ESP32-Gateways eintragen, zum Beispiel:

```text
192.168.1.50
```

Danach OpenCCU neu starten.

## Erwarteter Gateway-Status

Auf dem Gateway-Dashboard sollte nach der Initialisierung stehen:

```text
OpenCCU / HB-RF-ETH
1 verbunden / 1 gestartet
```

Außerdem müssen beide Traffic-Zähler steigen:

```text
Host -> HM
HM -> Host
```

und:

```text
CRC-Fehler: 0
Drops:      0
```

## Erfolgreich getestete OpenCCU-Hardwareanzeige

OpenCCU erkannte das Modul als:

```text
HomeMaticIP-RF:
HM-MOD-RPI-PCB (2.8.6)
Device-Node: /dev/raw-uart (HB-RF-ETH@192.168.1.50)

HomeMatic-RF:
HM-MOD-RPI-PCB (2.8.6)
Device-Node: /dev/raw-uart (HB-RF-ETH@192.168.1.50)
```

## Praxistest

Ein vorhandenes BidCos-RF-Gerät bedienen, zum Beispiel einen Heizkörperthermostat-Sollwert ändern.

Erfolgsmerkmale:

- `Host -> HM` steigt
- `HM -> Host` steigt
- Gerät reagiert
- `UNREACH` verschwindet nach erfolgreichem Funkkontakt

## Hinweis zu Servicemeldungen

Nach Gateway-, OpenCCU- oder Funkmodul-Neustarts können viele Geräte vorübergehend als
`Gerätekommunikation gestört` bzw. `UNREACH` erscheinen.

Batteriegeräte melden sich teilweise erst beim nächsten regulären Funkkontakt wieder.
