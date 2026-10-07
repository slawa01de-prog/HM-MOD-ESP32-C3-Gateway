# HM-MOD ESP32-C3 Gateway v3.1.1

Hotfix für den beobachteten HB-RF-ETH-Reconnect-Sturm.

## Änderungen gegenüber v3.1

- WiFi Power Save deaktiviert (`WiFi.setSleep(false)`), weil `hb_rf_eth.ko`
  auf die CONNECT-Antwort nur ungefähr 50 ms wartet.
- CONNECT-Pakete werden priorisiert und die Antwort wird vor dem Logging gesendet.
- Neue UDP-Source-Ports derselben OpenCCU-IP werden als Rebind behandelt,
  nicht als Disconnect/Connect.
- WLAN-Reconnect verwendet `WiFi.reconnect()` statt die Station aktiv mit
  `WiFi.disconnect()` abzureißen.
- HB-RF-ETH UDP wird im Main Loop vor WLAN-Wartung bearbeitet.
- Alle v3.1-Funktionen bleiben erhalten: DualCoPro 2.8.6, Diagnose, Log,
  HM-Firmware-Updater, ESP-OTA, Einstellungen.

## Erwarteter erfolgreicher Start

```text
HB CONNECT: <openccu-ip>
HB STARTCONN: <openccu-ip>
```

Danach sollten keine fortlaufend wechselnden UDP-Ports mehr erscheinen.
