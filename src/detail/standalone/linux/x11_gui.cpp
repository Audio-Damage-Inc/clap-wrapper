

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

#include "x11_gui.h"
#include "detail/standalone/standalone_window_hints.h"
#include <sys/epoll.h>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
void X11Gui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *sah)
{
  XInitThreads();
  display = XOpenDisplay(nullptr);
  if (!display)
  {
    const char *disp = getenv("DISPLAY");
    fprintf(stderr, "XOpenDisplay failed: could not connect to display '%s'\n",
            disp ? disp : "(DISPLAY not set)");
    exit(1);
  }
  sah->x11Gui = this;
  host = sah;
  font.load(display);
  theme = Theme::make(display, sah->screenAppearance == CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK);

  // shown in the strip, and in the settings window while it is open
  sah->displayAudioError = [this](const std::string &message)
  {
    fprintf(stderr, "Audio: %s\n", message.c_str());
    lastError = message;
    if (settings && !settings->closed()) settings->showError(message);
    updateStatus();
  };
  // one opener for the strip's button and the plugin's request
  sah->onShowSettingsScreen = [this]()
  {
    openSettings();
    return true;
  };
  sah->onAudioDevicesChanged = [this]()
  {
    if (settings && !settings->closed()) settings->devicesChanged();
    updateStatus();
  };
  sah->onScreenAppearanceChanged = [this]()
  {
    theme = Theme::make(display, host->screenAppearance == CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK);
    if (window) XSetWindowBackground(display, window, theme.strip);
    drawStrip();
    if (settings && !settings->closed()) settings->appearanceChanged();
  };
  sah->onRequestResize = [this](int w, int h) { return resetSizeTo(w, h); };
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
  {
    LOGINFO("Unable to create epoll");
  }
}

void X11Gui::runloop()
{
  if (!display || window == 0 || epoll_fd < 0)
  {
    // no window. Only way to kill us is with a signal.
    while (true)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      pollAudioEvents();
      serviceRequests();
    }
    return;
  }
  XEvent e;
  struct epoll_event events[maxEpollEvents];
  bool running{true};
  while (running)
  {
    // a settings window that closed is gone, and the plugin hears of it now, on a turn of
    // its own and never from inside one of its calls
    if (settingsClosed)
    {
      const auto result{*settingsClosed};
      settingsClosed.reset();
      host->settingsScreenClosed(result);
    }

    while (XPending(display))
    {
      XNextEvent(display, &e);
      if (settings && settings->owns(e.xany.window))
      {
        settings->handle(e);
        continue;
      }
      if (e.xany.window == window)
      {
        switch (e.type)
        {
          case MapNotify:
          {
            // placed: the size hints stop saying where
            placing = false;
            if (plugin && plugin->_ext._gui && pluginWindow)
            {
              clap_window win;
              win.api = CLAP_WINDOW_API_X11;
              win.x11 = pluginWindow;
              auto ui = plugin->_ext._gui;
              ui->set_parent(plugin->_plugin, &win);
              ui->show(plugin->_plugin);
            }
          }
          break;
          case ConfigureNotify:
            windowResized(e.xconfigure.width, e.xconfigure.height);
            break;
          case ClientMessage:
          {
            // 3. Check if the message is the delete request
            if ((Atom)(e.xclient.data.l[0]) == wmDeleteMessage)
            {
              running = false;
            }
            break;
          }
          default:
            handleStrip(e);
            break;
        }
      }
    }

    if (settings && settings->closed())
    {
      settingsClosed = settings->result();
      settings.reset();
    }

    if (setSizePending)
    {
      setSizePending = false;
      tellPluginSize();
    }

    // Poll for both X11 events and timer events
    int num{0};
    if (running) num = epoll_wait(epoll_fd, events, maxEpollEvents, 50);
    pollAudioEvents();
    serviceRequests();

    if (num > 0)
    {
      for (int i = 0; i < num; ++i)
      {
        auto fd = events[i].data.fd;
        auto tfd = fdToTimerId.find(fd);
        auto pfd = registeredFds.find(fd);

        if (tfd != fdToTimerId.end())
        {
          if (plugin->_ext._timer)
          {
            plugin->_ext._timer->on_timer(plugin->_plugin, tfd->second);
          }
        }
        if (pfd != registeredFds.end())
        {
          if (plugin->_ext._posixfd)
          {
            plugin->_ext._posixfd->on_fd(plugin->_plugin, fd, pfd->second);
          }
        }
      }
    }
  }

  // a settings window still open is cancelled with the standalone
  if (settings)
  {
    if (!settings->closed())
    {
      XEvent close{};
      close.xclient.type = ClientMessage;
      close.xclient.data.l[0] = static_cast<long>(XInternAtom(display, "WM_DELETE_WINDOW", False));
      settings->handle(close);
    }
    settings.reset();
  }
}

// the audio backend's events - a stream that stopped, devices that changed - acted on
// here, on the main thread, every 50 ms
void X11Gui::pollAudioEvents()
{
  const auto now{std::chrono::steady_clock::now()};
  if (!host || now < nextAudioPoll) return;
  nextAudioPoll = now + std::chrono::milliseconds(50);
  host->pollAudioEvents();
  if (settings && !settings->closed()) settings->tick();
  updateStatus();
}

// request_callback, request_restart and request_process, flags the plugin may set from
// any thread, acted on at the loop's next turn, as the macOS and Windows timers do. Never
// done, a plugin waiting on on_main_thread for work a thread of its own has finished
// waits for good
void X11Gui::serviceRequests()
{
  if (!host || !plugin) return;
  auto *pl = plugin->_plugin;

  if (host->callbackRequested.exchange(false)) pl->on_main_thread(pl);

  // an inactive plugin has nothing to restart. activatePlugin parks the callback,
  // reactivates, and lets processing resume, or leaves it stopped if the plugin refuses
  if (host->restartRequested.exchange(false) && host->isActive)
    host->activatePlugin(host->currentSampleRate, 1, host->currentBufferSize * 2);

  // the standalone owns its audio engine, so a wake-up stands an inactive plugin up; an
  // active one is already being pulled by the callback. Never before the engine has run
  if (host->processRequested.exchange(false) && !host->isActive && host->currentSampleRate > 0)
    host->activatePlugin(host->currentSampleRate, 1, host->currentBufferSize * 2);
}

void X11Gui::openSettings()
{
  if (!display || !host) return;
  // a second request while the window is open brings it to the front
  if (settings && !settings->closed())
  {
    settings->raise();
    return;
  }
  settings = std::make_unique<SettingsWindow>(display, host, font, window);
}

void X11Gui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  this->plugin = p;
  if (!display) return;

  pluginWidth = pluginHeight = 0;
  bool hasGui{false};
  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto pl = plugin->_plugin;
    if (!ui->is_api_supported(pl, CLAP_WINDOW_API_X11, false))
    {
      LOGINFO("[ERROR] CLAP does not support X11");
    }
    else
    {
      // This window is X11 and the plugin lives in the main program, where visage would
      // otherwise size itself from Wayland before it is handed its X11 parent.
      setenv("VISAGE_WINDOWING", "x11", 0);
      ui->create(pl, CLAP_WINDOW_API_X11, false);
      uint32_t w, h;
      ui->get_size(pl, &w, &h);
      ui->adjust_size(pl, &w, &h);
      pluginWidth = static_cast<int>(w);
      pluginHeight = static_cast<int>(h);
      hasGui = true;
      pluginCanResize = ui->can_resize(pl);
    }
  }

  // the window is there with or without a gui: the strip has the settings, and closing
  // the window quits as it does with one
  showStrip = !hideStrip || !hasGui;

  // it opens in the middle of the screen it opens on, and no bigger than that screen: a
  // plugin that would not fit is given the largest size that does
  const Rect area{openingArea()};
  if (hasGui) fitPluginInto(area.w, area.h - stripShown());
  const int windowW{widthFor(pluginWidth)}, windowH{pluginHeight + stripShown()};
  placeX = area.x + std::max(0, (area.w - windowW) / 2);
  placeY = area.y + std::max(0, (area.h - windowH) / 2);
  placing = true;

  int s = DefaultScreen(display);
  window = XCreateSimpleWindow(display, RootWindow(display, s), placeX, placeY,
                               static_cast<unsigned>(windowW), static_cast<unsigned>(windowH), 0,
                               theme.strip, theme.strip);
  XStoreName(display, window, plugin->_plugin->desc->name);
  XSelectInput(display, window,
               StructureNotifyMask | ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | LeaveWindowMask);
  gc = XCreateGC(display, window, 0, nullptr);
  if (hasGui)
  {
    // the gui's background where the build or the plugin's entry names one (windowHints()):
    // what the window shows until the gui has drawn, rather than black
    unsigned long background{BlackPixel(display, s)};
    if (const auto &hints{windowHints()}; hints.hasBackground)
    {
      XColor c{};
      c.red = static_cast<unsigned short>(((hints.background >> 16) & 0xFF) * 257);
      c.green = static_cast<unsigned short>(((hints.background >> 8) & 0xFF) * 257);
      c.blue = static_cast<unsigned short>((hints.background & 0xFF) * 257);
      c.flags = DoRed | DoGreen | DoBlue;
      if (XAllocColor(display, DefaultColormap(display, s), &c)) background = c.pixel;
    }
    pluginWindow =
        XCreateSimpleWindow(display, window, 0, stripShown(), static_cast<unsigned>(pluginWidth),
                            static_cast<unsigned>(pluginHeight), 0, background, background);
    XMapWindow(display, pluginWindow);
  }

  // Get window clsoed notifications
  wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDeleteMessage, 1);

  layoutWindow();
  updateStatus();

  XMapWindow(display, window);

  epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = ConnectionNumber(display);
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ConnectionNumber(display), &event) == -1)
  {
    LOGINFO("Unable to register display epoll");
    close(epoll_fd);
    epoll_fd = -1;
    return;
  }
}

namespace
{
const std::string restoreLabel{"Restore last session"};
constexpr int restoreCheckSize{13};
}  // namespace

// the window's size from the plugin's: the strip above it, at least wide enough for the
// strip's text and button, or the plugin's own where there is no strip
//
// held at that size to the window manager, which is what has a tiling one (Hyprland,
// sway, i3) float the window rather than tile it. A plugin that can be resized resizes
// it from inside, with request_resize. The hints go first, so that a manager holding the
// window to its last size grants the next one
void X11Gui::layoutWindow()
{
  if (!display || window == 0) return;
  const int w{widthFor(pluginWidth)}, h{pluginHeight + stripShown()};

  XSizeHints *hints = XAllocSizeHints();
  hints->flags = PMinSize | PMaxSize;
  hints->min_width = hints->max_width = w;
  hints->min_height = hints->max_height = h;
  if (placing)
  {
    hints->flags |= USPosition | PPosition;
    hints->x = placeX;
    hints->y = placeY;
  }
  XSetWMNormalHints(display, window, hints);
  XFree(hints);

  XResizeWindow(display, window, static_cast<unsigned>(w), static_cast<unsigned>(h));
  if (pluginWindow)
    XResizeWindow(display, pluginWindow, static_cast<unsigned>(std::max(pluginWidth, 1)),
                  static_cast<unsigned>(std::max(pluginHeight, 1)));

  layoutStrip(w);
}

// the room a window opening now has: the monitor the pointer is on, or failing that the
// primary one, as RandR has them, and of that the desktop's work area where the window
// manager publishes one (Hyprland's does not, and there it is the monitor). The whole X
// screen where there is no RandR
Rect X11Gui::openingArea() const
{
  const int s{DefaultScreen(display)};
  const ::Window root{RootWindow(display, s)};
  Rect area{0, 0, DisplayWidth(display, s), DisplayHeight(display, s)};

  int eventBase{0}, errorBase{0};
  if (XRRQueryExtension(display, &eventBase, &errorBase))
  {
    int count{0};
    if (XRRMonitorInfo *monitors{XRRGetMonitors(display, root, True, &count)})
    {
      ::Window rootReturn{0}, childReturn{0};
      int pointerX{0}, pointerY{0}, windowX{0}, windowY{0};
      unsigned int mask{0};
      const bool pointer{XQueryPointer(display, root, &rootReturn, &childReturn, &pointerX,
                                       &pointerY, &windowX, &windowY, &mask) == True};
      int chosen{-1};
      for (int i = 0; i < count && chosen < 0; ++i)
      {
        const Rect m{monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height};
        if (pointer && m.contains(pointerX, pointerY)) chosen = i;
      }
      for (int i = 0; i < count && chosen < 0; ++i)
        if (monitors[i].primary) chosen = i;
      if (count > 0)
      {
        const auto &m{monitors[std::max(chosen, 0)]};
        if (m.width > 0 && m.height > 0) area = {m.x, m.y, m.width, m.height};
      }
      XRRFreeMonitors(monitors);
    }
  }

  // a CARDINAL[] property of the root window, from offset on, as many as there are
  auto cardinals = [this, root](const char *name, long offset, long count)
  {
    std::vector<long> values;
    const Atom atom{XInternAtom(display, name, True)};
    if (atom == None) return values;
    Atom type{None};
    int format{0};
    unsigned long items{0}, after{0};
    unsigned char *data{nullptr};
    if (XGetWindowProperty(display, root, atom, offset, count, False, XA_CARDINAL, &type, &format,
                           &items, &after, &data) == Success &&
        data)
    {
      // 32-bit items come back as longs
      if (type == XA_CARDINAL && format == 32)
      {
        const auto *v{reinterpret_cast<const long *>(data)};
        values.assign(v, v + items);
      }
      XFree(data);
    }
    return values;
  };

  // x, y, width and height for each desktop: the current desktop's, where it is there
  const auto desktop{cardinals("_NET_CURRENT_DESKTOP", 0, 1)};
  const auto work{cardinals("_NET_WORKAREA", desktop.empty() ? 0 : desktop[0] * 4, 4)};
  if (work.size() == 4)
  {
    const int left{std::max(area.x, static_cast<int>(work[0]))};
    const int top{std::max(area.y, static_cast<int>(work[1]))};
    const int right{std::min(area.x + area.w, static_cast<int>(work[0] + work[2]))};
    const int bottom{std::min(area.y + area.h, static_cast<int>(work[1] + work[3]))};
    if (right > left && bottom > top) area = {left, top, right - left, bottom - top};
  }
  return area;
}

// a resizable plugin bigger than the room it has is asked for the largest size that fits:
// a box in its own proportions, its aspect hint's where it gives one, put through
// adjust_size, and smaller again for as long as the answer does not fit, since a plugin
// may keep its proportions from the longer side and answer bigger than it was asked. It
// hears the size with set_size, before it has a parent, as the gui extension allows
void X11Gui::fitPluginInto(int maxWidth, int maxHeight)
{
  if (!pluginCanResize || !plugin || !plugin->_ext._gui) return;
  if (maxWidth < 1 || maxHeight < 1 || pluginWidth < 1 || pluginHeight < 1) return;
  if (pluginWidth <= maxWidth && pluginHeight <= maxHeight) return;

  auto ui = plugin->_ext._gui;
  auto pl = plugin->_plugin;
  double aspect{static_cast<double>(pluginWidth) / pluginHeight};
  clap_gui_resize_hints_t resizeHints{};
  if (ui->get_resize_hints && ui->get_resize_hints(pl, &resizeHints) &&
      resizeHints.preserve_aspect_ratio && resizeHints.aspect_ratio_width > 0 &&
      resizeHints.aspect_ratio_height > 0)
    aspect = static_cast<double>(resizeHints.aspect_ratio_width) / resizeHints.aspect_ratio_height;

  double width{std::min(static_cast<double>(maxWidth), maxHeight * aspect)};
  uint32_t w{0}, h{0};
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    w = static_cast<uint32_t>(std::max(1.0, std::floor(width)));
    h = static_cast<uint32_t>(std::max(1.0, std::floor(width / aspect)));
    ui->adjust_size(pl, &w, &h);
    if (static_cast<int>(w) <= maxWidth && static_cast<int>(h) <= maxHeight) break;
    // a plugin whose smallest size is bigger than the screen keeps that, the last answer
    const double over{std::max(static_cast<double>(w) / maxWidth,
                               static_cast<double>(h) / maxHeight)};
    width = width / over - 1.0;
  }

  pluginWidth = static_cast<int>(w);
  pluginHeight = static_cast<int>(h);
  if (ui->set_size(pl, w, h))
  {
    toldWidth = pluginWidth;
    toldHeight = pluginHeight;
  }
}

// the strip's button and checkbox, from the right edge of a window w wide
void X11Gui::layoutStrip(int w)
{
  stripWidth = w;
  const auto label{std::string{"Settings\xE2\x80\xA6"}};
  const int bw{std::max(90, font.width(label) + 24)};
  settingsButton = {w - bw - 6, 4, bw, stripHeight - 8};

  restoreBox = {};
  if (showStrip && host && host->settingsFolder.has_value())
  {
    const int rw{restoreCheckSize + 6 + font.width(restoreLabel)};
    restoreBox = {settingsButton.x - 12 - rw, 0, rw, stripHeight};
  }
  drawStrip();
}

// the window manager's size for the window, and the only place the window's own size is
// recorded: the plugin gets what is below the strip, of the width the strip leaves it and
// as far as its adjust_size agrees, and the window keeps the size it was given
//
// compared with the plugin's size, not with the size last asked for: a window manager may
// refuse a request_resize - one that keeps a maximised window at the screen's size - and
// then the plugin gets the window it has, not the one it asked for
void X11Gui::windowResized(int w, int h)
{
  if (w != windowWidth || h != windowHeight)
  {
    windowWidth = w;
    windowHeight = h;
    layoutStrip(w);
  }

  if (!pluginCanResize || !pluginWindow || !plugin || !plugin->_ext._gui) return;
  auto ui = plugin->_ext._gui;
  auto pl = plugin->_plugin;
  uint32_t pw{static_cast<uint32_t>(pluginWidthFor(w))};
  uint32_t ph{static_cast<uint32_t>(std::max(h - stripShown(), 1))};
  ui->adjust_size(pl, &pw, &ph);
  if (static_cast<int>(pw) == pluginWidth && static_cast<int>(ph) == pluginHeight) return;
  pluginWidth = static_cast<int>(pw);
  pluginHeight = static_cast<int>(ph);
  XResizeWindow(display, pluginWindow, pw, ph);
  tellPluginSize();
}

void X11Gui::tellPluginSize()
{
  if (!plugin || !plugin->_ext._gui || !pluginWindow) return;
  if (pluginWidth == toldWidth && pluginHeight == toldHeight) return;
  toldWidth = pluginWidth;
  toldHeight = pluginHeight;
  plugin->_ext._gui->set_size(plugin->_plugin, static_cast<uint32_t>(pluginWidth),
                              static_cast<uint32_t>(pluginHeight));
}

// what runs, in a line: "PipeWire · Built-in Audio · 48000 Hz · 256 frames", or the
// error that stopped it
void X11Gui::updateStatus()
{
  if (!host) return;
  std::string text;
  bool error{false};
  if (host->isAudioRunning())
  {
    lastError.clear();
    text = host->audioApiDisplayName;
    if (host->audioOutputUsed && !host->audioOutputDevice.empty())
    {
      const auto *device{host->findDevice(host->audioOutputDevice)};
      text += " \xC2\xB7 " + (device ? device->name : host->audioOutputDevice);
    }
    text += " \xC2\xB7 " + std::to_string(host->currentSampleRate) + " Hz \xC2\xB7 " +
            std::to_string(host->currentBufferSize) + " frames";
  }
  else if (!lastError.empty())
  {
    text = lastError;
    error = true;
  }
  else
  {
    text = "Audio is off";
  }
  // the plugin may have changed the choice the strip shows
  const bool restores{host->restoresLastSession()};
  if (text == status && error == statusIsError && restores == restoreShownOn) return;
  status = text;
  statusIsError = error;
  drawStrip();
}

void X11Gui::drawStrip()
{
  if (!display || window == 0 || !gc || !showStrip) return;
  const int w{std::max(stripWidth, minimumWidth)};
  auto pixmap{XCreatePixmap(display, window, static_cast<unsigned>(w), stripHeight,
                            static_cast<unsigned>(DefaultDepth(display, DefaultScreen(display))))};
  Painter p{display, pixmap, gc, &font};
  p.fill({0, 0, w, stripHeight}, theme.strip);
  p.line(0, stripHeight - 1, w, stripHeight - 1, theme.rule);
  const int statusRight{restoreBox.w > 0 ? restoreBox.x - 12 : settingsButton.x - 6};
  p.textIn({0, 0, statusRight, stripHeight}, 10, status,
           statusIsError ? theme.critical : theme.secondary);

  restoreShownOn = host && host->restoresLastSession();
  if (restoreBox.w > 0)
  {
    p.line(restoreBox.x - 6, 6, restoreBox.x - 6, stripHeight - 7, theme.rule);
    const Rect b{restoreBox.x, (stripHeight - restoreCheckSize) / 2, restoreCheckSize, restoreCheckSize};
    p.fill(b, restoreShownOn ? theme.accent : theme.field);
    p.frame(b, restoreShownOn ? theme.accent : theme.fieldBorder);
    if (restoreShownOn) p.tick(b, theme.accentText);
    p.textIn({b.x + restoreCheckSize + 6, 0, restoreBox.w - restoreCheckSize - 6, stripHeight}, 0,
             restoreLabel, theme.text);
  }

  const auto &b{settingsButton};
  p.fill(b, settingsButtonHot ? theme.hover : theme.field);
  p.frame(b, theme.fieldBorder);
  const auto label{std::string{"Settings\xE2\x80\xA6"}};
  p.text(b.x + (b.w - font.width(label)) / 2, b.y + (b.h - font.height()) / 2, label, theme.text);

  XCopyArea(display, pixmap, window, gc, 0, 0, static_cast<unsigned>(w), stripHeight, 0, 0);
  XFreePixmap(display, pixmap);
  XFlush(display);
}

void X11Gui::handleStrip(const XEvent &e)
{
  if (!showStrip) return;
  switch (e.type)
  {
    case Expose:
      if (e.xexpose.count == 0) drawStrip();
      break;
    case MotionNotify:
    {
      const bool hot{settingsButton.contains(e.xmotion.x, e.xmotion.y)};
      if (hot != settingsButtonHot)
      {
        settingsButtonHot = hot;
        drawStrip();
      }
      break;
    }
    case LeaveNotify:
      if (settingsButtonHot)
      {
        settingsButtonHot = false;
        drawStrip();
      }
      break;
    case ButtonPress:
      settingsButtonPressed =
          e.xbutton.button == Button1 && settingsButton.contains(e.xbutton.x, e.xbutton.y);
      restorePressed = e.xbutton.button == Button1 && restoreBox.contains(e.xbutton.x, e.xbutton.y);
      break;
    case ButtonRelease:
      if (settingsButtonPressed && settingsButton.contains(e.xbutton.x, e.xbutton.y)) openSettings();
      settingsButtonPressed = false;
      // whether the next launch gets this session's state back; saved with the audio and
      // midi setup
      if (restorePressed && restoreBox.contains(e.xbutton.x, e.xbutton.y) && host)
      {
        host->setPluginStatePersistence(host->restoresLastSession()
                                            ? CLAP_WRAPPER_STANDALONE_STATE_PERSIST_NONE
                                            : CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION);
        drawStrip();
      }
      restorePressed = false;
      break;
    default:
      break;
  }
}

void X11Gui::saveSettings()
{
  if (!host) return;
  host->captureAudioSettings();
  if (!host->saveStandaloneSettings())
    LOGINFO("[WARNING] Unable to save the audio settings; the next launch will not have them");
}

void X11Gui::shutdown()
{
  settings.reset();
  if (host)
  {
    host->onShowSettingsScreen = nullptr;
    host->onAudioDevicesChanged = nullptr;
    host->onScreenAppearanceChanged = nullptr;
    host->displayAudioError = nullptr;
  }
  if (plugin && plugin->_ext._gui && pluginWindow)
  {
    plugin->_ext._gui->destroy(plugin->_plugin);
  }
  if (epoll_fd >= 0)
  {
    close(epoll_fd);
    epoll_fd = -1;
  }
  if (gc) XFreeGC(display, gc);
  gc = nullptr;
  if (display && window > 0)
  {
    XDestroyWindow(display, window);
    XFlush(display);
  }
  if (display)
  {
    font.unload(display);
    XCloseDisplay(display);
  }
  plugin.reset();
}

int X11Gui::nextTimerId{2112};

bool X11Gui::register_timer(int period_ms, clap_id *tid)
{
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  struct itimerspec ts;
  memset(&ts, 0, sizeof(ts));
  ts.it_interval.tv_nsec = period_ms * 1e6;  // Repeat every 1s
  ts.it_value.tv_nsec = period_ms * 1e6;     // First expiry in 1s
  timerfd_settime(tfd, 0, &ts, NULL);

  epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = tfd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &event) == -1)
  {
    LOGINFO("Unable to register timer epoll");
    return false;
  }

  auto id = nextTimerId;
  nextTimerId++;
  *tid = id;

  timerIdToFd[id] = tfd;
  fdToTimerId[tfd] = id;

  return true;
}
bool X11Gui::unregister_timer(clap_id tid)
{
  LOGINFO("Unregistering timer: {}", tid);
  if (epoll_fd < 0)
  {
    return false;
  }
  auto idF = timerIdToFd.find(tid);
  if (idF == timerIdToFd.end())
  {
    return false;
  }
  auto fd = idF->second;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
  {
    LOGINFO("epoll_ctl EPOLL_CTL_DEL failed to unregister timer");
    // Handle error
    return false;
  }
  close(fd);  // thereby stopping the timer
  timerIdToFd.erase(tid);
  fdToTimerId.erase(fd);
  return true;
}

bool X11Gui::register_fd(int fd, clap_posix_fd_flags_t iflags)
{
  int flags{0};
  if (iflags & CLAP_POSIX_FD_READ) flags = flags | EPOLLIN;
  if (iflags & CLAP_POSIX_FD_WRITE) flags = flags | EPOLLOUT;
  if (iflags & CLAP_POSIX_FD_ERROR) flags = flags | EPOLLERR;

  epoll_event event;
  event.events = flags;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1)
  {
    LOGINFO("Unable to register plugin provided fd");
    return false;
  }
  registeredFds[fd] = iflags;
  return true;
}

bool X11Gui::unregister_fd(int fd)
{
  LOGINFO("Unregistering FD: {}", fd);
  if (epoll_fd < 0) return false;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
  {
    LOGINFO("epoll_ctl EPOLL_CTL_DEL failed");
    // Handle error
    return false;
  }

  registeredFds.erase(fd);
  return true;
}

bool X11Gui::resetSizeTo(int w, int h)
{
  if (!display || window == 0 || !pluginWindow) return false;
  pluginWidth = w;
  pluginHeight = h;
  layoutWindow();
  setSizePending = true;
  return true;
}
};  // namespace freeaudio::clap_wrapper::standalone::linux_standalone