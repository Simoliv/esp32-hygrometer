# Whats this ??

A selfmade hygrometer with ESP32 which measures the humidity in our walls in the house. 

I use it, to measure the humidity in walls in our house. In a few days, we will put a creme into the walls, 
to get them dry again, and I want to see progress to know, if it was worth buying and doing it.

Also, to get an idea, if the weather has any influence on the humidity of our walls.

I use 2 RJ45 CAT7 Network cables to connect to the screws in the wall.

## What it does

- connects via Wifi 
- serves its values via prometheus endpoint on /metrics
- sends its values via MQTT to configurable endpoint
- uses a SHT31 Sensor (if available)
- prints out °C and % humidity + all 8 sensors/screws on a LCD Display (currently on 5V)
- has a website to configure it 

Read [MEASUREMENTS.md](MEASUREMENTS.md) (currently only in german, if interested, use a translator or tell me)

## Todos:

- accesspoint mode as long as no Wifi is configured
- reset button in webinterface
- smaller display
- buy 3d printer and give it a nice case :(
- put photos of setup (once its nice enough)

## Wiring Diagram: ESP32 DevKit V1 & 74HC4051

### 1. Power Supply

- V5 / VIN (on ESP32): Not required if USB-C is connected.
- 3V3 (ESP32) → VCC (Mux Pin 16)
- GND (ESP32) → GND (Mux Pin 8) AND VEE (Mux Pin 7)
- GND (ESP32) → EN (Mux Pin 6 / Enable) — Must be connected to GND for the Mux to operate.

### 2. Address Control (Which channel is being measured?)

- GPIO 25 (ESP32) → S0 / A (Mux Pin 11)
- GPIO 26 (ESP32) → S1 / B (Mux Pin 10)
- GPIO 27 (ESP32) → S2 / C (Mux Pin 9)

### 3. Measurement Path (Analog)

Here, the wall resistance is translated into voltage:

- Z / SIG (Mux Pin 3) → GPIO 34 (ESP32)
- 100kΩ Resistor: One leg to GPIO 34, the other to GND.

### 4. Wall Sensors (Screws)

- Screw A (at each measurement point): All are connected directly to 3V3 on the ESP32.
- Screw B (Measurement Point 1) → Y0 (Mux Pin 13)
- Screw B (Measurement Point 2) → Y1 (Mux Pin 14)
- ... (Y2=Pin 15, Y3=Pin 12, Y4=Pin 1, Y5=Pin 5, Y6=Pin 2, Y7=Pin 4)

### 5. Schematic Representation Attempt

grey: GND

red: Power (3,3V or 5V)

orange: channel control

blue: backchannel measurement 

green/violet: I2C controls

```mermaid
---
config:
  flowchart:
    curve: basis
---

flowchart LR
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

  RI([Resistor/Widerstand 100kOhm])
  
  %% link 17
  SHT-33V --- 33V 
  SHT-GND --- GND 
  SHT-SDA --- GPIO21
  SHT-SCL --- GPIO22 

  %% link 21
  DI-5V --- 5V
  DI-GND --- GND
  DI-SDA --- GPIO21
  DI-SCL --- GPIO22

  %% link 25
  sN1 --- 33V
  sN2 --- MYN

  subgraph esp32 [ESP32 DevKit]
    direction LR
    subgraph l[.]
      direction TB
      5V
      33V
      GND
    end
    subgraph r[.]
      direction TB
      GPIO21
      GPIO22
      GPIO25
      GPIO26
      GPIO27
      GPIO34  
    end
  end

  subgraph SHT31[SHT31 sensor]
    SHT-33V[3.3V]
    SHT-GND[GND]
    SHT-SDA[SDA]
    SHT-SCL[SCL]
  end

  subgraph display[Display]
    DI-5V[VCC 5V]
    DI-GND[GND]
    DI-SDA[SDA]
    DI-SCL[SCL]
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
      MYN[Yn...]
    end
  end

  subgraph sensors[Screws/Schrauben/Sensors]
    direction LR
    s11 -. Sensor 1 .- s12
    s21 -. Sensor 2 .- s22
    s31 -. Sensor 3 .- s32
    sN1 -. Sensor N .- sN2
  end

  linkStyle 0,7,9,11,17,21,25 stroke:#FF0000,stroke-width:2px;
  linkStyle 8,10,12 stroke:#00F,stroke-width:2px;
  linkStyle 6,13,14,15,16,18,22 stroke:#b9bec0,stroke-width:2px;
  linkStyle 1,2,3 stroke:#f7ad00,stroke-width:2px;
  linkStyle 19,23 stroke:#32a852,stroke-width:2px;
  linkStyle 20,24 stroke:#7532a8,stroke-width:2px;

  style sensors fill:#fff,stroke:#4287f5

```

## List of materials:

- ESP32 [amazon.de/dp/B0DGG7LXMF?psc=1&ref=ppx_pop_dt_b_product_details](amazon.de/dp/B0DGG7LXMF?psc=1&ref=ppx_pop_dt_b_product_details)
- Multiplexer [https://www.amazon.de/dp/B09Z29W8XV](https://www.amazon.de/dp/B09Z29W8XV)
- SHT31 [https://www.amazon.de/dp/B01GQFUY0I](https://www.amazon.de/dp/B01GQFUY0I)
- Display [https://www.amazon.de/dp/B07CQG6CMT?s=bazaar](https://www.amazon.de/dp/B07CQG6CMT?s=bazaar)
- RJ45 Breakout Board [https://www.amazon.de/dp/B0CML41H78](https://www.amazon.de/dp/B0CML41H78)
- 16 V2A Edelstahlschrauben Schlüsselschrauben M6x60 + M6 Unterlegscheiben von toom 
- CAT7 Netzwerkkabel 10m (cut in two for 8 Messpunkte)
