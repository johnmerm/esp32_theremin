# ESP32-S2 Theremin

A digital theremin using an ESP32-S2 microcontroller with two ultrasonic sonar sensors for gesture-based control of pitch and volume. Features analog audio output via DAC and USB MIDI output for controlling external synthesizers.

## Features

- **Dual Sonar Control**: One sensor controls pitch, the other controls volume
- **DAC Audio Output**: Direct analog audio output with multiple waveform options (sine, triangle, sawtooth, square)
- **USB MIDI**: Native USB MIDI output for controlling DAWs and synthesizers
- **Configurable Range**: Adjustable frequency range, volume sensitivity, and MIDI parameters
- **Smooth Response**: Exponential smoothing filter for stable, musical control

## Hardware Requirements

### Components

| Component | Quantity | Description |
|-----------|----------|-------------|
| ESP32-S2 Dev Board | 1 | Any ESP32-S2 based board (e.g., ESP32-S2-Saola, Lolin S2 Mini) |
| HC-SR04 Ultrasonic Sensor | 2 | Standard ultrasonic distance sensors |
| Audio Amplifier (optional) | 1 | PAM8403 or similar for speaker output |
| Speaker (optional) | 1 | 8Ω speaker for audio output |
| Breadboard & Wires | - | For connections |

### Wiring Diagram

```
ESP32-S2              Pitch Sonar (HC-SR04)
---------             ---------------------
GPIO 1  ------------> TRIG
GPIO 2  <------------ ECHO
3.3V    ------------> VCC
GND     ------------> GND

ESP32-S2              Volume Sonar (HC-SR04) [optional]
---------             --------------------------------
GPIO 3  ------------> TRIG
GPIO 4  <------------ ECHO
3.3V    ------------> VCC
GND     ------------> GND

ESP32-S2              Audio Output
---------             ------------
GPIO 17 (DAC1) -----> Amplifier IN (or direct to high-impedance headphones)
GND     ------------> Amplifier GND

ESP32-S2              USB (MIDI out)
---------             --------------
GPIO 19 (D-)          USB D-  (directly via USB connector)
GPIO 20 (D+)          USB D+  (directly via USB connector)
```

**Notes**:
- HC-SR04 sensors typically require 5V for VCC, but many work reliably at 3.3V. If you have issues, use a level shifter or 5V-tolerant GPIO pins.
- GPIO 19/20 are the ESP32-S2's native USB pins, directly wired to the USB connector on most dev boards.
- The volume sonar is optional. To enable it, uncomment `#define VOLUME_SONAR_ENABLED` in `config.h`.

## Software Setup

### Arduino IDE Setup

1. **Install Arduino IDE** (version 2.0+ recommended)

2. **Add ESP32 Board Support**:
   - Go to File → Preferences
   - Add to "Additional Board Manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Go to Tools → Board → Boards Manager
   - Search for "esp32" and install "esp32 by Espressif Systems"

3. **Select Board**:
   - Tools → Board → ESP32S2 Dev Module (or your specific board)
   - Tools → USB Mode → "USB-OTG (TinyUSB)"
   - Tools → USB CDC On Boot → "Enabled" (for debug output)

4. **Upload the Sketch**:
   - Open `esp32_theremin/esp32_theremin.ino`
   - Connect your ESP32-S2 via USB
   - Click Upload

### PlatformIO Setup (Alternative)

Create a `platformio.ini` file:

```ini
[env:nodemcu-32s2]
platform = espressif32
board = nodemcu-32s2
framework = arduino
build_flags =
    -DARDUINO_USB_MODE=0
    -DARDUINO_USB_CDC_ON_BOOT=1
build_unflags =
    -DARDUINO_USB_MODE=1
upload_protocol = esptool
monitor_speed = 115200
lib_deps =
    chegewara/ESP32TinyUSB@^2.0.2
```

**Important**: `ARDUINO_USB_MODE` must be `0` (TinyUSB/USB-OTG) for MIDI to work. Mode `1` is Hardware CDC/JTAG only and does not support MIDI.

## Configuration

All configuration parameters are in `config.h`:

### Pin Configuration

```cpp
#define PITCH_TRIGGER_PIN   1   // Pitch sonar trigger
#define PITCH_ECHO_PIN      2   // Pitch sonar echo
#define VOLUME_TRIGGER_PIN  3   // Volume sonar trigger
#define VOLUME_ECHO_PIN     4   // Volume sonar echo
#define DAC_OUTPUT_PIN      17  // DAC output (GPIO17 or GPIO18)
```

### Audio Settings

```cpp
#define AUDIO_SAMPLE_RATE   40000   // 40kHz sample rate
#define MIN_FREQUENCY       110.0f  // A2 (lowest note)
#define MAX_FREQUENCY       880.0f  // A5 (highest note)
#define WAVEFORM_TYPE       0       // 0=Sine, 1=Triangle, 2=Sawtooth, 3=Square
```

### MIDI Settings

```cpp
#define MIDI_ENABLED        true    // Enable/disable MIDI
#define MIDI_CHANNEL        1       // MIDI channel (1-16)
#define MIDI_NOTE_MIN       45      // A2
#define MIDI_NOTE_MAX       81      // A5
#define PITCH_BEND_RANGE    2       // Semitones for pitch bend
```

### Sensor Settings

```cpp
#define MIN_DISTANCE_CM     5       // Minimum detection distance
#define MAX_DISTANCE_CM     100     // Maximum detection distance
#define DISTANCE_SMOOTHING  0.3f    // Smoothing factor (0-1)
```

## Usage

1. **Power on** the ESP32-S2 via USB
2. **Position sensors** facing upward or toward playing area
3. **Control pitch** by moving your hand over the pitch sensor
   - Closer = higher pitch
   - Further = lower pitch
4. **Control volume** by moving your hand over the volume sensor
   - Closer = louder
   - Further = quieter (mute when far away)

### USB MIDI

The device appears as a USB MIDI device when connected to a computer. You can:
- Use it with any DAW (Ableton, Logic, FL Studio, etc.)
- Control software synthesizers
- Record MIDI performances

### Audio Output

Connect the DAC output (GPIO17) to:
- An audio amplifier for speaker output
- High-impedance headphones directly (low volume)
- Line-level input (may need attenuation)

## Troubleshooting

### No audio output
- Check DAC_OUTPUT_PIN matches your wiring
- Verify the amplifier is powered and connected
- Enable DEBUG_ENABLED in config.h to see readings

### Erratic readings
- Ensure sensors are securely mounted
- Avoid reflective surfaces nearby
- Increase DISTANCE_SMOOTHING for more stability
- Check for interference between the two sensors

### USB MIDI not recognized
- Ensure USB Mode is set to "USB-OTG (TinyUSB)" in Arduino IDE
- Try a different USB cable (must support data)
- Restart the ESP32-S2 after connecting

### Compilation errors
- Ensure you have the latest ESP32 board package
- Select the correct board (ESP32-S2 variant)
- For PlatformIO: ensure `ARDUINO_USB_MODE=0` and `chegewara/ESP32TinyUSB` is in `lib_deps`

## Technical Details

### Audio Generation

The audio is generated using a timer interrupt at the configured sample rate (default 40kHz). The interrupt routine:
1. Calculates phase increment based on current frequency
2. Updates the phase accumulator
3. Looks up or calculates the waveform value
4. Applies volume scaling
5. Writes to the DAC

### Distance Measurement

Ultrasonic distance is calculated using the formula:
```
distance (cm) = echo_duration (μs) × 0.01715
```
Where 0.01715 = speed of sound (343 m/s) / 2 / 10000

### MIDI Note Calculation

Frequency to MIDI note conversion:
```
MIDI note = 69 + 12 × log2(frequency / 440)
```
Fractional parts are converted to pitch bend values for smooth glissando effects.

## License

MIT License - See LICENSE file for details

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.
