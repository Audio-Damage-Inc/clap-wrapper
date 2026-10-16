#pragma once

/*
 * Audio Damage addition: the iOS host app's audio and MIDI settings, as the
 * hosted plugin's own UI needs to see them. standalone_devices.h is the same
 * idea for the desktop standalone, and deliberately the same shape -- a
 * plugin can draw one settings page against either.
 *
 * iOS is not the desktop and the rows say so:
 *
 *  - There is no output DEVICE to choose. The route is the system's, set in
 *    Control Centre or by what is plugged in, so route() is a label and not
 *    a picker.
 *  - Rate and block are REQUESTS. AVAudioSession takes a preferred value and
 *    gives back whatever the hardware and the rest of the system agree on,
 *    which is why both lists are fixed ladders and the selection is matched
 *    against what the session actually reports.
 *  - There is no driver row at all.
 */

#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone::ios_devices
{
// Same as the desktop facade's: the choices, and which is in force. An empty
// list means the row does not apply.
struct Choices
{
  std::vector<std::string> names;
  int selected{-1};
};

// Rates worth asking a phone or a tablet for, selected against the session's
// current one. A refused request simply leaves the selection where it was.
Choices sampleRates();
void setSampleRate(int index);

// I/O buffer in frames. AVAudioSession takes a DURATION, so these are
// converted against the live rate going in and back out again -- which is why
// what comes back is a nearby value rather than the one asked for.
Choices bufferSizes();
void setBufferSize(int index);

// Index 0 is "All", which is what the host does by default; the rest are
// CoreMIDI sources by display name.
Choices midiInputs();
void setMidiInput(int index);

// The current output route -- "Speaker", "Headphones", a dock's name. Shown
// as text: nothing here can change it.
std::string route();

// What the session settled on, for a status line.
std::string status();

// Applied as the session is configured and the MIDI port opened, before any
// of the above can be asked for. Registered by the plugin before main, which
// is where the settings file lives.
struct Startup
{
  int sampleRate{0};  // 0 => whatever the system prefers
  int bufferSize{0};
  std::string midiInput;  // "All", or a source name
};
void setStartup(Startup startup);
// Read by the host app as it configures the session and opens the MIDI port.
const Startup &savedStartup();
}  // namespace freeaudio::clap_wrapper::standalone::ios_devices
