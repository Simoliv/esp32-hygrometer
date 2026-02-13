# Schaltplan: ESP32 DevKit V1 & 74HC4051

## 1. Stromversorgung (Power)

- V5 / VIN (am ESP32): Nicht benötigt, wenn USB-C angeschlossen ist.
- 3V3 (ESP32) → VCC (Mux Pin 16)
- GND (ESP32) → GND (Mux Pin 8) UND VEE (Mux Pin 7)
- GND (ESP32) → EN (Mux Pin 6 / Enable) — Muss auf GND liegen, damit der Mux arbeitet.

## 2. Adress-Steuerung (Welcher Kanal wird gemessen?)

- GPIO 25 (ESP32) → S0 / A (Mux Pin 11)
- GPIO 26 (ESP32) → S1 / B (Mux Pin 10)
- GPIO 27 (ESP32) → S2 / C (Mux Pin 9)

## 3. Mess-Pfad (Analog)

Hier wird der Widerstand der Wand in Spannung übersetzt:

- Z / SIG (Mux Pin 3) → GPIO 34 (ESP32)
- 100kΩ Widerstand: Ein Bein an GPIO 34, das andere Bein an GND.

## 4. Die Wand-Sensoren (Schrauben)

- Schraube A (an jeder Messstelle): Alle werden direkt mit 3V3 am ESP32 verbunden.
- Schraube B (Messstelle 1) → Y0 (Mux Pin 13)
- Schraube B (Messstelle 2) → Y1 (Mux Pin 14)
- ... (Y2=Pin 15, Y3=Pin 12, Y4=Pin 1, Y5=Pin 5, Y6=Pin 2, Y7=Pin 4)

## 5. Versuch einer schematischen Darstellung

```mermaid
---
config:
  flowchart:
    curve: basis
---

flowchart TB
  33V --- MPIN16

  GPIO25 --- MPIN11
  GPIO26 --- MPIN10
  GPIO27 --- MPIN9

  MPIN3 --- GPIO34

  RI --- GPIO34
  RI --- GND

  s11 --- 33V
  s12 --- MY0
  s21 --- 33V
  s22 --- MY1
  s31 --- 33V
  s32 --- MY2
 
  GND --- MGND
  GND --- MPIN7
  GND --- MGND
  GND --- MPIN6

  RI([Widerstand 100kOhm])

  subgraph esp32 [ESP32 DevKit]
    direction LR
    subgraph l[.]
      direction TB
      33V
      GND
    end
    subgraph r[.]
      direction TB
      GPIO25
      GPIO26
      GPIO27
      GPIO34
    end
  end

  subgraph mux [74HC4051 Multiplexer]
    direction LR
    subgraph muxl [Control/Input]
      direction TB
      MPIN3[Z/SIG]
      MPIN9[S0]
      MPIN10[S1]
      MPIN11[S2]
      MPIN6[E]
    end

    subgraph muxr [Power/Outputs]
      direction TB
      MPIN16[VCC]
      MPIN7[VEE]    
      MGND[GND]
      MY0[Y0]
      MY1[Y1]
      MY2[Y2]
    end
  end

  subgraph sensors[Schrauben/Sensoren]
    direction LR
    s11 -. Sensor 1 .- s12
    s21 -. Sensor 2 .- s22
    s31 -. Sensor 3 .- s32
  end

  linkStyle 0,7,9,11 stroke:#FF0000,stroke-width:2px;
  linkStyle 8,10,12 stroke:#00F,stroke-width:2px;
  linkStyle 6,13,14,15,16 stroke:#b9bec0,stroke-width:2px;
  linkStyle 1,2,3 stroke:#f7ad00,stroke-width:2px;

  style sensors fill:#fff,stroke:#4287f5

```