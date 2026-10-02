#pragma once

/*
 * Audio Damage addition: the device settings, as the hosted plugin's own UI
 * needs to see them.
 *
 * The standalone owns RtAudio and RtMidi, and a clap-first plugin is compiled
 * once for every format -- so it cannot link anything in here, let alone
 * include RtAudio.h. This is the whole of what crosses that line: plain
 * strings and indices, one row per setting, which the plugin draws however it
 * likes and hands back an index from. Everything that knows what a device IS
 * stays on this side.
 */

#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone::devices
{
// One settings row: the choices, and which of them is in force. An EMPTY list
// means the row does not apply here -- a host with one compiled audio API has
// no driver to choose -- and a selected of -1 means nothing matched, which the
// plugin shows as its own "None".
struct Choices
{
  std::vector<std::string> names;
  int selected{-1};
};

// The compiled RtAudio APIs, by display name. One entry is not a choice; the
// plugin hides the row.
Choices drivers();
void setDriver(int index);

// Output devices on the current driver. Sandbender is an instrument, so the
// input side is deliberately absent.
Choices outputs();
void setOutput(int index);

// Rates the CURRENT OUTPUT device reports, as plain integers in text.
// StandaloneHost::getSampleRates() asks the input device, which an instrument
// does not open.
Choices sampleRates();
void setSampleRate(int index);

// The block sizes the standalone is willing to ask for. RtAudio has no way to
// enumerate what a device will accept -- it tells you only by succeeding or
// failing to open -- so this is a fixed ladder, and what the device actually
// gave us is in status().
Choices bufferSizes();
void setBufferSize(int index);

// Index 0 is "All", which is what the standalone has always done; the rest are
// ports in RtMidi's order.
Choices midiInputs();
void setMidiInput(int index);

// What the stream settled on, for a status line: the rate and block the device
// handed back, which is not always what was asked for, plus the error text if
// the last open failed.
std::string status();

// What to open at launch, BY NAME. Device ids are assigned per boot, so a
// saved id points at whatever lands in that slot next time; a name either
// matches something present or falls back to the default. Empty strings and
// zeroes mean "whatever the system prefers".
//
// Registered by the plugin before main -- the settings file is the plugin's,
// not ours -- and read by mainStartAudio.
struct Startup
{
  std::string driver;
  std::string output;
  std::string midiInput;  // "All", or a port name
  int sampleRate{0};
  int bufferSize{0};
};
void setStartup(Startup startup);

// Turns the names above into the ids RtAudio and RtMidi want, and leaves them
// on the host for startMIDIThread/startAudioThread to pick up. Called from
// mainStartAudio; a no-op when nothing was registered.
void applyStartup();
}  // namespace freeaudio::clap_wrapper::standalone::devices
