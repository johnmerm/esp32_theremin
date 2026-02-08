/**
 * ESP32-S2 Theremin
 *
 * A digital theremin using ultrasonic sonar sensors:
 * - One sensor controls pitch (frequency/MIDI note)
 * - Optional second sensor controls volume (amplitude/MIDI velocity)
 *
 * Features:
 * - DAC audio output with configurable waveforms
 * - USB MIDI output for controlling external synthesizers
 * - Smooth sensor readings with configurable filtering
 * - Optional volume sonar (enable VOLUME_SONAR_ENABLED in config.h)
 * - Serial debug with "monitor" and "raw" output modes
 *
 * Hardware:
 * - ESP32-S2 development board
 * - One or two HC-SR04 ultrasonic sensors
 * - Audio output via DAC (GPIO17)
 *
 * Author: ESP32 Theremin Project
 * License: MIT
 */

#include <Arduino.h>
#include "config.h"
#if MIDI_ENABLED
#include "midiusb.h"
#endif

// ============================================================================
// Global Variables
// ============================================================================

#if MIDI_ENABLED
// USB MIDI instance
MIDIusb MIDIout;

// MIDI state
int8_t lastMidiNote = -1;
uint8_t lastMidiVelocity = 0;
unsigned long lastMidiUpdate = 0;
#endif

// Sonar readings (smoothed)
volatile float pitchDistance = MAX_DISTANCE_CM;
#ifdef VOLUME_SONAR_ENABLED
volatile float volumeDistance = MAX_DISTANCE_CM;
#endif

// Audio generation
volatile float currentFrequency = MIN_FREQUENCY;
volatile uint8_t currentVolume = 0;
volatile float phaseAccumulator = 0.0f;

// Waveform lookup table (256 entries for sine wave)
static uint8_t sineTable[256];

// Timing
unsigned long lastSonarRead = 0;
unsigned long lastDebugPrint = 0;

// Timer for audio generation
hw_timer_t *audioTimer = NULL;

// Debug output mode
#if DEBUG_ENABLED
enum DebugMode { MODE_MONITOR, MODE_RAW };
DebugMode debugMode = MODE_MONITOR;
String serialInputBuffer = "";
#endif

// ============================================================================
// Note name utility (used for debug display)
// ============================================================================

static const char* NOTE_NAMES[] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/**
 * Convert frequency to MIDI note number
 */
int8_t frequencyToNoteNumber(float frequency) {
  if (frequency <= 0) return -1;
  return (int8_t)(69.0f + 12.0f * log2(frequency / 440.0f));
}

/**
 * Get note name string (e.g. "A4", "C#5") from MIDI note number
 */
void noteName(int8_t noteNum, char *buf, size_t bufLen) {
  if (noteNum < 0 || noteNum > 127) {
    snprintf(buf, bufLen, "---");
    return;
  }
  int octave = (noteNum / 12) - 1;
  int note = noteNum % 12;
  snprintf(buf, bufLen, "%s%d", NOTE_NAMES[note], octave);
}

// ============================================================================
// Waveform Generation
// ============================================================================

/**
 * Initialize the sine lookup table
 */
void initSineTable() {
  for (int i = 0; i < 256; i++) {
    sineTable[i] = (uint8_t)(128.0f + 127.0f * sin(2.0f * PI * i / 256.0f));
  }
}

/**
 * Generate waveform sample based on phase (0-255)
 */
uint8_t IRAM_ATTR generateWaveform(uint8_t phase) {
  switch (WAVEFORM_TYPE) {
    case 0:  // Sine
      return sineTable[phase];

    case 1:  // Triangle
      if (phase < 128) {
        return phase * 2;
      } else {
        return 255 - (phase - 128) * 2;
      }

    case 2:  // Sawtooth
      return phase;

    case 3:  // Square
      return (phase < 128) ? 255 : 0;

    default:
      return sineTable[phase];
  }
}

// ============================================================================
// Audio Timer ISR
// ============================================================================

/**
 * Timer interrupt for audio sample generation
 * Called at AUDIO_SAMPLE_RATE Hz
 */
void IRAM_ATTR onAudioTimer() {
  // Calculate phase increment based on frequency
  float phaseIncrement = (currentFrequency * 256.0f) / AUDIO_SAMPLE_RATE;

  // Update phase accumulator
  phaseAccumulator += phaseIncrement;
  if (phaseAccumulator >= 256.0f) {
    phaseAccumulator -= 256.0f;
  }

  // Generate waveform sample
  uint8_t sample = generateWaveform((uint8_t)phaseAccumulator);

  // Apply volume scaling
  sample = (uint8_t)((sample * currentVolume) / 255);

  // Output to DAC
  dacWrite(DAC_OUTPUT_PIN, sample);
}

// ============================================================================
// Sonar Functions
// ============================================================================

/**
 * Measure distance using ultrasonic sensor
 * Returns distance in centimeters, or -1 if timeout
 */
float measureDistance(int triggerPin, int echoPin) {
  // Send trigger pulse
  digitalWrite(triggerPin, LOW);
  delayMicroseconds(2);
  digitalWrite(triggerPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(triggerPin, LOW);

  // Measure echo pulse duration
  long duration = pulseIn(echoPin, HIGH, SONAR_TIMEOUT_US);

  if (duration == 0) {
    return -1;  // Timeout - no echo received
  }

  // Calculate distance: speed of sound = 343 m/s = 0.0343 cm/us
  // Distance = duration * 0.0343 / 2 (divide by 2 for round trip)
  float distance = duration * 0.01715f;

  return distance;
}

/**
 * Read and smooth sonar distances
 */
void readSonars() {
  // Read pitch sonar
  float newPitchDist = measureDistance(PITCH_TRIGGER_PIN, PITCH_ECHO_PIN);
  if (newPitchDist > 0) {
    // Apply exponential smoothing
    pitchDistance = pitchDistance * (1.0f - DISTANCE_SMOOTHING) +
                    newPitchDist * DISTANCE_SMOOTHING;
    // Clamp to valid range
    pitchDistance = constrain(pitchDistance, MIN_DISTANCE_CM, MAX_DISTANCE_CM);
  }

#ifdef VOLUME_SONAR_ENABLED
  // Small delay between sensor readings to avoid interference
  delayMicroseconds(500);

  // Read volume sonar
  float newVolumeDist = measureDistance(VOLUME_TRIGGER_PIN, VOLUME_ECHO_PIN);
  if (newVolumeDist > 0) {
    volumeDistance = volumeDistance * (1.0f - DISTANCE_SMOOTHING) +
                     newVolumeDist * DISTANCE_SMOOTHING;
    volumeDistance = constrain(volumeDistance, MIN_DISTANCE_CM, MAX_DISTANCE_CM);
  }
#endif
}

// ============================================================================
// Audio Control Functions
// ============================================================================

/**
 * Map pitch distance to frequency (logarithmic mapping for musical scale)
 */
float distanceToFrequency(float distance) {
  // Normalize distance to 0-1 range
  float normalized = (distance - MIN_DISTANCE_CM) /
                     (MAX_DISTANCE_CM - MIN_DISTANCE_CM);

  // Invert so closer = higher pitch
  normalized = 1.0f - normalized;

  // Logarithmic mapping for musical frequency scaling
  // f = f_min * (f_max/f_min)^normalized
  float ratio = MAX_FREQUENCY / MIN_FREQUENCY;
  float frequency = MIN_FREQUENCY * pow(ratio, normalized);

  return frequency;
}

#ifdef VOLUME_SONAR_ENABLED
/**
 * Map volume distance to amplitude
 */
uint8_t distanceToVolume(float distance) {
  // Normalize distance to 0-1 range
  float normalized = (distance - MIN_DISTANCE_CM) /
                     (MAX_DISTANCE_CM - MIN_DISTANCE_CM);

  // Invert so closer = louder
  normalized = 1.0f - normalized;

  // Linear mapping to volume range
  uint8_t volume = (uint8_t)(MIN_VOLUME + normalized * (MAX_VOLUME - MIN_VOLUME));

  return volume;
}
#endif

/**
 * Update audio parameters from sonar readings
 */
void updateAudio() {
  currentFrequency = distanceToFrequency(pitchDistance);
#ifdef VOLUME_SONAR_ENABLED
  currentVolume = distanceToVolume(volumeDistance);
#else
  currentVolume = FIXED_VOLUME;
#endif
}

// ============================================================================
// MIDI Functions
// ============================================================================

#if MIDI_ENABLED

/**
 * Convert frequency to MIDI note number with pitch bend
 */
void frequencyToMidi(float frequency, int8_t *note, int16_t *pitchBend) {
  // MIDI note = 69 + 12 * log2(f / 440)
  float midiFloat = 69.0f + 12.0f * log2(frequency / 440.0f);

  // Clamp to valid MIDI range
  midiFloat = constrain(midiFloat, MIDI_NOTE_MIN, MIDI_NOTE_MAX);

  // Extract integer note and fractional part for pitch bend
  *note = (int8_t)midiFloat;
  float fraction = midiFloat - *note;

  // Convert fraction to pitch bend value
  // Pitch bend range: -8192 to 8191 (14-bit)
  // Scale fraction to pitch bend considering PITCH_BEND_RANGE
  *pitchBend = (int16_t)(fraction * 8192.0f / PITCH_BEND_RANGE);
}

#ifdef VOLUME_SONAR_ENABLED
/**
 * Convert volume distance to MIDI velocity
 */
uint8_t distanceToVelocity(float distance) {
  float normalized = (distance - MIN_DISTANCE_CM) /
                     (MAX_DISTANCE_CM - MIN_DISTANCE_CM);
  normalized = 1.0f - normalized;  // Invert

  uint8_t velocity = (uint8_t)(MIDI_VELOCITY_MIN +
                               normalized * (MIDI_VELOCITY_MAX - MIDI_VELOCITY_MIN));
  return velocity;
}
#endif

/**
 * Send MIDI note on message
 */
void sendNoteOn(uint8_t note, uint8_t velocity) {
  MIDIout.noteON(note, velocity, MIDI_CHANNEL - 1);
}

/**
 * Send MIDI note off message
 */
void sendNoteOff(uint8_t note) {
  MIDIout.noteOFF(note, 0, MIDI_CHANNEL - 1);
}

/**
 * Send MIDI pitch bend message
 */
void sendPitchBend(int16_t bend) {
  // pitchChange takes uint16_t (0-16383, center at 8192)
  uint16_t bendValue = (uint16_t)(bend + 8192);
  MIDIout.pitchChange(bendValue, MIDI_CHANNEL - 1);
}

/**
 * Send MIDI control change for volume (CC7)
 */
void sendVolume(uint8_t volume) {
  MIDIout.controlChange(7, volume, MIDI_CHANNEL - 1);
}

/**
 * Update MIDI output based on current readings
 */
void updateMidi() {
  int8_t note;
  int16_t pitchBend;
  frequencyToMidi(currentFrequency, &note, &pitchBend);

#ifdef VOLUME_SONAR_ENABLED
  uint8_t velocity = distanceToVelocity(volumeDistance);

  // Check if we should turn off the note (hand far away = volume too low)
  if (velocity < NOTE_OFF_THRESHOLD) {
    if (lastMidiNote >= 0) {
      sendNoteOff(lastMidiNote);
      lastMidiNote = -1;
    }
    return;
  }
#else
  uint8_t velocity = FIXED_MIDI_VELOCITY;
#endif

  // Note changed - send note off for old, note on for new
  if (note != lastMidiNote) {
    if (lastMidiNote >= 0) {
      sendNoteOff(lastMidiNote);
    }
    sendNoteOn(note, velocity);
    lastMidiNote = note;
  }

  // Always update pitch bend for smooth pitch transitions
  sendPitchBend(pitchBend);

  // Update volume via CC7
  sendVolume(velocity);

  lastMidiVelocity = velocity;
}

#endif // MIDI_ENABLED

// ============================================================================
// Debug / Serial Command Functions
// ============================================================================

#if DEBUG_ENABLED

// ANSI escape helpers
#define ANSI_HOME       "\033[H"
#define ANSI_CLEAR      "\033[2J"
#define ANSI_CLEAR_LINE "\033[K"
#define ANSI_GOTO(r,c)  "\033[" #r ";" #c "H"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"
#define ANSI_RESET      "\033[0m"

void printHelp() {
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(ANSI_BOLD "Commands:" ANSI_RESET);
  DEBUG_SERIAL.println("  monitor  - ANSI dashboard (default)");
  DEBUG_SERIAL.println("  raw      - CSV output for scripts");
  DEBUG_SERIAL.println("  help     - show this help");
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(ANSI_DIM "Raw format: D,<ms>,<dist_cm>,<freq_hz>,<volume>,<note_num>,<note_name>" ANSI_RESET);
  DEBUG_SERIAL.println();
}

void printMonitorHeader() {
  DEBUG_SERIAL.print(ANSI_CLEAR);
  DEBUG_SERIAL.print(ANSI_HOME);
  DEBUG_SERIAL.println(ANSI_BOLD "=== ESP32-S2 Theremin ===" ANSI_RESET);
  DEBUG_SERIAL.println(ANSI_DIM "Send 'raw' for CSV, 'help' for commands" ANSI_RESET);
}

void printMonitor() {
  int8_t note = frequencyToNoteNumber(currentFrequency);
  char name[8];
  noteName(note, name, sizeof(name));

  DEBUG_SERIAL.print(ANSI_GOTO(4,1));
  DEBUG_SERIAL.printf("Distance:  %7.1f cm" ANSI_CLEAR_LINE "\n", pitchDistance);
  DEBUG_SERIAL.printf("Frequency: %7.1f Hz" ANSI_CLEAR_LINE "\n", currentFrequency);
  DEBUG_SERIAL.printf("Note:      %4s (MIDI %d)" ANSI_CLEAR_LINE "\n", name, note);
#ifdef VOLUME_SONAR_ENABLED
  DEBUG_SERIAL.printf("Vol dist:  %7.1f cm" ANSI_CLEAR_LINE "\n", volumeDistance);
#endif
  DEBUG_SERIAL.printf("Volume:    %7d/255" ANSI_CLEAR_LINE "\n", currentVolume);
#if MIDI_ENABLED
  DEBUG_SERIAL.printf("MIDI sent: %4s  vel=%d" ANSI_CLEAR_LINE "\n", name, lastMidiVelocity);
#endif
  DEBUG_SERIAL.printf("Uptime:    %7lu s" ANSI_CLEAR_LINE "\n", millis() / 1000);
}

void printRaw() {
  int8_t note = frequencyToNoteNumber(currentFrequency);
  char name[8];
  noteName(note, name, sizeof(name));

  // CSV: D,timestamp_ms,distance_cm,frequency_hz,volume,note_number,note_name
  DEBUG_SERIAL.printf("D,%lu,%.1f,%.1f,%d,%d,%s\n",
    millis(),
    pitchDistance,
    currentFrequency,
    currentVolume,
    note,
    name);
}

void processSerialCommand(const String &cmd) {
  String trimmed = cmd;
  trimmed.trim();
  trimmed.toLowerCase();

  if (trimmed == "raw") {
    debugMode = MODE_RAW;
    DEBUG_SERIAL.println("OK:raw");
  } else if (trimmed == "monitor") {
    debugMode = MODE_MONITOR;
    printMonitorHeader();
  } else if (trimmed == "help") {
    printHelp();
  } else if (trimmed.length() > 0) {
    DEBUG_SERIAL.printf("ERR:unknown command '%s'\n", trimmed.c_str());
  }
}

void handleSerialInput() {
  while (DEBUG_SERIAL.available()) {
    char c = DEBUG_SERIAL.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        processSerialCommand(serialInputBuffer);
        serialInputBuffer = "";
      }
    } else {
      serialInputBuffer += c;
    }
  }
}

void printDebug() {
  if (debugMode == MODE_RAW) {
    printRaw();
  } else {
    printMonitor();
  }
}

#endif // DEBUG_ENABLED

// ============================================================================
// Setup and Loop
// ============================================================================

void setup() {
  // Initialize sonar pins first (before anything that might block)
  pinMode(PITCH_TRIGGER_PIN, OUTPUT);
  digitalWrite(PITCH_TRIGGER_PIN, LOW);
  pinMode(PITCH_ECHO_PIN, INPUT);
#ifdef VOLUME_SONAR_ENABLED
  pinMode(VOLUME_TRIGGER_PIN, OUTPUT);
  digitalWrite(VOLUME_TRIGGER_PIN, LOW);
  pinMode(VOLUME_ECHO_PIN, INPUT);
#endif

  // Initialize DAC
  pinMode(DAC_OUTPUT_PIN, OUTPUT);

  // Initialize serial for debugging
  #if DEBUG_ENABLED
  DEBUG_SERIAL.begin(115200);
  unsigned long serialWait = millis();
  while (!DEBUG_SERIAL && millis() - serialWait < 3000) delay(10);
  #endif

#if MIDI_ENABLED
  // Initialize USB MIDI
  MIDIout.begin();
#endif

  // Initialize sine lookup table
  initSineTable();

  // Setup audio timer
  // Timer 0, prescaler 80 (1 MHz), count up
  audioTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(audioTimer, &onAudioTimer, true);
  // Set alarm to trigger at sample rate
  timerAlarmWrite(audioTimer, 1000000 / AUDIO_SAMPLE_RATE, true);
  timerAlarmEnable(audioTimer);

  #if DEBUG_ENABLED
  printMonitorHeader();
  #endif
}

void loop() {
  unsigned long currentTime = millis();

  // Read sonars at configured interval
  if (currentTime - lastSonarRead >= SONAR_INTERVAL_MS) {
    lastSonarRead = currentTime;
    readSonars();
    updateAudio();
  }

#if MIDI_ENABLED
  // MIDI test: send a note on/off every second to verify USB MIDI works
  static unsigned long lastMidiTest = 0;
  static bool testNoteOn = false;
  if (currentTime - lastMidiTest >= 1000) {
    lastMidiTest = currentTime;
    if (testNoteOn) {
      MIDIout.noteOFF(60, 0, 0);  // C4 off
      #if DEBUG_ENABLED
      DEBUG_SERIAL.println("MIDI TEST: note OFF 60");
      #endif
    } else {
      MIDIout.noteON(60, 127, 0);  // C4 on, full velocity
      #if DEBUG_ENABLED
      DEBUG_SERIAL.println("MIDI TEST: note ON 60");
      #endif
    }
    testNoteOn = !testNoteOn;
  }

  // Update MIDI at configured interval
  if (currentTime - lastMidiUpdate >= MIDI_UPDATE_MS) {
    lastMidiUpdate = currentTime;
    updateMidi();
  }
#endif

  // Handle serial commands and debug output
  #if DEBUG_ENABLED
  handleSerialInput();

  if (currentTime - lastDebugPrint >= DEBUG_INTERVAL_MS) {
    lastDebugPrint = currentTime;
    printDebug();
  }
  #endif

  // Small delay to prevent watchdog issues
  delay(1);
}
