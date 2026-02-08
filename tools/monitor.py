#!/usr/bin/env python3
"""
ESP32-S2 Theremin Monitor

Connects to the theremin's serial port, switches to raw mode,
and displays a live dashboard with rolling graphs for distance,
frequency, and MIDI note.

Usage:
    python monitor.py [PORT] [BAUD]
    python monitor.py /dev/ttyUSB0 115200

Dependencies:
    pip install pyserial matplotlib
"""

import sys
import time
import threading
from collections import deque

import serial
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.gridspec import GridSpec

# --- Configuration ---
DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200
HISTORY_SIZE = 200  # number of data points to keep in graphs

# --- Data storage ---
timestamps = deque(maxlen=HISTORY_SIZE)
distances = deque(maxlen=HISTORY_SIZE)
frequencies = deque(maxlen=HISTORY_SIZE)
notes = deque(maxlen=HISTORY_SIZE)
volumes = deque(maxlen=HISTORY_SIZE)

# Latest values for status display
latest = {
    "distance": 0.0,
    "frequency": 0.0,
    "volume": 0,
    "note_num": 0,
    "note_name": "---",
    "uptime_ms": 0,
}

lock = threading.Lock()
running = True


def serial_reader(ser):
    """Background thread that reads and parses serial data."""
    global running

    # Switch to raw mode
    time.sleep(0.5)
    ser.write(b"raw\n")
    time.sleep(0.2)
    # Flush any initial output
    ser.reset_input_buffer()

    while running:
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("D,"):
                continue

            # D,timestamp_ms,distance_cm,frequency_hz,volume,note_number,note_name
            parts = line.split(",")
            if len(parts) < 7:
                continue

            ts = int(parts[1]) / 1000.0  # convert to seconds
            dist = float(parts[2])
            freq = float(parts[3])
            vol = int(parts[4])
            note_num = int(parts[5])
            note_name = parts[6]

            with lock:
                timestamps.append(ts)
                distances.append(dist)
                frequencies.append(freq)
                notes.append(note_num)
                volumes.append(vol)
                latest["distance"] = dist
                latest["frequency"] = freq
                latest["volume"] = vol
                latest["note_num"] = note_num
                latest["note_name"] = note_name
                latest["uptime_ms"] = int(parts[1])

        except (ValueError, IndexError):
            continue
        except serial.SerialException:
            print("Serial connection lost.")
            running = False
            break


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_to_label(n):
    """Convert MIDI note number to label like 'A4'."""
    if n < 0 or n > 127:
        return ""
    octave = (n // 12) - 1
    name = NOTE_NAMES[n % 12]
    return f"{name}{octave}"


def create_dashboard(port_name):
    """Create and run the matplotlib dashboard."""
    fig = plt.figure(figsize=(12, 8))
    fig.canvas.manager.set_window_title("ESP32-S2 Theremin Monitor")
    fig.patch.set_facecolor("#1e1e2e")

    gs = GridSpec(3, 2, figure=fig, hspace=0.4, wspace=0.3,
                  left=0.08, right=0.95, top=0.92, bottom=0.06)

    ax_dist = fig.add_subplot(gs[0, :])
    ax_freq = fig.add_subplot(gs[1, :])
    ax_note = fig.add_subplot(gs[2, 0])
    ax_info = fig.add_subplot(gs[2, 1])

    for ax in [ax_dist, ax_freq, ax_note]:
        ax.set_facecolor("#2a2a3e")
        ax.tick_params(colors="#aaaacc")
        ax.spines["bottom"].set_color("#555577")
        ax.spines["left"].set_color("#555577")
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    ax_info.set_facecolor("#2a2a3e")
    ax_info.set_xticks([])
    ax_info.set_yticks([])
    for spine in ax_info.spines.values():
        spine.set_visible(False)

    # Distance plot
    ax_dist.set_ylabel("Distance (cm)", color="#aaaacc")
    ax_dist.set_ylim(0, 110)
    line_dist, = ax_dist.plot([], [], color="#89b4fa", linewidth=1.5)
    ax_dist.set_title("Sonar Distance", color="#cdd6f4", fontsize=10)

    # Frequency plot
    ax_freq.set_ylabel("Frequency (Hz)", color="#aaaacc")
    ax_freq.set_ylim(0, 1000)
    line_freq, = ax_freq.plot([], [], color="#a6e3a1", linewidth=1.5)
    ax_freq.set_title("Audio Frequency", color="#cdd6f4", fontsize=10)

    # Note plot (as horizontal bars / piano roll style)
    ax_note.set_ylabel("MIDI Note", color="#aaaacc")
    ax_note.set_ylim(40, 85)
    ax_note.set_xlabel("Time (s)", color="#aaaacc")
    line_note, = ax_note.plot([], [], color="#f9e2af", linewidth=2, drawstyle="steps-post")
    ax_note.set_title("MIDI Note", color="#cdd6f4", fontsize=10)

    # Add note name labels on the y-axis for the note plot
    note_ticks = list(range(45, 82, 3))
    ax_note.set_yticks(note_ticks)
    ax_note.set_yticklabels([note_to_label(n) for n in note_ticks], fontsize=7)

    # Title
    fig.suptitle(f"ESP32-S2 Theremin  [{port_name}]",
                 color="#cdd6f4", fontsize=14, fontweight="bold")

    # Info text elements
    info_texts = {}

    def update(_frame):
        with lock:
            if len(timestamps) < 2:
                return line_dist, line_freq, line_note

            ts = list(timestamps)
            t0 = ts[0]
            t_rel = [t - t0 for t in ts]

            line_dist.set_data(t_rel, list(distances))
            line_freq.set_data(t_rel, list(frequencies))
            line_note.set_data(t_rel, list(notes))

            for ax in [ax_dist, ax_freq, ax_note]:
                ax.set_xlim(t_rel[0], max(t_rel[-1], t_rel[0] + 1))

            # Auto-scale frequency
            f_list = list(frequencies)
            if f_list:
                f_min = max(0, min(f_list) - 50)
                f_max = max(f_list) + 50
                ax_freq.set_ylim(f_min, f_max)

            # Update info panel
            ax_info.clear()
            ax_info.set_facecolor("#2a2a3e")
            ax_info.set_xticks([])
            ax_info.set_yticks([])
            for spine in ax_info.spines.values():
                spine.set_visible(False)

            info_lines = [
                f"Distance:  {latest['distance']:7.1f} cm",
                f"Frequency: {latest['frequency']:7.1f} Hz",
                f"Note:      {latest['note_name']:>4s} (MIDI {latest['note_num']})",
                f"Volume:    {latest['volume']:>3d}/255",
                f"Uptime:    {latest['uptime_ms'] // 1000}s",
            ]
            text = "\n".join(info_lines)
            ax_info.text(0.1, 0.5, text, transform=ax_info.transAxes,
                         fontsize=12, fontfamily="monospace",
                         color="#cdd6f4", verticalalignment="center")

        return line_dist, line_freq, line_note

    ani = animation.FuncAnimation(fig, update, interval=100, blit=False, cache_frame_data=False)
    plt.show()


def main():
    global running

    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_BAUD

    print(f"Connecting to {port} at {baud} baud...")
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error: {e}")
        print(f"\nUsage: {sys.argv[0]} [PORT] [BAUD]")
        print(f"Example: {sys.argv[0]} /dev/ttyUSB0 115200")
        sys.exit(1)

    print(f"Connected. Switching to raw mode...")

    reader_thread = threading.Thread(target=serial_reader, args=(ser,), daemon=True)
    reader_thread.start()

    try:
        create_dashboard(port)
    except KeyboardInterrupt:
        pass
    finally:
        running = False
        # Switch back to monitor mode before closing
        try:
            ser.write(b"monitor\n")
            time.sleep(0.1)
        except serial.SerialException:
            pass
        ser.close()
        print("\nDisconnected.")


if __name__ == "__main__":
    main()
