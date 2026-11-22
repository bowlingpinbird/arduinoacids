# arduinoacids

Build a peptide with your hands, one amino acid at a time, and see it in 3D.

arduinoacids is a hands-on tool for biochemistry students to visualize protein sequences spatially. Each 3D-printed block is an amino acid, modeled from its N-terminus to its C-terminus, with a resistor inside whose value identifies the residue. Students snap blocks together into a chain and cap it with a terminal –COOH topper. An Arduino reads the chain, works out which amino acid was just added, and streams the growing sequence to a local web app. Press **Fold** and the sequence is built into a 3D peptide structure with [PeptideBuilder](https://github.com/clauswilke/PeptideBuilder) and shown in an interactive viewer.

## How it works

1. **Blocks in series.** Every amino acid block contains a resistor with a unique value. Connecting blocks puts the resistors in series, and the –COOH topper closes the circuit.
2. **Reading the chain.** An Arduino UNO R4 WiFi measures the total resistance through a switched voltage divider on `A0`. Each time the topper goes back on, the firmware compares the new total to the last one, and the jump identifies the residue that was added.
3. **Auto-ranging.** Three reference resistors (4.7 kΩ, 33 kΩ, 220 kΩ on pins D5, D6, D7) are switched in one at a time so the reading always sits in the ADC's most sensitive range as the chain gets longer. Each reading averages 400 samples, and a residue only counts after the reading holds steady for 1.5 seconds.
4. **Web app.** `app.py` finds the Arduino's USB port, listens to its serial output, and serves a live page at `http://localhost:5000` showing the sequence as it grows.
5. **Folding and viewing.** Pressing **Fold** runs `predict.py`, which uses PeptideBuilder and Biopython to build the peptide and save it as `built_peptide.pdb`. The page then renders the structure in 3D so you can rotate and explore it.

## Repository layout

| Path | What it is |
| --- | --- |
| `aaseq5/aaseq5.ino` | Current firmware: 5 amino acids (A, E, L, S, V) spaced about 10 kΩ apart for reliable identification |
| `aaseq2/aaseq2.ino` | Firmware for a 10-letter set (every other resistor from the CSV) |
| `aaseq/aaseq.ino` | Original firmware for longer chains (up to about 20 resistors) |
| `resistor code.csv` | Resistor value assigned to each of the 20 standard amino acids |
| `app.py` | Local server: Arduino serial bridge, live sequence, fold and calibration endpoints |
| `index.html` | Browser UI: live sequence, Fold and Reset buttons, calibration controls, 3D viewer |
| `predict.py` | Builds a peptide structure from a sequence with PeptideBuilder and writes a PDB file |
| `built_peptide.pdb` | Most recent generated structure |
| `autoranging.txt` | Notes on the auto-ranging approach |

## Hardware

- Arduino UNO R4 WiFi (14-bit ADC). A classic Uno also works at 10-bit resolution.
- Reference resistors: 4.7 kΩ, 33 kΩ, and 220 kΩ
- One resistor per amino acid block (values in `resistor code.csv`)
- 3D-printed amino acid blocks and a –COOH topper
- Wires

### Wiring

```
D5 --[ 4.7k ]--+
D6 --[ 33k  ]--+--- A0 node ---[ R1 + R2 + ... series chain ]--- GND
D7 --[ 220k ]--+
```

The firmware drives one reference pin HIGH at a time and leaves the others as high-impedance inputs.

### Amino acid resistor values (aaseq5)

| Amino acid | Letter | Resistance |
| --- | --- | --- |
| Alanine | A | 1.98 kΩ |
| Glutamate | E | 11.9 kΩ |
| Leucine | L | 21.9 kΩ |
| Serine | S | 33 kΩ |
| Valine | V | 43 kΩ |

## Getting started

### 1. Flash the firmware

Open `aaseq5/aaseq5.ino` in the Arduino IDE, select your board and port, and upload. Close the Serial Monitor afterwards so the web app can use the port.

### 2. Install the Python dependencies

```bash
pip install pyserial PeptideBuilder biopython
```

### 3. Run the app

```bash
python app.py
```

Then open [http://localhost:5000](http://localhost:5000) in your browser.

### 4. Calibrate (first time on a new board)

Calibration is saved to the Arduino's EEPROM, so you only need to do it once. Use the buttons on the page, or type the commands into the Serial Monitor:

1. With the topper off (loop open), run **Calibrate open circuit** (`calopen`).
2. For each range, put a known resistor in the chain, close the topper, enter its value in ohms, and run **Calibrate range N** (`cal <range> <ohms>`). Pick a resistor that falls roughly in 1–25 kΩ for range 0, 6–180 kΩ for range 1, and 40 kΩ–1 MΩ for range 2.

### 5. Build a peptide

1. Add the first amino acid block and put the topper on. The page shows the letter once the reading settles.
2. Take the topper off, add the next block, and put the topper back on.
3. Repeat for each residue.
4. Press **Fold** to build the 3D structure and view it.
5. Press **Reset** to start a new sequence.

## Serial commands

| Command | What it does |
| --- | --- |
| `fold` | Finish the sequence and print the final result |
| `r` | Reset and start a new sequence |
| `c` | Print the calibration currently in use |
| `calopen` | Record each range's open-loop reading |
| `cal <range> <ohms>` | Calibrate one range against a known resistor |
| `caldef` | Restore default calibration |
| `raw` | Print raw ADC readings and implied resistance for every range |

## What's next

- More dependable electronic components for easier, more reliable sequence identification
- Magnetic connections between blocks so building a chain feels more intuitive
- Support for more of the 20 standard amino acids
- Bringing arduinoacids into biochemistry classrooms

## Team
Rachel Hu, Ellen Schmidt, Sarah Fu
