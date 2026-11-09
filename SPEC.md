# LoxPanel P4 – Spezifikation v0.1

Eigene Touch-Bedienoberfläche für Loxone auf Elecrow CrowPanel Advanced ESP32-P4.
Ziel: frei konfigurierbare Seiten und Buttons, Konfiguration per Vorlage klonbar auf mehrere Displays.

Arbeitsname «LoxPanel P4» – kann jederzeit geändert werden.

---

## 1. Rahmenbedingungen

| Punkt | Festlegung |
|---|---|
| SoC | ESP32-P4 (RISC-V), WLAN/BT über ESP32-C6 (esp_hosted) |
| Framework | ESP-IDF ≥ 5.4, kein PlatformIO (P4 dort nicht offiziell unterstützt) |
| Grafik | LVGL 9.x über `esp_lvgl_port` |
| Sprache | C (IDF-Komponenten), C++ wo sinnvoll |
| Miniserver | Gen 1, Gen 2 und Compact – alle drei müssen funktionieren |
| UI-Sprache | Deutsch (Texte zentral, später übersetzbar) |
| Lizenz | Eigenes Projekt, kein Code aus MiKa-Lox-Control |

## 2. Hardware-Profile (Boards)

Die Firmware wird pro Board gebaut (Kconfig-Auswahl `LOXPANEL_BOARD_*`). Ein Profil kapselt alles Hardwarespezifische:

- Panel-Treiber und Init-Sequenz (MIPI-DSI), Auflösung, Farbformat
- Touch-Controller und Pins
- Hintergrundbeleuchtung (PWM-Pin, Min/Max)
- Optional: Audio, SD-Karte, Helligkeitssensor

Start-Profile:

| Profil | Auflösung | Quelle der Init-Daten |
|---|---|---|
| `crowpanel_p4_5in` | 800×480 | Elecrow-Beispiel 5" |
| `crowpanel_p4_7in` | 1024×600 | Elecrow-Beispiel 7" |
| `crowpanel_p4_9in` | 1024×600 | Elecrow-Beispiel 9" |
| `crowpanel_p4_10in1` | 1024×600 | Elecrow-Beispiel 10.1" |

Die Init-Sequenzen, Pins und Touch-Controller **aus den offiziellen Elecrow-Repos übernehmen und auf dem echten Board prüfen**, nicht raten.

Neues Display = neues Profil in `components/boards/`, sonst keine Änderung.

## 3. Auflösungsunabhängiges Layout

- Jede Seite ist ein Raster mit `spalten × zeilen` Kacheln (Standard: aus Auflösung berechnet, z. B. 4×3 bei 800×480, 5×3 bei 1024×600).
- Kacheln können mehrere Zellen belegen (`breite`, `hoehe`).
- Schriftgrössen und Abstände skalieren über einen Faktor aus der Displayhöhe.
- In der Konfiguration optional `raster` pro Seite überschreibbar.
- Kopfzeile (Uhrzeit, Raum, Verbindungsstatus) und Seitennavigation (Wischen + Punkte/Tabs) sind fest.

## 4. Loxone-Anbindung

### 4.1 Verbindung
- WebSocket `ws://<host>/ws/rfc6455`, Unterprotokoll `remotecontrol`.
- Optional TLS (`wss`), wo der Miniserver es lokal anbietet; sonst unverschlüsselte Verbindung mit verschlüsselter Authentifizierung.
- Keepalive regelmässig senden, Reconnect mit Backoff, Anzeige des Status in der Kopfzeile.
- Fokus auf stabile Verbindung in Mesh-WLANs (Roaming → sauberer Reconnect ohne Neustart).

### 4.2 Authentifizierung
- Token-basiert: RSA-Public-Key holen, Session-Key per RSA austauschen, AES für verschlüsselte Befehle, HMAC für Passwort-Hash, Token holen und speichern.
- Token im NVS verschlüsselt ablegen, rechtzeitig erneuern, Passwort danach nicht mehr nötig.
- Implementierung mit mbedTLS.
- **Gen 1 / Gen 2 / Compact**: Unterschiede in Firmware-Version und Hash-Algorithmus (SHA1 vs. SHA256) aus der Antwort des Miniservers erkennen, nicht anhand der Generation hartcodieren.
- Referenz: offizielle Loxone-Dokumentation «Communicating with the Miniserver» und «Structure File».

### 4.3 Struktur und Status
- `LoxAPP3.json` laden, im PSRAM halten, auf SD/Flash cachen; über `LoxAPPversion3` prüfen, ob neu laden nötig.
- Daraus: Räume, Kategorien, Bausteine (UUID, Typ, Name, Raum, States).
- Binäre Status-Updates aktivieren; Value-States (UUID + double) und Text-States verarbeiten und an gebundene Widgets verteilen.
- Befehle: `jdev/sps/io/<uuid>/<befehl>`.

### 4.4 Unterstützte Bausteine – Version 1
| Loxone-Typ | Widget |
|---|---|
| Switch, Pushbutton | Schalter / Taster |
| Dimmer | Schieberegler + Ein/Aus |
| LightControllerV2 | Stimmungen wählen, Ein/Aus |
| Jalousie | Auf/Ab/Stopp, Position |
| IRoomControllerV2 | Ist/Soll-Temperatur, Modus, ±0,5 °C |
| Meter | Aktueller Wert + Zähler mit Einheit aus Format |
| InfoOnlyAnalog / InfoOnlyDigital / TextState | Anzeige |

Weitere Typen (Tor, Audio, Alarm, Intercom) in späteren Versionen.

## 5. Konfiguration

Die gesamte Display-Konfiguration ist **eine JSON-Datei** (`/config/panel.json`), getrennt von Geräte-Einstellungen (WLAN, Miniserver, Token), die nie exportiert werden.

### 5.1 Format

```json
{
  "schema": 1,
  "vorlage": "Wohnung-Standard",
  "vorlage_version": 3,
  "variablen": { "raum": "W12a Wohnen" },
  "anzeige": { "helligkeit": 80, "dimmen_nach_s": 60, "startseite": 0 },
  "seiten": [
    {
      "titel": "Licht",
      "raster": [5, 3],
      "kacheln": [
        { "typ": "auto", "ziel": "{raum}/Licht Decke", "breite": 2 },
        { "typ": "jalousie", "ziel": "{raum}/Beschattung" },
        { "typ": "auto", "ziel": "uuid:0f1e2d3c-...", "titel": "Aussenlicht" }
      ]
    }
  ],
  "abweichungen": {
    "Licht/2": { "ausblenden": true }
  }
}
```

### 5.2 Bindung von Kacheln (`ziel`)
- `Raum/Name` – Auflösung über die Strukturdatei (bevorzugt, klonbar).
- `{variable}` – Platzhalter, wird pro Display ersetzt.
- `uuid:<uuid>` – feste Bindung, für Sonderfälle.
- `typ: "auto"` – Widget aus dem Loxone-Typ ableiten.
- Nicht auflösbar → Kachel «nicht zugeordnet», antippen öffnet Auswahl aus der Struktur.
- Mehrdeutig (gleicher Name zweimal im Raum) → ebenfalls Auswahl anbieten.

### 5.3 Klonen und Vorlagen
Stufe 1 – **Export/Import** über die Weboberfläche (JSON herunterladen / hochladen).
Stufe 2 – **Klonen im Netz**: Displays melden sich per mDNS (`_loxpanel._tcp`); ein Display kann die Konfiguration eines anderen übernehmen. Variablen und `abweichungen` des Ziel-Displays bleiben auf Wunsch erhalten.
Stufe 3 – **Vorlagen verteilen**: Ändert man eine Vorlage, kann man sie an alle Displays mit gleichem `vorlage`-Namen schicken; jedes behält seine Variablen und Abweichungen. Verteilung nur nach Bestätigung, mit Anzeige, welche Displays betroffen sind.

Beim Import immer: Schema-Version prüfen, Vorschau, was sich ändert, alte Konfiguration als Backup behalten (1 Stufe Rückgängig).

## 6. Einrichtung und Weboberfläche

- Erststart: Access Point `LoxPanel-XXXX` mit Captive Portal → WLAN, Miniserver-Adresse, Benutzer, Passwort.
- Danach Weboberfläche im LAN (`http://loxpanel-xxxx.local`):
  - Status (Verbindung, Firmware, Board-Profil, freier Speicher)
  - Konfigurations-Editor: Seiten und Kacheln anlegen, Bausteine aus der Loxone-Struktur auswählen (Baum nach Raum/Kategorie)
  - Export / Import / Klonen / Vorlage verteilen
  - Firmware-Update (OTA, Datei hochladen)
- Weboberfläche mit Passwort schützen.
- Am Display selbst: Einstellungsseite hinter langem Druck auf die Kopfzeile + PIN.

## 7. Betrieb

- Displayhelligkeit dimmen und Bildschirm aus nach Zeit; Aufwecken per Touch.
- Uhrzeit vom Miniserver (zusätzlich SNTP als Fallback).
- OTA mit zwei App-Partitionen und Rollback bei Startproblemen.
- Log über serielle Schnittstelle, Log-Level per Kconfig.

## 8. Projektstruktur (Vorschlag)

```
loxpanel-p4/
├── main/                 App-Start, Zustandsautomat
├── components/
│   ├── boards/           Hardware-Profile (ein Unterordner je Board)
│   ├── loxone/           WebSocket, Auth, Struktur, Status, Befehle
│   ├── config/           JSON laden/speichern, Auflösung von Zielen, Vorlagen
│   ├── ui/               LVGL-Seiten, Raster, Widgets je Bausteintyp
│   ├── web/              HTTP-Server, Weboberfläche (statische Dateien eingebettet)
│   └── discovery/        mDNS, Klonen, Vorlagen-Verteilung
├── partitions.csv
├── sdkconfig.defaults
└── SPEC.md
```

## 9. Umsetzungsreihenfolge

1. **Board bringt Bild**: ein Profil (das Board, das auf dem Tisch liegt), LVGL Demo, Touch, Helligkeit.
2. **WLAN über C6** stabil, inkl. Reconnect.
3. **Loxone-Verbindung**: Auth mit Token, Struktur laden, Status-Updates loggen.
4. **Erste UI**: feste Testseite mit Schalter + Dimmer + Jalousie, Bedienung funktioniert.
5. **Konfiguration aus JSON** inkl. Raster, `Raum/Name`-Bindung, Platzhalter.
6. **Weboberfläche**: Einrichtung, Export/Import.
7. **Restliche Bausteine** (Raumregler, Lichtsteuerung, Meter, Info).
8. **Klonen im Netz**, dann Vorlagen verteilen.
9. **Weitere Board-Profile** und OTA.
10. Test mit Gen 1, Gen 2 und Compact.

Jeder Schritt endet mit: kompiliert, geflasht, auf dem Board geprüft.

## 10. Offene Punkte

- ~~Erstes Board für die Entwicklung~~ → CrowPanel 9" (PCB V1.0, ESP32-P4 Rev. 1.3). Profil `crowpanel_p4_9in`.
  - PCB V1.0 hat die C6-SDIO-Datenleitungen anders belegt als V1.1/V1.2 (D0=14 statt 17). Für spätere Boards Revision prüfen, ggf. eigenes Profil.
  - SDIO zum C6 nur stabil mit 1 Datenleitung @ 40 MHz (4 Bit bricht unter Last mit CRC-Fehlern ab).
  - C6 kam mit esp_hosted 2.3.0, wurde auf 2.12.3 aktualisiert (über SDIO, `CONFIG_LOXPANEL_C6_UPDATE`). Host-`esp_hosted` muss exakt dazu passen.
- ~~**DHCP**~~ → geklärt: Der UniFi-Router blockiert seit einem Neustart sämtlichen Verkehr der Original-MAC des C6 (`dc:1e:d5:7e:a1:04`) zum Gateway (kein DHCP, kein Ping, kein DNS), LAN-Verkehr geht weiter. Nachgewiesen per Mitschnitt und Testanfragen vom PC. Ursache in UniFi nicht gefunden. Lösung: eigene, lokal verwaltete MAC (`CONFIG_LOXPANEL_WIFI_MAC`, später Einstellung in der Weboberfläche). Zusätzlich fordert das Panel DHCP-Antworten jetzt als Broadcast an.
- Token im NVS verschlüsselt (4.2): NVS-Verschlüsselung braucht einen eFuse-Schlüssel (irreversibel). Vorerst unverschlüsselt, Entscheidung vor dem Produktiveinsatz.
- Firmware-Stände der Miniserver (für Auth-Varianten)
- Eigener Benutzer für die Displays in Loxone Config (empfohlen, mit eingeschränkten Rechten)
- Sollen Displays auch Sensoren liefern (z. B. Helligkeit, Präsenz per Kamera) – später
