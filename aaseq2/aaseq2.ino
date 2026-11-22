// Arduino resistor sequencer (aaseq2: 10-letter set, every other resistor from the CSV)
// Three-reference auto-ranging, series chain.
// Target board: Arduino UNO R4 WiFi (14-bit ADC). Also builds for a classic Uno (10-bit).
//
// Reads a growing SERIES resistor chain on A0 through a switched voltage divider,
// identifies each newly added resistor by the jump in total resistance, maps it to
// an amino-acid letter, and prints the running sequence to Serial after each add.
//
// Wiring (only wires + resistors + the board):
//   Each reference resistor connects a digital pin to the A0 node; the amino-acid
//   resistor chain runs from the A0 node to GND.
//
//     D5 --[ 4.7k ]--+
//     D6 --[ 33k  ]--+--- A0 node ---[ R1 + R2 + ... series chain ]--- GND
//     D7 --[ 220k ]--+
//                    |
//                   A0 (analog in)
//
//   The firmware drives exactly ONE reference pin HIGH (that range is active, sourcing
//   ~5V through its resistor) and sets the other reference pins to INPUT (Hi-Z, so they
//   draw no current and are effectively disconnected). It auto-ranges between the three
//   references so every reading stays in the ADC's sensitive band.
//
// Usage:
//   1) Add the first amino-acid resistor and put the topper on to close the loop.
//   2) The firmware waits for a steady reading, appends the letter, and prints the sequence.
//   3) Take the topper off (loop opens), add the next resistor in series with the chain,
//      and put the topper back on. The new total is compared to the last closed total,
//      so the difference identifies the new resistor.
//   4) Repeat 3 for each amino acid; type the finish command (default "fold") when done.
//   Opening the loop never changes the baseline; only "r" resets the whole chain.
//   Each closure is evaluated exactly once: after the reading settles, one line is printed
//   (even when no resistor is added) and nothing more can be added until the loop has
//   been opened and closed again.
//
// Calibration (do in this order; saved to EEPROM):
//   a) Topper off (loop open):            calopen
//   b) Known resistor in, topper on:      cal <range> <ohms>   once per range
//      Pick a known resistor that reads 15-85 % of full scale on that range
//      (roughly: range 0 ~1-25k, range 1 ~6-180k, range 2 ~40k-1M).
//
// Serial commands:
//   fold                 finish: print FINAL sequence, stop detecting until reset
//   r                    reset the sequence, start a new one
//   c                    print the calibration currently in use
//   calopen              record each range's open-loop count (its full-scale value)
//   cal <range> <ohms>   calibrate one range against a known resistor in the chain
//   caldef               restore nominal defaults and save to EEPROM
//   raw                  print the averaged ADC count + implied ohms for every range

#include <EEPROM.h>

// ---------------- Configuration ----------------
const int   READ_PIN = A0;

// ADC resolution: 14-bit on boards that support it (UNO R4), 10-bit on classic AVR.
#if defined(ARDUINO_ARCH_AVR)
const int   ADC_BITS = 10;
#else
const int   ADC_BITS = 14;
#endif
const float ADC_MAX  = (float)((1L << ADC_BITS) - 1);

const int   NUM_REFS = 3;
const int   REF_PINS[NUM_REFS] = {5, 6, 7};
// Nominal reference values in ohms. Live values are in REF_OHMS[] (loaded from EEPROM
// if a calibration was saved, otherwise copied from these defaults at startup).
const float REF_OHMS_DEFAULT[NUM_REFS] = {4700.0, 33000.0, 220000.0};
float       REF_OHMS[NUM_REFS];
// Full-scale count per range = the count read with the loop open ("calopen").
// The divider model is  count = FULL_COUNT * R / (R + REF_OHMS), so
//   R = REF_OHMS * count / (FULL_COUNT - count).
// Using the measured open-loop count instead of the ideal ADC maximum accounts for the
// pin not reaching the full supply voltage and for leakage at the A0 node.
float       FULL_COUNT[NUM_REFS];

const int   AVG_SAMPLES = 400;   // samples averaged per reading (noise reduction)
const int   SETTLE_MS   = 5;     // settle time after switching the active range

// Auto-range thresholds, as a fraction of that range's full-scale count
const float LOW_FRAC    = 0.15;  // below  -> switch to a SMALLER reference
const float HIGH_FRAC   = 0.85;  // above  -> switch to a LARGER reference
const float OPEN_FRAC   = 0.93;  // this high on the largest range -> open (no chain)
// A real chain never exceeds a few hundred kohm, so anything above this is "open".
const float OPEN_OHMS   = 600000.0;

// Stability / step detection
const float         STABLE_ABS       = 150.0;  // ohms: absolute tolerance for "steady"
const float         STABLE_FRAC      = 0.01;   // + relative tolerance (1% of the reading)
const unsigned long STABLE_MS        = 1500;   // reading must stay steady this long
const int           STABLE_MIN_READS = 4;      // ...and for at least this many readings
const float         STEP_MIN         = 1000.0; // ohms: minimum jump counted as a new resistor
const int           OPEN_CONFIRM_N   = 5;      // consecutive open reads before "loop open"

// Amino-acid match guard (ohms): reject if the nearest match is farther than this
const float MATCH_ABS   = 1200.0;
const float MATCH_FRAC  = 0.08;

// Serial command words. Change freely; comparison is trimmed + case-insensitive.
const char* FINISH_COMMAND = "fold";
const char* RESET_COMMAND  = "r";
const char* CALIB_COMMAND  = "c";

// EEPROM persistence of the calibration (magic changes whenever the layout/scale changes)
const uint32_t CAL_MAGIC = 0x52534532UL;  // "RSE2"
const int      EE_ADDR   = 0;

// ---------------- Amino-acid lookup (measured values from "resistor code.csv") ----------------
// aaseq2: only every other resistor from the CSV is used (rows 1, 3, 5, ... starting
// with A), so neighbouring values are at least 3.4 kohm apart instead of about 1.5 kohm.
// Left out: R, D, E, G, I, K, F, S, W, V.
const int   NUM_AA = 10;
const char  AA_LETTER[NUM_AA] = {
  'A','N','C','Q','H','L','M','P','T','Y'
};
const float AA_OHMS[NUM_AA] = {
  1980, 6500, 10000, 13700, 18000, 21900, 26600, 30000, 34800, 39000
};

// ---------------- State ----------------
String        sequence       = "";    // running amino-acid sequence
float         settledOhms    = 0.0;   // confirmed total resistance of the recognized chain
float         runMean        = 0.0;   // mean of the current steady run of readings (ohms)
int           runCount       = 0;     // readings in the current steady run (0 = none)
unsigned long runStartMs     = 0;     // when the current steady run started
bool          armed          = false; // loop has been opened since the last evaluation;
                                      // only an armed closure can add a resistor
int           openN          = 0;     // consecutive open (no chain) reads
int           currentRange   = 0;     // last-used reference index (persisted for speed)
bool          finished       = false; // true after FINISH_COMMAND, until reset
bool          calFromEeprom  = false; // whether live cal came from EEPROM
String        inLine         = "";    // serial line buffer

// ---------------- EEPROM calibration ----------------
void saveCal() {
  int addr = EE_ADDR;
  EEPROM.put(addr, CAL_MAGIC); addr += sizeof(CAL_MAGIC);
  for (int i = 0; i < NUM_REFS; i++) { EEPROM.put(addr, REF_OHMS[i]);   addr += sizeof(float); }
  for (int i = 0; i < NUM_REFS; i++) { EEPROM.put(addr, FULL_COUNT[i]); addr += sizeof(float); }
}

void restoreDefaults() {
  for (int i = 0; i < NUM_REFS; i++) {
    REF_OHMS[i]   = REF_OHMS_DEFAULT[i];
    FULL_COUNT[i] = ADC_MAX;
  }
}

bool loadCal() {
  int addr = EE_ADDR;
  uint32_t magic; EEPROM.get(addr, magic); addr += sizeof(magic);
  if (magic != CAL_MAGIC) return false;
  float rf[NUM_REFS], fc[NUM_REFS];
  for (int i = 0; i < NUM_REFS; i++) { EEPROM.get(addr, rf[i]); addr += sizeof(float); }
  for (int i = 0; i < NUM_REFS; i++) { EEPROM.get(addr, fc[i]); addr += sizeof(float); }
  for (int i = 0; i < NUM_REFS; i++) {
    if (!(rf[i] > 0.0 && rf[i] < 1.0e7)) return false;              // sanity checks
    if (!(fc[i] > 0.5 * ADC_MAX && fc[i] <= ADC_MAX)) return false;
  }
  for (int i = 0; i < NUM_REFS; i++) { REF_OHMS[i] = rf[i]; FULL_COUNT[i] = fc[i]; }
  return true;
}

// ---------------- Range selection / measurement ----------------
void selectRange(int r) {
  for (int i = 0; i < NUM_REFS; i++) {
    if (i == r) {
      pinMode(REF_PINS[i], OUTPUT);
      digitalWrite(REF_PINS[i], HIGH);   // this reference is active (sources ~5V)
    } else {
      pinMode(REF_PINS[i], INPUT);       // Hi-Z: effectively disconnected
    }
  }
  delay(SETTLE_MS);
  for (int i = 0; i < 8; i++) analogRead(READ_PIN);  // flush stale ADC sample/hold
}

// Averaged ADC count on range r (fractional counts are kept).
float readAvgCount(int r) {
  selectRange(r);
  unsigned long sum = 0;
  for (int i = 0; i < AVG_SAMPLES; i++) sum += (unsigned long)analogRead(READ_PIN);
  return (float)sum / (float)AVG_SAMPLES;
}

float countToOhms(int r, float c) {
  if (c <= 0.0) return 0.0;
  float denom = FULL_COUNT[r] - c;
  if (denom <= 0.0) return 1.0e12;       // at or above full scale: effectively open
  return REF_OHMS[r] * c / denom;
}

// Auto-ranges, then returns true with *ohms set if a finite chain is present,
// or false if the node reads open (no chain to GND).
bool measureChain(float *ohms) {
  int r = currentRange;
  for (int iter = 0; iter < NUM_REFS * 2 + 1; iter++) {
    float c = readAvgCount(r);
    float frac = c / FULL_COUNT[r];
    if (frac > HIGH_FRAC && r < NUM_REFS - 1) { r++; continue; }
    if (frac < LOW_FRAC  && r > 0)            { r--; continue; }
    currentRange = r;
    if (frac >= OPEN_FRAC) return false;                // open / no chain
    float rr = countToOhms(r, c);
    if (rr > OPEN_OHMS) return false;                   // too high to be a real chain
    *ohms = rr;
    return true;
  }
  currentRange = r;
  return false;
}

// ---------------- Amino-acid matching ----------------
int matchAminoAcid(float deltaOhms, float *bestErr) {
  int best = 0;
  float bestD = 1e12;
  for (int i = 0; i < NUM_AA; i++) {
    float d = fabs(deltaOhms - AA_OHMS[i]);
    if (d < bestD) { bestD = d; best = i; }
  }
  *bestErr = bestD;
  return best;
}

// ---------------- Serial commands ----------------
void printCalibration() {
  Serial.print("CALIB (");
  Serial.print(calFromEeprom ? "EEPROM" : "defaults");
  Serial.print(", "); Serial.print(ADC_BITS); Serial.println("-bit ADC):");
  for (int i = 0; i < NUM_REFS; i++) {
    Serial.print("  range "); Serial.print(i);
    Serial.print(" (D"); Serial.print(REF_PINS[i]);
    Serial.print("): reference="); Serial.print(REF_OHMS[i], 1);
    Serial.print(" ohm, open count="); Serial.println(FULL_COUNT[i], 1);
  }
}

void doCalibrateOpen() {
  float fc[NUM_REFS];
  for (int i = 0; i < NUM_REFS; i++) {
    fc[i] = readAvgCount(i);
    if (fc[i] < 0.80 * ADC_MAX) {
      Serial.print("calopen failed: range "); Serial.print(i);
      Serial.print(" reads "); Serial.print(fc[i], 1);
      Serial.println(" - the loop does not look open. Take the topper off and retry.");
      return;
    }
  }
  for (int i = 0; i < NUM_REFS; i++) FULL_COUNT[i] = fc[i];
  saveCal();
  calFromEeprom = true;
  Serial.print("CALOPEN: open counts =");
  for (int i = 0; i < NUM_REFS; i++) { Serial.print(' '); Serial.print(FULL_COUNT[i], 1); }
  Serial.println(" (saved). Now run 'cal <range> <ohms>' for each range.");
}

void doCalibrate(int r, float knownOhms) {
  if (r < 0 || r >= NUM_REFS || !(knownOhms > 0.0)) {
    Serial.println("usage: cal <range 0..2> <known_ohms>   e.g. cal 0 10000");
    return;
  }
  float c = readAvgCount(r);
  float frac = c / FULL_COUNT[r];
  if (frac < 0.05 || frac > 0.95) {
    Serial.print("cal failed: count "); Serial.print(c, 1);
    Serial.print(" is "); Serial.print(frac * 100.0, 0);
    Serial.println("% of full scale - is the known resistor in, topper on, and sized for this range?");
    return;
  }
  float rf = knownOhms * (FULL_COUNT[r] - c) / c;
  REF_OHMS[r] = rf;
  saveCal();
  calFromEeprom = true;
  Serial.print("CAL range "); Serial.print(r);
  Serial.print(" (D"); Serial.print(REF_PINS[r]); Serial.print("): count=");
  Serial.print(c, 1); Serial.print(" ("); Serial.print(frac * 100.0, 0);
  Serial.print("% of full scale), known="); Serial.print(knownOhms, 1);
  Serial.print(" -> reference="); Serial.print(rf, 1); Serial.println(" ohm (saved)");
  if (frac < LOW_FRAC || frac > HIGH_FRAC) {
    Serial.println("  warning: outside 15-85% of full scale; a better-sized known resistor will be more accurate.");
  }
}

void doRaw() {
  Serial.println("RAW (per range):");
  for (int i = 0; i < NUM_REFS; i++) {
    float c = readAvgCount(i);
    float frac = c / FULL_COUNT[i];
    Serial.print("  range "); Serial.print(i);
    Serial.print(" (D"); Serial.print(REF_PINS[i]); Serial.print("): count=");
    Serial.print(c, 1); Serial.print(" ("); Serial.print(frac * 100.0, 0);
    Serial.print("% of open "); Serial.print(FULL_COUNT[i], 0); Serial.print(")  ~ ");
    if (frac < OPEN_FRAC) { Serial.print(countToOhms(i, c) / 1000.0, 2); Serial.println(" k"); }
    else                  { Serial.println("(open)"); }
  }
}

void resetRun() {
  runCount = 0;
  armed = false;   // the loop must be opened and closed again before anything is added
}

void handleCommand(String raw) {
  raw.trim();
  if (raw.length() == 0) return;

  // Tokenize into a command plus up to two arguments.
  char buf[48];
  raw.toCharArray(buf, sizeof(buf));
  char* t0 = strtok(buf, " \t");
  if (!t0) return;
  char* t1 = strtok(NULL, " \t");
  char* t2 = strtok(NULL, " \t");

  String cmd = String(t0); cmd.toLowerCase();

  String finishCmd = String(FINISH_COMMAND); finishCmd.toLowerCase();
  String resetCmd  = String(RESET_COMMAND);  resetCmd.toLowerCase();
  String calibCmd  = String(CALIB_COMMAND);  calibCmd.toLowerCase();

  if (cmd == finishCmd) {
    finished = true;
    Serial.print("FINAL: "); Serial.println(sequence);
    Serial.println("(sequence complete - type 'r' to start a new one)");
  } else if (cmd == resetCmd) {
    sequence = "";
    settledOhms = 0.0;
    resetRun();
    finished = false;
    Serial.println("RESET: ready for a new sequence (start from an empty chain with the loop open)");
  } else if (cmd == calibCmd) {
    printCalibration();
  } else if (cmd == "calopen") {
    doCalibrateOpen();
    resetRun();
  } else if (cmd == "cal") {
    if (!t1 || !t2) { Serial.println("usage: cal <range 0..2> <known_ohms>   e.g. cal 0 10000"); }
    else { doCalibrate(atoi(t1), atof(t2)); resetRun(); }
  } else if (cmd == "caldef") {
    restoreDefaults();
    saveCal();
    calFromEeprom = false;
    resetRun();
    Serial.println("Defaults restored and saved.");
    printCalibration();
  } else if (cmd == "raw") {
    doRaw();
  } else {
    Serial.print("Unknown command: "); Serial.println(raw);
  }
}

void readSerial() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (inLine.length() > 0) { handleCommand(inLine); inLine = ""; }
    } else {
      inLine += ch;
      if (inLine.length() > 46) inLine = "";  // guard against runaway input
    }
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(9600);
#if !defined(ARDUINO_ARCH_AVR)
  analogReadResolution(ADC_BITS);
#endif
  pinMode(READ_PIN, INPUT);
  for (int i = 0; i < NUM_REFS; i++) pinMode(REF_PINS[i], INPUT);
  currentRange = 0;

  restoreDefaults();
  calFromEeprom = loadCal();   // override defaults if a saved calibration exists

  Serial.println("Resistor sequencer ready (3-reference auto-ranging).");
  Serial.println("Add a resistor, close the topper; the sequence prints after each.");
  Serial.println("Commands: fold | r | c | calopen | cal <range> <ohms> | caldef | raw");
  printCalibration();
}

void loop() {
  readSerial();

  if (finished) { delay(20); return; }  // stop detecting until reset

  float m;
  bool haveChain = measureChain(&m);

  if (!haveChain) {                      // open / no chain
    runCount = 0;
    // The baseline is kept: closing the loop again is compared to the last closed total.
    if (openN < OPEN_CONFIRM_N) {
      openN++;
      if (openN == OPEN_CONFIRM_N) {
        Serial.println("Loop open - add the next resistor and close the topper");
      }
    }
    if (openN >= OPEN_CONFIRM_N) armed = true;   // a confirmed open arms the next closure
    delay(10);
    return;
  }
  openN = 0;

  // Track a steady run of readings; the accepted value is the mean of the run.
  float tol = STABLE_ABS + STABLE_FRAC * m;
  if (runCount > 0 && fabs(m - runMean) <= tol) {
    runCount++;
    runMean += (m - runMean) / (float)runCount;
  } else {
    runCount = 1;
    runMean = m;
    runStartMs = millis();
  }

  // Exactly one evaluation per closure: nothing can be added again until the loop
  // has been opened and closed again.
  if (!armed) return;
  if (runCount < STABLE_MIN_READS || (millis() - runStartMs) < STABLE_MS) return;
  armed = false;

  float total = runMean;
  float delta = total - settledOhms;

  if (delta > STEP_MIN) {                // a real upward step = a newly added resistor
    float err;
    int idx = matchAminoAcid(delta, &err);
    float guard = MATCH_ABS + MATCH_FRAC * delta;

    if (err <= guard) {
      sequence += AA_LETTER[idx];
      Serial.print("Added "); Serial.print(AA_LETTER[idx]);
      Serial.print("  (delta "); Serial.print(delta / 1000.0, 2);
      Serial.print(" k ~ "); Serial.print(AA_OHMS[idx] / 1000.0, 2);
      Serial.print(" k; total "); Serial.print(total / 1000.0, 2);
      Serial.print(" k, range "); Serial.print(currentRange);
      Serial.print(")  ->  SEQ: "); Serial.println(sequence);
    } else {
      Serial.print("Unrecognized resistor: delta "); Serial.print(delta / 1000.0, 2);
      Serial.print(" k (nearest "); Serial.print(AA_LETTER[idx]);
      Serial.print(" off by "); Serial.print(err / 1000.0, 2);
      Serial.print(" k; total "); Serial.print(total / 1000.0, 2);
      Serial.print(" k, range "); Serial.print(currentRange);
      Serial.println(") - not added");
    }
    settledOhms = total;                 // resistor is physically present: advance the baseline
  } else {                               // loop closed but nothing new: say so
    Serial.print("Loop closed: total "); Serial.print(total / 1000.0, 2);
    Serial.print(" k (range "); Serial.print(currentRange);
    Serial.print("), baseline "); Serial.print(settledOhms / 1000.0, 2);
    Serial.print(" k, change "); Serial.print(delta / 1000.0, 2);
    Serial.println(" k - no new resistor added");
  }
}
