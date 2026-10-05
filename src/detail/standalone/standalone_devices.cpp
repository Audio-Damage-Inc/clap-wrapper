/*
 * Audio Damage addition. See standalone_devices.h.
 */

#include "standalone_devices.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtAudio.h"
#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "standalone_host.h"
#include "entry.h"

#include <algorithm>
#include <optional>

namespace freeaudio::clap_wrapper::standalone::devices
{
namespace
{
Startup gStartup;
bool gStartupSet{false};
std::string gLastError;

StandaloneHost *host()
{
  return getStandaloneHost();
}

// Reopen the stream on whatever is currently selected. Everything here goes
// through this: RtAudio has no way to change a running stream's device, rate
// or block, so each is a stop, an open and a start -- which is also what
// re-activates the plugin at the new rate.
void restartAudio()
{
  auto *sh = host();
  if (!sh) return;

  gLastError.clear();
  sh->displayAudioError = [](const std::string &text) { gLastError = text; };

  sh->startAudioThreadOn(sh->audioInputDeviceID, 2, false, sh->audioOutputDeviceID, 2,
                         sh->numAudioOutputs > 0, sh->currentSampleRate);
}

std::vector<RtAudio::DeviceInfo> outputDevices()
{
  auto *sh = host();
  if (!sh) return {};
  return sh->getOutputAudioDevices();
}

// The output device in force, or the first one when the id does not name
// anything (which is what a device unplugged under us looks like).
std::optional<RtAudio::DeviceInfo> currentOutput()
{
  auto *sh = host();
  if (!sh) return std::nullopt;

  for (const auto &info : outputDevices())
    if (info.ID == sh->audioOutputDeviceID) return info;

  return std::nullopt;
}
}  // namespace

Choices drivers()
{
  auto *sh = host();
  if (!sh) return {};

  Choices out;
  const auto apis = sh->getCompiledApi();
  for (auto api : apis)
  {
    // UNSPECIFIED is how RtAudio says "pick for me". It is the startup state,
    // not something to offer: a user choosing it would be choosing nothing.
    if (api == RtAudio::Api::UNSPECIFIED) continue;

    out.names.push_back(RtAudio::getApiDisplayName(api));
    if (api == sh->audioApi) out.selected = static_cast<int>(out.names.size()) - 1;
  }

  // Started on UNSPECIFIED and never asked to change: RtAudio resolved it to
  // one of the compiled APIs, and its name is what the dac reports.
  if (out.selected < 0)
  {
    for (size_t i = 0; i < out.names.size(); ++i)
      if (out.names[i] == sh->audioApiDisplayName) out.selected = static_cast<int>(i);
  }

  // One API is not a choice. macOS is always exactly this.
  if (out.names.size() < 2) return {};

  return out;
}

void setDriver(int index)
{
  auto *sh = host();
  if (!sh) return;

  const auto names = drivers().names;
  if (index < 0 || index >= static_cast<int>(names.size())) return;

  for (auto api : sh->getCompiledApi())
  {
    if (RtAudio::getApiDisplayName(api) != names[static_cast<size_t>(index)]) continue;

    sh->stopAudioThread();
    sh->setAudioApi(api);
    // The device ids belonged to the old API's enumeration. Start the new one
    // on its own default rather than on an id that means something else now.
    const auto [in, dflt, rate] = sh->getDefaultAudioInOutSampleRate();
    sh->audioOutputDeviceID = dflt;
    sh->currentSampleRate = rate;
    restartAudio();
    return;
  }
}

Choices outputs()
{
  auto *sh = host();
  if (!sh) return {};

  Choices out;
  for (const auto &info : outputDevices())
  {
    out.names.push_back(info.name);
    if (info.ID == sh->audioOutputDeviceID) out.selected = static_cast<int>(out.names.size()) - 1;
  }
  return out;
}

void setOutput(int index)
{
  auto *sh = host();
  if (!sh) return;

  const auto devices = outputDevices();
  if (index < 0 || index >= static_cast<int>(devices.size())) return;

  const auto &info = devices[static_cast<size_t>(index)];
  sh->audioOutputDeviceID = info.ID;
  sh->audioOutputUsed = true;

  // The rate lists belong to the device. Keep the current rate if the new one
  // can do it, else take its preferred -- which is what startAudioThreadOn
  // would do anyway, only this way the panel shows the right thing at once.
  const bool keeps = std::any_of(info.sampleRates.begin(), info.sampleRates.end(),
                                 [sh](auto sr) { return (int)sr == (int)sh->currentSampleRate; });
  if (!keeps) sh->currentSampleRate = static_cast<int32_t>(info.preferredSampleRate);

  restartAudio();
}

Choices sampleRates()
{
  auto *sh = host();
  if (!sh) return {};

  Choices out;
  const auto info = currentOutput();
  if (!info) return out;

  for (auto rate : info->sampleRates)
  {
    out.names.push_back(std::to_string(rate));
    if (static_cast<int>(rate) == static_cast<int>(sh->currentSampleRate))
      out.selected = static_cast<int>(out.names.size()) - 1;
  }
  return out;
}

void setSampleRate(int index)
{
  auto *sh = host();
  if (!sh) return;

  const auto info = currentOutput();
  if (!info || index < 0 || index >= static_cast<int>(info->sampleRates.size())) return;

  sh->currentSampleRate = static_cast<int32_t>(info->sampleRates[static_cast<size_t>(index)]);
  restartAudio();
}

Choices bufferSizes()
{
  auto *sh = host();
  if (!sh) return {};

  Choices out;
  for (auto size : sh->getBufferSizes())
  {
    out.names.push_back(std::to_string(size));
    if (size == sh->currentBufferSize) out.selected = static_cast<int>(out.names.size()) - 1;
  }
  return out;
}

void setBufferSize(int index)
{
  auto *sh = host();
  if (!sh) return;

  const auto sizes = sh->getBufferSizes();
  if (index < 0 || index >= static_cast<int>(sizes.size())) return;

  // openStream takes the block as an in/out parameter and writes back what it
  // actually opened, so this is a request. status() reports the answer.
  sh->currentBufferSize = sizes[static_cast<size_t>(index)];
  restartAudio();
}

Choices midiInputs()
{
  auto *sh = host();
  if (!sh) return {};

  Choices out;
  out.names.push_back("All");
  const auto ports = sh->getMidiPortNames();
  for (const auto &name : ports) out.names.push_back(name);

  // The host's own settings carry the selection, by name. "All" unless
  // exactly one port that is present is chosen.
  out.selected = 0;
  if (!sh->settings.midiBindAllPorts && sh->settings.midiPortNames.size() == 1)
  {
    for (size_t i = 0; i < ports.size(); ++i)
      if (ports[i] == sh->settings.midiPortNames.front()) out.selected = static_cast<int>(i) + 1;
  }
  return out;
}

void setMidiInput(int index)
{
  auto *sh = host();
  if (!sh) return;

  const auto ports = sh->getMidiPortNames();
  if (index <= 0 || index - 1 >= static_cast<int>(ports.size()))
  {
    sh->settings.midiBindAllPorts = true;
    sh->settings.midiPortNames.clear();
  }
  else
  {
    sh->settings.midiBindAllPorts = false;
    sh->settings.midiPortNames = {ports[static_cast<size_t>(index - 1)]};
  }
  sh->openMidiPorts(sh->settings.midiPortNames, sh->settings.midiBindAllPorts);
}

std::string status()
{
  auto *sh = host();
  if (!sh) return {};

  if (!gLastError.empty()) return gLastError;

  if (sh->currentSampleRate <= 0 || sh->currentBufferSize == 0) return "No audio device";

  return std::to_string(sh->currentSampleRate) + " Hz / " + std::to_string(sh->currentBufferSize) +
         " samples";
}

void setStartup(Startup startup)
{
  gStartup = std::move(startup);
  gStartupSet = true;
}

void applyStartup()
{
  auto *sh = host();
  if (!sh || !gStartupSet) return;

  sh->displayAudioError = [](const std::string &text) { gLastError = text; };

  // The plugin's saved choices become the standalone's own settings, which
  // startAudioThread and startMIDIThread read. Written through to the
  // standalone's settings file because startAudioThread loads that file.
  auto &st = sh->settings;
  st.audioApiName.clear();
  if (!gStartup.driver.empty())
  {
    for (auto api : sh->getCompiledApi())
      if (RtAudio::getApiDisplayName(api) == gStartup.driver) st.audioApiName = RtAudio::getApiName(api);
  }
  st.outputDeviceName = gStartup.output;
  st.inputDeviceName.clear();
  // The instrument has no audio input.
  st.audioInputUsed = false;
  st.audioOutputUsed = true;
  st.sampleRate = gStartup.sampleRate > 0 ? gStartup.sampleRate : 0;
  if (gStartup.bufferSize > 0) st.bufferSize = static_cast<uint32_t>(gStartup.bufferSize);

  if (!gStartup.midiInput.empty() && gStartup.midiInput != "All")
  {
    st.midiBindAllPorts = false;
    st.midiPortNames = {gStartup.midiInput};
  }
  else
  {
    st.midiBindAllPorts = true;
    st.midiPortNames.clear();
  }
  sh->saveStandaloneSettings();
}
}  // namespace freeaudio::clap_wrapper::standalone::devices
