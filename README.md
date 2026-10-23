# Artificial_Pancreas
ESP32-based artificial pancreas bench prototype: closed-loop insulin dosing, meal-triggered enzyme pump, and a web page for glucose dose calculation (patient and doctor). Dexathon 2026. Educational only, not a medical device.

**Team:** Moorthy & Gang | **Event:** Dexathon 2026 | **Team ID:** DEX26319 | **Category:** Hardware + Software

A low-cost prototype that imitates both functions of the pancreas:

| Function | What the prototype does |
|---|---|
| **Endocrine** | Reads glucose, predicts the next 30 minutes, and doses insulin in small steps with a stepper-driven syringe pump (closed loop). |
| **Exocrine** | When a meal with fat is announced, a peristaltic pump releases a metered amount of digestive-enzyme fluid. |
| **Web page** | Dose calculator for the patient, settings and log for the doctor, and a live view of the device. |

> **Safety notice.** This is an educational, hackathon-level prototype and **not a medical device**.
> Use it on the bench with coloured water only. Never connect it to a person and never use real insulin.

---

## Repository layout

```
artificial-pancreas-prototype/
├── README.md
├── LICENSE.txt
├── .gitignore
├── firmware/
│   └── artificial_pancreas/
│       └── artificial_pancreas.ino     # ESP32 firmware
├── web/
│   └── index.html                      # Patient / Doctor / Live device page (no dependencies)
└── docs/
    └── Wiring_Guide.docx               # Parts list, pin map, wiring steps, calibration
```

## Hardware (low-cost parts)

- ESP32 DevKit v1
- ADS1115 16-bit ADC module (real-sensor mode)
- Glucose sensing front end: enzyme (glucose oxidase) electrode strip + potentiostat/transimpedance amplifier (see wiring guide)
- NEMA 17 stepper + A4988 driver + lead-screw syringe pump (insulin side)
- 12 V peristaltic pump + logic-level MOSFET (IRLZ44N) + flyback diode (enzyme side)
- Emergency-stop push button, active buzzer, 12 V 2 A supply, 100 uF capacitor

Full pin map and wiring steps are in `docs/Wiring_Guide.docx`.

## Quick start

### 1. Firmware
1. Install the Arduino IDE and the **ESP32** board package.
2. Install libraries: **Adafruit ADS1X15** and **AccelStepper**.
3. Open `firmware/artificial_pancreas/artificial_pancreas.ino`.
4. Set `WIFI_SSID` and `WIFI_PASS`. If Wi-Fi fails, the board opens a hotspot named `ArtificialPancreas` (password `12345678`, IP `192.168.4.1`).
5. Keep `#define SIM_MODE true` for the first run. The simulator generates glucose values, so you can demo the full loop without a real sensor.
6. Upload, open Serial Monitor at 115200 baud, and note the IP address.

### 2. Web page
- Open `web/index.html` directly in a browser (double-click) or serve it locally.
- **Patient** tab: enter glucose, carbs and fat to get the suggested insulin and enzyme dose.
- **Doctor** tab: set target glucose, ISF and ICR, review and export the log.
- **Live device** tab: enter the ESP32 IP and press *Connect*.

> A page served over HTTPS (for example GitHub Pages) cannot call the device over plain HTTP. The calculator and log still work there; for live control open the file locally.

## How it works

```
 glucose sensor --> ESP32 --> predict (30 min) --> safety checks --> insulin syringe pump
                      |
 meal (carbs, fat) ---+--> meal bolus + correction --> insulin pump
                      +--> fat grams --> enzyme peristaltic pump
```

Formulas used (also shown in the web page):

- Meal insulin = carbs / ICR
- Correction insulin = (glucose - target) / ISF
- Dose = meal + correction - insulin on board (IOB)

## Built-in safety features

- Low-glucose suspend: stops insulin when glucose < 70 mg/dL or predicted < 80 mg/dL in 30 minutes
- Automatic correction only when glucose is high and not falling, capped at 0.5 U per cycle
- Meal bolus cap (5 U), daily cap (20 U), enzyme dose cap (5 ml)
- Hardware emergency-stop button (latches until reset)
- Buzzer and LED alerts

## Limitations and honest notes

- Real continuous glucose sensing needs a calibrated, biocompatible sensor. The DIY electrode is for bench demonstration only.
- Exocrine replacement in patients is normally taken as oral enzyme capsules. The pump emulates the timing and proportional dosing as a concept demonstration.
- No clinical validation has been done. Treat all numbers as demonstration values.

## Roadmap

- Replace the simulator with a commercial CGM data feed
- Add model-predictive control and test it against the UVA/Padova simulator
- Cloud sync for doctor review, with proper authentication
- Flash-based storage of settings and dose history

## License

MIT - see `LICENSE.txt`.
