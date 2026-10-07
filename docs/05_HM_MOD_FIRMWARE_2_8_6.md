# HM-MOD-RPI-PCB Firmware 2.8.6

## Warum 2.8.6?

Das HM-MOD-RPI-PCB lief zu Beginn mit Firmware **1.4.1**. OpenCCU versuchte ein automatisches Update
auf **2.8.6**, dieser Weg schlug im Test jedoch fehl.

Der ESP32-Gateway-Updater wurde deshalb verwendet, um die offizielle eQ-3-Datei direkt über UART auf
das fest angeschlossene Funkmodul zu schreiben.

Der erfolgreiche OpenCCU-Status bestätigte danach:

```text
HM-MOD-RPI-PCB (2.8.6)
```

für sowohl HmRF als auch HmIP.

## Offizielle Firmwaredatei

Dateiname:

```text
dualcopro_si1002_update_blhm.eq3
```

Offizielle Quelle:

```text
https://raw.githubusercontent.com/eq-3/occu/master/firmware/HM-MOD-UART/dualcopro_si1002_update_blhm.eq3
```

Zum Zeitpunkt des Tests:

```text
Dateigröße: 98944 Byte
Blöcke:     48
```

## Update über Gateway-WebUI

1. OpenCCU/RaspberryMatic stoppen bzw. herunterfahren.
2. Gateway eingeschaltet lassen.
3. Browser: `http://<gateway-ip>/firmware`
4. `.eq3`-Datei hochladen.
5. Prüfen, dass die Datei validiert wird.
6. Erst dann Firmwareflash starten.
7. Versorgung während des Schreibens nicht absichtlich unterbrechen.
8. Nach Abschluss ESP neu starten.
9. OpenCCU wieder starten.
10. In OpenCCU unter Hilfe/Hardware prüfen, ob `2.8.6` angezeigt wird.

## Achtung

Die Firmware **nicht erneut flashen**, solange das Modul bereits korrekt mit 2.8.6 arbeitet.

Historischer Hinweis: Die erste Updater-Version meldete nach erfolgreichem Schreiben fälschlich,
`Co_CPU_App startet aber nicht`, weil sie den neuen Zustand `DualCoPro_App` noch nicht korrekt
berücksichtigte. OpenCCU bestätigte anschließend trotzdem eindeutig die erfolgreiche Firmware 2.8.6.
