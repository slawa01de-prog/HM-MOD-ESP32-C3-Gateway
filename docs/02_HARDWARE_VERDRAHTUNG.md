# Hardware und Verdrahtung

## Verwendete Hardware

- ESP32-C3 SuperMini
- eQ-3 HM-MOD-RPI-PCB
- stabile 3,3-V-Versorgung
- gemeinsame Masse
- WLAN-Verbindung zum OpenCCU/RaspberryMatic-Netz

## Geprüfte Verdrahtung

```text
HM-MOD-RPI-PCB                 ESP32-C3 SuperMini
-------------------------------------------------
Pin 1   3,3 V              ->  3V3
GND                       ->  GND

Pin 8   HM_RX              <-  GPIO5  (ESP TX)
Pin 10  HM_TX              ->  GPIO6  (ESP RX)
Pin 12  HM_RESET           <-  GPIO7
```

### Wichtig

UART muss **gekreuzt** angeschlossen werden:

```text
ESP TX GPIO5  -> HM RX Pin 8
ESP RX GPIO6  <- HM TX Pin 10
```

Pin 12 ist der Reset-Eingang des Funkmoduls. Der Reset ist aktiv LOW.

## UART-Parameter

```text
Baudrate: 115200
Datenbits: 8
Parität:   keine
Stopbits:  1
```

## Nicht verwenden

- Pin 11 ist Debug/C2D und wird für diesen Aufbau nicht benötigt.
- Das HM-MOD-RPI-PCB wird nicht mit 5 V an Pin 1 betrieben.
- Keine Verbindung zu Netzspannung herstellen; der komplette Gateway-Aufbau arbeitet nur im
  Kleinspannungsbereich.
