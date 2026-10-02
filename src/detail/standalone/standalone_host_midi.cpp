#include "standalone_host.h"
#include "standalone_details.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace freeaudio::clap_wrapper::standalone
{
// Audio Damage addition: the port names, for a hosted plugin's settings page.
// A fresh RtMidiIn each time -- the list is what is plugged in NOW, and the
// open inputs below are not it.
std::vector<std::string> StandaloneHost::getMidiInputNames()
{
  std::vector<std::string> names;
  try
  {
    auto midiIn = std::make_unique<RtMidiIn>();
    const unsigned int count = midiIn->getPortCount();
    for (unsigned int i = 0; i < count; ++i) names.push_back(midiIn->getPortName(i));
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
  }
  return names;
}

void StandaloneHost::startMIDIThread()
{
  try
  {
    LOGINFO("Initializing Midi");
    auto midiIn = std::make_unique<RtMidiIn>();
    numMidiPorts = midiIn->getPortCount();
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
    exit(EXIT_FAILURE);
  }

  // selectedMidiPort is -1 for every port, which is the standalone's own
  // default; a settings page may have narrowed it to one. A saved port that
  // is no longer there binds everything rather than nothing -- silence is
  // indistinguishable from a broken build.
  const bool one = selectedMidiPort >= 0 && selectedMidiPort < static_cast<int>(numMidiPorts);

  LOGDETAIL("MIDI: There are {} MIDI input sources available. Binding {}.", numMidiPorts,
            one ? "one" : "all");
  for (unsigned int i = 0; i < numMidiPorts; i++)
  {
    if (one && static_cast<int>(i) != selectedMidiPort) continue;

    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOGDETAIL("  - '{}'", midiIn->getPortName(i));
      midiIn->openPort(i);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
    }
    catch (RtMidiError &error)
    {
      error.printMessage();
    }
  }
}

void StandaloneHost::restartMIDIThread()
{
  stopMIDIThread();
  midiIns.clear();
  startMIDIThread();
}

void StandaloneHost::processMIDIEvents(double deltatime, std::vector<unsigned char> *message)
{
  auto nBytes = message->size();

  if (nBytes <= 3)
  {
    midiChunk ck;
    memset(ck.dat, 0, sizeof(ck.dat));
    memcpy(ck.dat, message->data(), nBytes);
    midiToAudioQueue.push(ck);
  }
}

void StandaloneHost::midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData)
{
  auto sh = (StandaloneHost *)userData;
  sh->processMIDIEvents(deltatime, message);
}

void StandaloneHost::stopMIDIThread()
{
  for (auto &m : midiIns)
  {
    m.reset();
  }
}

}  // namespace freeaudio::clap_wrapper::standalone
