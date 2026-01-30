/**
 * ESP32-S2 Theremin
 *
 * A digital theremin using two ultrasonic sonar sensors:
 * - One sensor controls pitch (frequency/MIDI note)
 * - One sensor controls volume (amplitude/MIDI velocity)
 *
 * Features:
 * - DAC audio output with configurable waveforms
 * - USB MIDI output for controlling external synthesizers
 * - Smooth sensor readings with configurable filtering
 *
 * Hardware:
 * - ESP32-S2 development board
 * - Two HC-SR04 ultrasonic sensors
 * - Audio output via DAC (GPIO17)
 *
 * Author: ESP32 Theremin Project
 * License: MIT
 */

#include <USB.h>
#include <USBMIDI.h>
#include "config.h"

// ============================================================================
// Global Variables
// ============================================================================

// USB MIDI instance
USBMIDI MIDI;

// Sonar readings (smoothed)
volatile float pitchDistance = MAX_DISTANCE_CM;
volatile float volumeDistance = MAX_DISTANCE_CM;

// Audio generation
volatile float currentFrequency = MIN_FREQUENCY;
volatile uint8_t currentVolume = 0;
volatile float phaseAccumulator = 0.0f;

// Waveform lookup table (256 entries for sine wave)
static uint8_t sineTable[256];

// MIDI state
int8_t lastMidiNote = -1;
uint8_t lastMidiVelocity = 0;
unsigned long lastMidiUpdate = 0;

// Timing
unsigned long lastSonarRead = 0;
unsigned long lastDebugPrint = 0;

// Timer for audio generation
hw_timer_t *audioTimer = NULL;

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

  // Small delay between sensor readings to avoid interference
  delayMicroseconds(500);

  // Read volume sonar
  float newVolumeDist = measureDistance(VOLUME_TRIGGER_PIN, VOLUME_ECHO_PIN);
  if (newVolumeDist > 0) {
    volumeDistance = volumeDistance * (1.0f - DISTANCE_SMOOTHING) +
                     newVolumeDist * DISTANCE_SMOOTHING;
    volumeDistance = constrain(volumeDistance, MIN_DISTANCE_CM, MAX_DISTANCE_CM);
  }
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

/**
 * Update audio parameters from sonar readings
 */
void updateAudio() {
  currentFrequency = distanceToFrequency(pitchDistance);
  currentVolume = distanceToVolume(volumeDistance);
}

// ============================================================================
// MIDI Functions
// ============================================================================

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

/**
 * Send MIDI note on message
 */
void sendNoteOn(uint8_t note, uint8_t velocity) {
  MIDI.noteOn(note, velocity, MIDI_CHANNEL);
}

/**
 * Send MIDI note off message
 */
void sendNoteOff(uint8_t note) {
  MIDI.noteOff(note, 0, MIDI_CHANNEL);
}

/**
 * Send MIDI pitch bend message
 */
void sendPitchBend(int16_t bend) {
  // Convert to 14-bit unsigned (0-16383, center at 8192)
  uint16_t bendValue = (uint16_t)(bend + 8192);
  MIDI.pitchBend(bendValue, MIDI_CHANNEL);
}

/**
 * Send MIDI control change for volume (CC7)
 */
void sendVolume(uint8_t volume) {
  MIDI.controlChange(7, volume, MIDI_CHANNEL);
}

/**
 * Update MIDI output based on current readings
 */
void updateMidi() {
  if (!MIDI_ENABLED) return;

  int8_t note;
  int16_t pitchBend;
  frequencyToMidi(currentFrequency, &note, &pitchBend);

  uint8_t velocity = distanceToVelocity(volumeDistance);

  // Check if we should turn off the note (hand far away = volume too low)
  if (velocity < NOTE_OFF_THRESHOLD) {
    if (lastMidiNote >= 0) {
      sendNoteOff(lastMidiNote);
      lastMidiNote = -1;
    }
    return;
  }

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

// ============================================================================
// Debug Functions
// ============================================================================

#if DEBUG_ENABLED
void printDebug() {
  Serial.print("Pitch: ");
  Serial.print(pitchDistance, 1);
  Serial.print(" cm -> ");
  Serial.print(currentFrequency, 1);
  Serial.print(" Hz | Volume: ");
  Serial.print(volumeDistance, 1);
  Serial.print(" cm -> ");
  Serial.print(currentVolume);
  Serial.print(" | MIDI Note: ");
  Serial.print(lastMidiNote);
  Serial.print(" Vel: ");
  Serial.println(lastMidiVelocity);
}
#endif

// ============================================================================
// Setup and Loop
// ============================================================================

void setup() {
  // Initialize serial for debugging
  #if DEBUG_ENABLED
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("ESP32-S2 Theremin Starting...");
  #endif

  // Initialize USB and MIDI
  USB.begin();
  MIDI.begin();

  #if DEBUG_ENABLED
  Serial.println("USB MIDI initialized");
  #endif

  // Initialize sonar pins
  pinMode(PITCH_TRIGGER_PIN, OUTPUT);
  pinMode(PITCH_ECHO_PIN, INPUT);
  pinMode(VOLUME_TRIGGER_PIN, OUTPUT);
  pinMode(VOLUME_ECHO_PIN, INPUT);

  // Initialize DAC
  pinMode(DAC_OUTPUT_PIN, OUTPUT);

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
  Serial.println("Audio timer started at ");
  Serial.print(AUDIO_SAMPLE_RATE);
  Serial.println(" Hz");
  Serial.println("Theremin ready!");
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

  // Update MIDI at configured interval
  if (MIDI_ENABLED && currentTime - lastMidiUpdate >= MIDI_UPDATE_MS) {
    lastMidiUpdate = currentTime;
    updateMidi();
  }

  // Debug output
  #if DEBUG_ENABLED
  if (currentTime - lastDebugPrint >= DEBUG_INTERVAL_MS) {
    lastDebugPrint = currentTime;
    printDebug();
  }
  #endif

  // Small delay to prevent watchdog issues
  delay(1);
}
