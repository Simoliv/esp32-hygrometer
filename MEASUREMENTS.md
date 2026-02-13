# Interpretation der Messwerte

Diese Dokumentation erklärt die Bedeutung der Prometheus-Metriken, die das Hygrometer liefert, und wie sie im Kontext der Mauertrocknung (Horizontalsperre) zu interpretieren sind.

## 1. Raumklima (SHT31 Sensor)

| Metrik | Einheit | Beschreibung |
| :--- | :--- | :--- |
| `hygrometer_ambient_temperature_celsius` | °C | Die Lufttemperatur direkt am Gerät. |
| `hygrometer_ambient_humidity_percent` | % | Die relative Luftfeuchtigkeit im Raum. |

**Interpretation:**
Diese Werte sind wichtig, um externe Einflüsse auszuschließen. Wenn die Wandfeuchte steigt, aber gleichzeitig die Raumluftfeuchte extrem hoch ist, könnte es sich um Kondensation handeln.

## 2. Mauermessung (Widerstand & Index)

Das Gerät misst den elektrischen Widerstand zwischen zwei Edelstahlschrauben in der Wand.

| Metrik | Einheit | Beschreibung |
| :--- | :--- | :--- |
| `hygrometer_resistance_ohms` | Ω (Ohm) | **Die physikalische Wahrheit.** Der rohe elektrische Widerstand. |
| `hygrometer_index_percent` | % | Die berechnete Feuchtigkeit (basiert auf den Referenzwerten). |

### Interpretation des Widerstands (`resistance_ohms`):
*   **Hoher Widerstand (> 2 MΩ / 2.000.000 Ω):** Die Wand ist trocken. Es sind kaum freie Wassermoleküle vorhanden, die Strom leiten könnten.
*   **Niedriger Widerstand (< 100 kΩ / 100.000 Ω):** Die Wand ist feucht oder nass. Wasser und gelöste Salze leiten den Strom gut.
*   **Abwärtstrend:** Wenn der Widerstand über die Wochen **steigt**, ist das ein direktes Zeichen für den Erfolg der Horizontalsperre (die Wand trocknet aus).

## 3. Dynamische Referenzwerte (Kalibrierung)

Da sich Materialeigenschaften und Temperatur ändern, nutzt das System Referenzpunkte in der Wand (einen "trockenen" und optional einen "nassen" Punkt).

| Metrik | Einheit | Beschreibung |
| :--- | :--- | :--- |
| `hygrometer_effective_dry_ohms` | Ω | Der Widerstandswert, der aktuell als **0% (trocken)** definiert ist. |
| `hygrometer_effective_wet_ohms` | Ω | Der Widerstandswert, der aktuell als **100% (nass)** definiert ist. |

**Warum diese Werte wichtig sind:**
Der `hygrometer_index_percent` wird wie folgt berechnet:
$$Index = 100 \times \frac{\ln(Dry\_Ohms) - \ln(Measured\_Ohms)}{\ln(Dry\_Ohms) - \ln(Wet\_Ohms)}$$

Wenn du in Prometheus siehst, dass der Prozentwert springt, prüfe zuerst, ob sich die `effective_dry_ohms` oder `effective_wet_ohms` geändert haben (z.B. durch Umstellen der Referenzkanäle im Webinterface).

## 4. Analyse der Horizontalsperre

Um die Wirksamkeit der Injektionscreme zu beurteilen, solltest du in Grafana/Prometheus auf folgende Zeichen achten:

1.  **Dauerhafter Widerstandsanstieg:** Sinken die Prozentwerte (Index) stetig ab, während der Widerstand (`resistance_ohms`) steigt? -> **Erfolg.**
2.  **Referenz-Check:** Vergleiche die Messkanäle mit dem Referenzkanal (`dry_ch`). Nähert sich der Messkanal dem Widerstandswert des Referenzkanals an? -> **Ziel erreicht.**
3.  **Wetter-Korrektur:** Schwankt der Widerstand synchron zur relativen Luftfeuchte im Raum, aber der Trend geht nach oben? -> **Normales Verhalten**, die Injektion wirkt, aber die Oberfläche reagiert auf die Luft.

## 5. Konfiguration (Metadaten)

| Metrik | Wert | Beschreibung |
| :--- | :--- | :--- |
| `hygrometer_config_reference_dry_channel` | ID | Welcher Hardware-Kanal (0-7) aktuell die Trocken-Referenz stellt. |
| `hygrometer_config_reference_wet_channel` | ID | Welcher Hardware-Kanal (0-7) aktuell die Nass-Referenz stellt. |
