# Fehlerbehebung

## 1. `0 verbunden / 0 gestartet`

Bedeutung: OpenCCU ist nicht aktuell mit dem Gateway verbunden.

Prüfen:

- OpenCCU läuft?
- Gateway-IP in OpenCCU korrekt?
- ESP erreichbar?
- UDP 3008 nicht blockiert?
- System-Log öffnen.

## 2. `1 verbunden / 0 gestartet`

Bedeutung: HB-RF-ETH-Verbindung steht, aber Raw-UART wurde noch nicht vollständig mit `STARTCONN`
aktiviert.

Während eines OpenCCU-Starts kann dieser Zustand kurz normal sein.

## 3. Reconnect-Sturm mit wechselnden UDP-Ports

Hotfix v3.1.1:

- `WiFi.setSleep(false)`
- CONNECT-Antwort priorisiert
- neue Source-Ports derselben IP als Rebind behandeln
- `WiFi.reconnect()` statt aktivem Disconnect
- UDP-Bearbeitung vor WLAN-Wartung

## 4. Viele `UNREACH`-Meldungen

Nach Wiederherstellung:

- Gateway auf `1 verbunden / 1 gestartet` prüfen
- Traffic in beide Richtungen prüfen
- ein Gerät gezielt bedienen
- bei Batteriegeräten auf nächsten regulären Funkkontakt warten

## 5. Modul bleibt bei `Co_CPU_BL`

Direkt nach Reset kann `Co_CPU_BL` normal sein.

Wenn OpenCCU korrekt verbunden ist, sollte die Initialisierung das Modul in `DualCoPro_App`
überführen.

## 6. Firmwareanzeige `unknown`

Bei Bootloader-Zustand kann die lokale Anzeige `unknown` sein. Die OpenCCU-Hardwareinfo ist
maßgeblich.

## 7. OpenCCU meldet kein Funkmodul

Prüfen:

- Gateway 1/1?
- Traffic?
- Firmware 2.8.6?
- `/dev/raw-uart (HB-RF-ETH@<gateway-ip>)` in OpenCCU-Hilfe vorhanden?
- ESP-Systemlog ansehen.

## 8. Buttons im Betrieb

Solange OpenCCU aktiv arbeitet, möglichst nicht verwenden:

```text
Modulinfo neu lesen
HM-Modul Reset
```

Diese Funktionen greifen direkt auf den Funkmodul-UART zu und können die laufende OpenCCU-Sitzung
stören.
