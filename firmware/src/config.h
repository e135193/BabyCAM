#pragma once

// ---------------------------------------------------------------- network
// Example values -- replace with your own before flashing.
// The ESP32 is 2.4 GHz only: it cannot see a 5 GHz network at all.
#define WIFI_SSID      "lleo"
#define WIFI_PASSWORD  "MyPassw123"
#define HOSTNAME       "babycam"

// Enable mDNS (Bonjour/ZeroConf) so the board is reachable at http://babycam.local/
// without needing to know its IP. Requires ESP32 Arduino core 2.x.
#define USE_MDNS       1

// ---------------------------------------------------------------- persistence
// Store runtime settings (gain, lamp, chan, shift) in NVS so they survive reboot.
#define USE_NVS        1
#define NVS_NAMESPACE  "babycam"

// ---------------------------------------------------------------- microphone
// INMP441 on I2S1. I2S0 is not available: the camera driver uses it to clock
// the parallel DVP bus.
//
// All three pins belong to the microSD slot, which we simply never initialise.
// SD is useless for a live stream anyway.
//
// SD (data out of the mic) MUST NOT go to GPIO12. That pin is the MTDI strap
// and sets the flash voltage at reset; a microphone driving it high at
// power-up stops the board from booting. GPIO13 has no strapping function.
#define MIC_SCK_PIN    14
#define MIC_WS_PIN     15
#define MIC_SD_PIN     13
// Tie the module's L/R pad to GND -> the mic speaks in the left slot.

#define MIC_SAMPLE_RATE   16000   // plenty for a voice; halves the bandwidth
#define MIC_BLOCK          256    // samples per I2S read
#define MIC_DEFAULT_GAIN     2    // software gain, adjustable at runtime

// Which half of the I2S frame carries the data, and how far to shift the
// 32-bit slot down to 16 bits. Both were found by measurement, not by reading
// datasheets, and both differ from the obvious answer:
//
//   chan = 1 (ONLY_RIGHT) even though L/R is tied to GND. The ESP32 receiver
//   disagrees with the microphone's datasheet about which slot is "left";
//   ONLY_LEFT yields a flat zero, which is indistinguishable from an
//   unconnected microphone and cost an evening to find.
//
//   shift = 8. The 24-bit sample is NOT left-justified in the 32-bit slot as
//   the usual example code assumes; it sits in the low 24 bits. Shifting by 16
//   throws away three quarters of the signal and leaves it inaudibly quiet.
//
// Measured with these values: peak 30012/32767, RMS 17.4%, no clipping.
#define MIC_DEFAULT_CHAN     1
#define MIC_DEFAULT_SHIFT    8

// Ring capacity between the microphone task and the HTTP listener.
// ~0.25 s at 16 kHz mono 16-bit. Deliberately small: this buffer is a shock
// absorber, not a store. When it overflows the samples are DROPPED rather than
// queued, which is what keeps latency bounded on the device side - a growing
// queue would turn into permanent delay, and for a baby monitor stale audio is
// worse than a momentary gap.
#define AUDIO_RING_BYTES   8192

// ---------------------------------------------------------------- battery monitor
// ADC pin for battery voltage measurement (via voltage divider).
// ESP32-CAM: GPIO33 (ADC1_CH5) is typically free if microSD not used.
// ADC2 pins CANNOT be used when WiFi is active.
// Voltage divider: R1 (upper) + R2 (lower) to GND. ADC reads midpoint.
// Example: 200k + 100k -> 3x division, 4.2V -> 1.4V at ADC (safe for 11dB atten).
#define BAT_ADC_PIN        33          // GPIO33 = ADC1_CH5
#define BAT_ADC_ATTEN      ADC_ATTEN_DB_11    // 0-3.3V range (actually ~0-2.6V calibrated)
#define BAT_DIVIDER_RATIO  3.0f        // (R1+R2)/R2, e.g. 200k+100k = 3.0
#define BAT_MIN_VOLTAGE    3.3f        // 0% (cutoff)
#define BAT_MAX_VOLTAGE    4.2f        // 100% (full)
#define BAT_SAMPLES        64          // averaging samples

// ---------------------------------------------------------------- camera
#define CAM_FRAME_SIZE   FRAMESIZE_VGA
#define CAM_JPEG_QUALITY 12        // lower value = better picture, more bytes