/**
 * ESP32-S2 Theremin Configuration
 *
 * Hardware configuration and tuning parameters
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Hardware Pin Configuration
// ============================================================================

// Pitch sonar (controls frequency/note)
#define PITCH_TRIGGER_PIN   1
#define PITCH_ECHO_PIN      2

// Volume sonar (controls amplitude/velocity)
#define VOLUME_TRIGGER_PIN  3
#define VOLUME_ECHO_PIN     4

// DAC output (ESP32-S2 has DAC on GPIO17 and GPIO18)
#define DAC_OUTPUT_PIN      17

// ============================================================================
// Sonar Configuration
// ============================================================================

// Measurement timing
#define SONAR_TIMEOUT_US    30000   // Timeout for echo (30ms ~= 5m range)
#define SONAR_INTERVAL_MS   20      // Time between measurements (50Hz rate)

// Distance range in centimeters
#define MIN_DISTANCE_CM     5       // Minimum detection distance
#define MAX_DISTANCE_CM     100     // Maximum detection distance

// Smoothing factor (0.0 - 1.0, higher = smoother but slower response)
#define DISTANCE_SMOOTHING  0.3f

// ============================================================================
// Audio Configuration
// ============================================================================

// Sample rate for DAC output
#define AUDIO_SAMPLE_RATE   40000   // 40kHz sample rate

// Frequency range in Hz
#define MIN_FREQUENCY       110.0f  // A2
#define MAX_FREQUENCY       880.0f  // A5 (3 octaves)

// Volume range (0-255 for 8-bit DAC)
#define MIN_VOLUME          0
#define MAX_VOLUME          255

// Waveform type: 0=Sine, 1=Triangle, 2=Sawtooth, 3=Square
#define WAVEFORM_TYPE       0

// ============================================================================
// MIDI Configuration
// ============================================================================

// Enable/disable MIDI output
#define MIDI_ENABLED        true

// MIDI channel (1-16)
#define MIDI_CHANNEL        1

// MIDI note range
#define MIDI_NOTE_MIN       45      // A2
#define MIDI_NOTE_MAX       81      // A5

// MIDI update rate (ms between note changes)
#define MIDI_UPDATE_MS      50

// Pitch bend range (semitones)
#define PITCH_BEND_RANGE    2

// Velocity scaling
#define MIDI_VELOCITY_MIN   0
#define MIDI_VELOCITY_MAX   127

// Note-off threshold (velocity below this turns note off)
#define NOTE_OFF_THRESHOLD  5

// ============================================================================
// Debug Configuration
// ============================================================================

// Enable serial debug output (disable for production)
#define DEBUG_ENABLED       false

// Debug print interval (ms)
#define DEBUG_INTERVAL_MS   100

#endif // CONFIG_H
