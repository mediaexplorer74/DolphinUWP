// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>

#include <future>

#include "Common/Version.h"

#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HotkeyManager.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/State.h"

#include "DiscIO/NANDImporter.h"

#include "DolphinQt2/AboutDialog.h"
#include "DolphinQt2/Config/ControllersWindow.h"
#include "DolphinQt2/Config/Graphics/GraphicsWindow.h"
#include "DolphinQt2/Config/LogConfigWidget.h"
#include "DolphinQt2/Config/LogWidget.h"
#include "DolphinQt2/Config/Mapping/MappingWindow.h"
#include "DolphinQt2/Config/SettingsWindow.h"
#include "DolphinQt2/FIFOPlayerWindow.h"
#include "DolphinQt2/Host.h"
#include "DolphinQt2/HotkeyScheduler.h"
#include "DolphinQt2/MainWindow.h"
#include "DolphinQt2/NetPlay/NetPlayDialog.h"
#include "DolphinQt2/NetPlay/NetPlaySetupDialog.h"
#include "DolphinQt2/QtUtils/QueueOnObject.h"
#include "DolphinQt2/QtUtils/RunOnObject.h"
#include "DolphinQt2/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt2/Resources.h"
#include "DolphinQt2/Settings.h"
#include "DolphinQt2/WiiUpdate.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "UICommon/UICommon.h"

#if defined(HAVE_XRANDR) && HAVE_XRANDR
#include <qpa/qplatformnativeinterface.h>
#include "UICommon/X11Utils.h"
#endif

MainWindow::MainWindow(std::unique_ptr<BootParameters> boot_parameters) : QMainWindow(nullptr)
{
  setWindowTitle(QString::fromStdString(Common::scm_rev_str));
  setWindowIcon(QIcon(Resources::GetMisc(Resources::LOGO_SMALL)));
  setUnifiedTitleAndToolBarOnMac(true);
  setAcceptDrops(true);

  InitControllers();

  CreateComponents();

  ConnectGameList();
  ConnectToolBar();
  ConnectRenderWidget();
  ConnectStack();
  ConnectMenuBar();

  InitCoreCallbacks();

  NetPlayInit();

  if (boot_parameters)
    StartGame(std::move(boot_parameters));
}

MainWindow::~MainWindow()
{
  m_render_widget->deleteLater();
  ShutdownControllers();
}

void MainWindow::InitControllers()
{
  if (g_controller_interface.IsInit())
    return;

  g_controller_interface.Initialize(reinterpret_cast<void*>(winId()));
  Pad::Initialize();
  Keyboard::Initialize();
  Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
  m_hotkey_scheduler = new HotkeyScheduler();
  m_hotkey_scheduler->Start();

  ConnectHotkeys();
}

void MainWindow::ShutdownControllers()
{
  m_hotkey_scheduler->Stop();

  g_controller_interface.Shutdown();
  Pad::Shutdown();
  Keyboard::Shutdown();
  Wiimote::Shutdown();
  HotkeyManagerEmu::Shutdown();

  m_hotkey_scheduler->deleteLater();
}

void MainWindow::InitCoreCallbacks()
{
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [=](Core::State state) {
    if (state == Core::State::Uninitialized)
      OnStopComplete();
  });
  installEventFilter(this);
  m_render_widget->installEventFilter(this);
}

static void InstallHotkeyFilter(QWidget* dialog)
{
  auto* filter = new WindowActivationEventFilter();
  dialog->installEventFilter(filter);

  filter->connect(filter, &WindowActivationEventFilter::windowDeactivated,
                  [] { HotkeyManagerEmu::Enable(true); });
  filter->connect(filter, &WindowActivationEventFilter::windowActivated,
                  [] { HotkeyManagerEmu::Enable(false); });
}

void MainWindow::CreateComponents()
{
  m_menu_bar = new MenuBar(this);
  m_tool_bar = new ToolBar(this);
  m_game_list = new GameList(this);
  m_render_widget = new RenderWidget;
  m_stack = new QStackedWidget(this);
  m_controllers_window = new ControllersWindow(this);
  m_settings_window = new SettingsWindow(this);

  m_hotkey_window = new MappingWindow(this, MappingWindow::Type::MAPPING_HOTKEYS, 0);

  m_log_widget = new LogWidget(this);
  m_log_config_widget = new LogConfigWidget(this);
  m_fifo_window = new FIFOPlayerWindow(this);

  connect(m_fifo_window, &FIFOPlayerWindow::LoadFIFORequested, this,
          static_cast<void (MainWindow::*)(const QString&)>(&MainWindow::StartGame));

#if defined(HAVE_XRANDR) && HAVE_XRANDR
  m_graphics_window = new GraphicsWindow(
      new X11Utils::XRRConfiguration(
          static_cast<Display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
              "display", windowHandle())),
          winId()),
      this);
#else
  m_graphics_window = new GraphicsWindow(nullptr, this);
#endif

  InstallHotkeyFilter(m_hotkey_window);
  InstallHotkeyFilter(m_controllers_window);
  InstallHotkeyFilter(m_settings_window);
  InstallHotkeyFilter(m_graphics_window);
}

void MainWindow::ConnectMenuBar()
{
  setMenuBar(m_menu_bar);
  // File
  connect(m_menu_bar, &MenuBar::Open, this, &MainWindow::Open);
  connect(m_menu_bar, &MenuBar::Exit, this, &MainWindow::close);

  // Emulation
  connect(m_menu_bar, &MenuBar::Pause, this, &MainWindow::Pause);
  connect(m_menu_bar, &MenuBar::Play, this, &MainWindow::Play);
  connect(m_menu_bar, &MenuBar::Stop, this, &MainWindow::RequestStop);
  connect(m_menu_bar, &MenuBar::Reset, this, &MainWindow::Reset);
  connect(m_menu_bar, &MenuBar::Fullscreen, this, &MainWindow::FullScreen);
  connect(m_menu_bar, &MenuBar::FrameAdvance, this, &MainWindow::FrameAdvance);
  connect(m_menu_bar, &MenuBar::Screenshot, this, &MainWindow::ScreenShot);
  connect(m_menu_bar, &MenuBar::StateLoad, this, &MainWindow::StateLoad);
  connect(m_menu_bar, &MenuBar::StateSave, this, &MainWindow::StateSave);
  connect(m_menu_bar, &MenuBar::StateLoadSlot, this, &MainWindow::StateLoadSlot);
  connect(m_menu_bar, &MenuBar::StateSaveSlot, this, &MainWindow::StateSaveSlot);
  connect(m_menu_bar, &MenuBar::StateLoadSlotAt, this, &MainWindow::StateLoadSlotAt);
  connect(m_menu_bar, &MenuBar::StateSaveSlotAt, this, &MainWindow::StateSaveSlotAt);
  connect(m_menu_bar, &MenuBar::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_menu_bar, &MenuBar::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_menu_bar, &MenuBar::StateSaveOldest, this, &MainWindow::StateSaveOldest);
  connect(m_menu_bar, &MenuBar::SetStateSlot, this, &MainWindow::SetStateSlot);

  // Options
  connect(m_menu_bar, &MenuBar::Configure, this, &MainWindow::ShowSettingsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureGraphics, this, &MainWindow::ShowGraphicsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureAudio, this, &MainWindow::ShowAudioWindow);
  connect(m_menu_bar, &MenuBar::ConfigureControllers, this, &MainWindow::ShowControllersWindow);
  connect(m_menu_bar, &MenuBar::ConfigureHotkeys, this, &MainWindow::ShowHotkeyDialog);

  // Tools
  connect(m_menu_bar, &MenuBar::BootGameCubeIPL, this, &MainWindow::OnBootGameCubeIPL);
  connect(m_menu_bar, &MenuBar::ImportNANDBackup, this, &MainWindow::OnImportNANDBackup);
  connect(m_menu_bar, &MenuBar::PerformOnlineUpdate, this, &MainWindow::PerformOnlineUpdate);
  connect(m_menu_bar, &MenuBar::BootWiiSystemMenu, this, &MainWindow::BootWiiSystemMenu);
  connect(m_menu_bar, &MenuBar::StartNetPlay, this, &MainWindow::ShowNetPlaySetupDialog);
  connect(m_menu_bar, &MenuBar::ShowFIFOPlayer, this, &MainWindow::ShowFIFOPlayer);

  // Movie
  connect(m_menu_bar, &MenuBar::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_menu_bar, &MenuBar::StartRecording, this, &MainWindow::OnStartRecording);
  connect(m_menu_bar, &MenuBar::StopRecording, this, &MainWindow::OnStopRecording);
  connect(m_menu_bar, &MenuBar::ExportRecording, this, &MainWindow::OnExportRecording);

  // View
  connect(m_menu_bar, &MenuBar::ShowList, m_game_list, &GameList::SetListView);
  connect(m_menu_bar, &MenuBar::ShowGrid, m_game_list, &GameList::SetGridView);
  connect(m_menu_bar, &MenuBar::ColumnVisibilityToggled, m_game_list,
          &GameList::OnColumnVisibilityToggled);

  connect(m_menu_bar, &MenuBar::GameListPlatformVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);
  connect(m_menu_bar, &MenuBar::GameListRegionVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);

  connect(m_menu_bar, &MenuBar::ShowAboutDialog, this, &MainWindow::ShowAboutDialog);

  connect(m_game_list, &GameList::SelectionChanged, m_menu_bar, &MenuBar::SelectionChanged);
  connect(this, &MainWindow::ReadOnlyModeChanged, m_menu_bar, &MenuBar::ReadOnlyModeChanged);
  connect(this, &MainWindow::RecordingStatusChanged, m_menu_bar, &MenuBar::RecordingStatusChanged);
}

void MainWindow::ConnectHotkeys()
{
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExitHotkey, this, &MainWindow::close);
  connect(m_hotkey_scheduler, &HotkeyScheduler::PauseHotkey, this, &MainWindow::Pause);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StopHotkey, this, &MainWindow::RequestStop);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ScreenShotHotkey, this, &MainWindow::ScreenShot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::FullScreenHotkey, this, &MainWindow::FullScreen);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlotHotkey, this,
          &MainWindow::StateLoadSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlotHotkey, this,
          &MainWindow::StateSaveSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetStateSlotHotkey, this,
          &MainWindow::SetStateSlot);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StartRecording, this,
          &MainWindow::OnStartRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExportRecording, this,
          &MainWindow::OnExportRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleReadOnlyMode, [this] {
    bool read_only = !Movie::IsReadOnly();
    Movie::SetReadOnly(read_only);
    emit ReadOnlyModeChanged(read_only);
  });
}

void MainWindow::ConnectToolBar()
{
  addToolBar(m_tool_bar);
  connect(m_tool_bar, &ToolBar::OpenPressed, this, &MainWindow::Open);
  connect(m_tool_bar, &ToolBar::PlayPressed, this, &MainWindow::Play);
  connect(m_tool_bar, &ToolBar::PausePressed, this, &MainWindow::Pause);
  connect(m_tool_bar, &ToolBar::StopPressed, this, &MainWindow::RequestStop);
  connect(m_tool_bar, &ToolBar::FullScreenPressed, this, &MainWindow::FullScreen);
  connect(m_tool_bar, &ToolBar::ScreenShotPressed, this, &MainWindow::ScreenShot);
  connect(m_tool_bar, &ToolBar::SettingsPressed, this, &MainWindow::ShowSettingsWindow);
  connect(m_tool_bar, &ToolBar::ControllersPressed, this, &MainWindow::ShowControllersWindow);
  connect(m_tool_bar, &ToolBar::GraphicsPressed, this, &MainWindow::ShowGraphicsWindow);
}

void MainWindow::ConnectGameList()
{
  connect(m_game_list, &GameList::GameSelected, this, &MainWindow::Play);
  connect(m_game_list, &GameList::NetPlayHost, this, &MainWindow::NetPlayHost);

  connect(m_game_list, &GameList::OpenGeneralSettings, this, &MainWindow::ShowGeneralWindow);
}

void MainWindow::ConnectRenderWidget()
{
  m_rendering_to_main = false;
  m_render_widget->hide();
  connect(m_render_widget, &RenderWidget::EscapePressed, this, &MainWindow::RequestStop);
  connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
}

void MainWindow::ConnectStack()
{
  m_stack->addWidget(m_game_list);

  setCentralWidget(m_stack);

  setTabPosition(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea, QTabWidget::North);
  addDockWidget(Qt::RightDockWidgetArea, m_log_widget);
  addDockWidget(Qt::RightDockWidgetArea, m_log_config_widget);

  tabifyDockWidget(m_log_widget, m_log_config_widget);
}

void MainWindow::Open()
{
  QString file = QFileDialog::getOpenFileName(
      this, tr("Select a File"), QDir::currentPath(),
      tr("All GC/Wii files (*.elf *.dol *.gcm *.iso *.tgc *.wbfs *.ciso *.gcz *.wad);;"
         "All Files (*)"));
  if (!file.isEmpty())
    StartGame(file);
}

void MainWindow::Play()
{
  // If we're in a paused game, start it up again.
  // Otherwise, play the selected game, if there is one.
  // Otherwise, play the default game.
  // Otherwise, play the last played game, if there is one.
  // Otherwise, prompt for a new game.
  if (Core::GetState() == Core::State::Paused)
  {
    Core::SetState(Core::State::Running);
  }
  else
  {
    QString selection = m_game_list->GetSelectedGame()->GetFilePath();
    if (selection.length() > 0)
    {
      StartGame(selection);
    }
    else
    {
      auto default_path = QString::fromStdString(SConfig::GetInstance().m_strDefaultISO);
      if (!default_path.isEmpty() && QFile::exists(default_path))
      {
        StartGame(default_path);
      }
      else
      {
        Open();
      }
    }
  }
}

void MainWindow::Pause()
{
  Core::SetState(Core::State::Paused);
}

void MainWindow::OnStopComplete()
{
  m_stop_requested = false;
  HideRenderWidget();

  if (m_exit_requested)
    QGuiApplication::instance()->quit();

  // If the current emulation prevented the booting of another, do that now
  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

bool MainWindow::RequestStop()
{
  if (!Core::IsRunning())
  {
    Core::QueueHostJob([this] { OnStopComplete(); }, true);
    return true;
  }

  if (SConfig::GetInstance().bConfirmStop)
  {
    const Core::State state = Core::GetState();

    // Only pause the game, if NetPlay is not running
    bool pause = Settings::Instance().GetNetPlayClient() != nullptr;

    if (pause)
      Core::SetState(Core::State::Paused);

    QMessageBox::StandardButton confirm;
    confirm = QMessageBox::question(m_render_widget, tr("Confirm"),
                                    m_stop_requested ?
                                        tr("A shutdown is already in progress. Unsaved data "
                                           "may be lost if you stop the current emulation "
                                           "before it completes. Force stop?") :
                                        tr("Do you want to stop the current emulation?"));

    if (pause)
      Core::SetState(state);

    if (confirm != QMessageBox::Yes)
      return false;
  }

  // TODO: Add Movie shutdown
  // TODO: Add Debugger shutdown

  if (!m_stop_requested && UICommon::TriggerSTMPowerEvent())
  {
    m_stop_requested = true;

    // Unpause because gracefully shutting down needs the game to actually request a shutdown.
    // TODO: Do not unpause in debug mode to allow debugging until the complete shutdown.
    if (Core::GetState() == Core::State::Paused)
      Core::SetState(Core::State::Running);

    return true;
  }

  ForceStop();
#ifdef Q_OS_WIN
  // Allow windows to idle or turn off display again
  SetThreadExecutionState(ES_CONTINUOUS);
#endif
  return true;
}

void MainWindow::ForceStop()
{
  BootManager::Stop();
}

void MainWindow::Reset()
{
  if (Movie::IsRecordingInput())
    Movie::SetReset(true);
  ProcessorInterface::ResetButton_Tap();
}

void MainWindow::FrameAdvance()
{
  Core::DoFrameStep();
}

void MainWindow::FullScreen()
{
  // If the render widget is fullscreen we want to reset it to whatever is in
  // settings. If it's set to be fullscreen then it just remakes the window,
  // which probably isn't ideal.
  bool was_fullscreen = m_render_widget->isFullScreen();
  HideRenderWidget();
  if (was_fullscreen)
    ShowRenderWidget();
  else
    m_render_widget->showFullScreen();
}

void MainWindow::ScreenShot()
{
  Core::SaveScreenShot();
}

void MainWindow::StartGame(const QString& path)
{
  StartGame(BootParameters::GenerateFromFile(path.toStdString()));
}

void MainWindow::StartGame(std::unique_ptr<BootParameters>&& parameters)
{
  // If we're running, only start a new game once we've stopped the last.
  if (Core::GetState() != Core::State::Uninitialized)
  {
    if (!RequestStop())
      return;

    // As long as the shutdown isn't complete, we can't boot, so let's boot later
    m_pending_boot = std::move(parameters);
    return;
  }
  // Boot up, show an error if it fails to load the game.
  if (!BootManager::BootCore(std::move(parameters)))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to init core"), QMessageBox::Ok);
    return;
  }
  ShowRenderWidget();

#ifdef Q_OS_WIN
  // Prevents Windows from sleeping, turning off the display, or idling
  EXECUTION_STATE shouldScreenSave =
      SConfig::GetInstance().bDisableScreenSaver ? ES_DISPLAY_REQUIRED : 0;
  SetThreadExecutionState(ES_CONTINUOUS | shouldScreenSave | ES_SYSTEM_REQUIRED);
#endif
}

void MainWindow::ShowRenderWidget()
{
  if (SConfig::GetInstance().bRenderToMain)
  {
    // If we're rendering to main, add it to the stack and update our title when necessary.
    m_rendering_to_main = true;
    m_stack->setCurrentIndex(m_stack->addWidget(m_render_widget));
    connect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
  }
  else
  {
    // Otherwise, just show it.
    m_rendering_to_main = false;
    if (SConfig::GetInstance().bFullscreen)
    {
      m_render_widget->showFullScreen();
    }
    else
    {
      m_render_widget->showNormal();
      m_render_widget->resize(640, 480);
    }
  }
}

void MainWindow::HideRenderWidget()
{
  if (m_rendering_to_main)
  {
    // Remove the widget from the stack and reparent it to nullptr, so that it can draw
    // itself in a new window if it wants. Disconnect the title updates.
    m_stack->removeWidget(m_render_widget);
    m_render_widget->setParent(nullptr);
    m_rendering_to_main = false;
    disconnect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
    setWindowTitle(QString::fromStdString(Common::scm_rev_str));
  }
  m_render_widget->hide();
}

void MainWindow::ShowControllersWindow()
{
  m_controllers_window->show();
  m_controllers_window->raise();
  m_controllers_window->activateWindow();
}

void MainWindow::ShowSettingsWindow()
{
  m_settings_window->show();
  m_settings_window->raise();
  m_settings_window->activateWindow();
}

void MainWindow::ShowAudioWindow()
{
  m_settings_window->SelectAudioPane();
  ShowSettingsWindow();
}

void MainWindow::ShowGeneralWindow()
{
  m_settings_window->SelectGeneralPane();
  ShowSettingsWindow();
}

void MainWindow::ShowAboutDialog()
{
  AboutDialog about{this};
  about.exec();
}

void MainWindow::ShowHotkeyDialog()
{
  m_hotkey_window->show();
  m_hotkey_window->raise();
  m_hotkey_window->activateWindow();
}

void MainWindow::ShowGraphicsWindow()
{
  m_graphics_window->show();
  m_graphics_window->raise();
  m_graphics_window->activateWindow();
}

void MainWindow::ShowNetPlaySetupDialog()
{
  m_netplay_setup_dialog->show();
  m_netplay_setup_dialog->raise();
  m_netplay_setup_dialog->activateWindow();
}

void MainWindow::ShowFIFOPlayer()
{
  m_fifo_window->show();
  m_fifo_window->raise();
  m_fifo_window->activateWindow();
}

void MainWindow::StateLoad()
{
  QString path = QFileDialog::getOpenFileName(this, tr("Select a File"), QDir::currentPath(),
                                              tr("All Save States (*.sav *.s##);; All Files (*)"));
  State::LoadAs(path.toStdString());
}

void MainWindow::StateSave()
{
  QString path = QFileDialog::getSaveFileName(this, tr("Select a File"), QDir::currentPath(),
                                              tr("All Save States (*.sav *.s##);; All Files (*)"));
  State::SaveAs(path.toStdString());
}

void MainWindow::StateLoadSlot()
{
  State::Load(m_state_slot);
}

void MainWindow::StateSaveSlot()
{
  State::Save(m_state_slot, true);
  m_menu_bar->UpdateStateSlotMenu();
}

void MainWindow::StateLoadSlotAt(int slot)
{
  State::Load(slot);
}

void MainWindow::StateSaveSlotAt(int slot)
{
  State::Save(slot, true);
  m_menu_bar->UpdateStateSlotMenu();
}

void MainWindow::StateLoadUndo()
{
  State::UndoLoadState();
}

void MainWindow::StateSaveUndo()
{
  State::UndoSaveState();
}

void MainWindow::StateSaveOldest()
{
  State::SaveFirstSaved();
}

void MainWindow::SetStateSlot(int slot)
{
  Settings::Instance().SetStateSlot(slot);
  m_state_slot = slot;
}

void MainWindow::PerformOnlineUpdate(const std::string& region)
{
  WiiUpdate::PerformOnlineUpdate(region, this);
  // Since the update may have installed a newer system menu, refresh the tools menu.
  m_menu_bar->UpdateToolsMenu(false);
}

void MainWindow::BootWiiSystemMenu()
{
  StartGame(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}));
}

void MainWindow::NetPlayInit()
{
  m_netplay_setup_dialog = new NetPlaySetupDialog(this);
  m_netplay_dialog = new NetPlayDialog(this);

  connect(m_netplay_dialog, &NetPlayDialog::Boot, this,
          static_cast<void (MainWindow::*)(const QString&)>(&MainWindow::StartGame));
  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, &MainWindow::RequestStop);
  connect(m_netplay_dialog, &NetPlayDialog::rejected, this, &MainWindow::NetPlayQuit);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Join, this, &MainWindow::NetPlayJoin);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Host, this, &MainWindow::NetPlayHost);
}

bool MainWindow::NetPlayJoin()
{
  if (Core::IsRunning())
  {
    QMessageBox::critical(
        nullptr, QObject::tr("Error"),
        QObject::tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  if (m_netplay_dialog->isVisible())
  {
    QMessageBox::critical(nullptr, QObject::tr("Error"),
                          QObject::tr("A NetPlay Session is already in progress!"));
    return false;
  }

  // Settings
  std::string host_ip;
  u16 host_port;
  if (Settings::Instance().GetNetPlayServer() != nullptr)
  {
    host_ip = "127.0.0.1";
    host_port = Settings::Instance().GetNetPlayServer()->GetPort();
  }
  else
  {
    host_ip = Config::Get(Config::NETPLAY_HOST_CODE);
    host_port = Config::Get(Config::NETPLAY_HOST_PORT);
  }

  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = traversal_choice == "traversal";

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);

  // Create Client
  Settings::Instance().ResetNetPlayClient(new NetPlayClient(
      host_ip, host_port, m_netplay_dialog, nickname,
      NetTraversalConfig{Settings::Instance().GetNetPlayServer() != nullptr ? false : is_traversal,
                         traversal_host, traversal_port}));

  if (!Settings::Instance().GetNetPlayClient()->IsConnected())
  {
    QMessageBox::critical(nullptr, QObject::tr("Error"),
                          QObject::tr("Failed to connect to server"));
    return false;
  }

  m_netplay_setup_dialog->close();
  m_netplay_dialog->show(nickname, is_traversal);

  return true;
}

bool MainWindow::NetPlayHost(const QString& game_id)
{
  if (Core::IsRunning())
  {
    QMessageBox::critical(
        nullptr, QObject::tr("Error"),
        QObject::tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  if (m_netplay_dialog->isVisible())
  {
    QMessageBox::critical(nullptr, QObject::tr("Error"),
                          QObject::tr("A NetPlay Session is already in progress!"));
    return false;
  }

  // Settings
  u16 host_port = Config::Get(Config::NETPLAY_HOST_PORT);
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = traversal_choice == "traversal";
  const bool use_upnp = Config::Get(Config::NETPLAY_USE_UPNP);

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);

  if (is_traversal)
    host_port = Config::Get(Config::NETPLAY_LISTEN_PORT);

  // Create Server
  Settings::Instance().ResetNetPlayServer(new NetPlayServer(
      host_port, use_upnp, NetTraversalConfig{is_traversal, traversal_host, traversal_port}));

  if (!Settings::Instance().GetNetPlayServer()->is_connected)
  {
    QMessageBox::critical(
        nullptr, QObject::tr("Failed to open server"),
        QObject::tr(
            "Failed to listen on port %1. Is another instance of the NetPlay server running?")
            .arg(host_port));
    return false;
  }

  Settings::Instance().GetNetPlayServer()->ChangeGame(game_id.toStdString());

  // Join our local server
  return NetPlayJoin();
}

void MainWindow::NetPlayQuit()
{
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
}

bool MainWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Close)
  {
    if (RequestStop() && object == this)
      m_exit_requested = true;

    static_cast<QCloseEvent*>(event)->ignore();
    return true;
  }

  return false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const auto& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  const auto& url = urls[0];
  QFileInfo file_info(url.toLocalFile());

  auto path = file_info.filePath();

  if (!file_info.exists() || !file_info.isReadable())
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
    return;
  }

  if (file_info.isFile())
  {
    StartGame(path);
  }
  else
  {
    auto& settings = Settings::Instance();

    if (settings.GetPaths().size() != 0)
    {
      if (QMessageBox::question(
              this, tr("Confirm"),
              tr("Do you want to add \"%1\" to the list of Game Paths?").arg(path)) !=
          QMessageBox::Yes)
        return;
    }
    settings.AddPath(path);
  }
}

QSize MainWindow::sizeHint() const
{
  return QSize(800, 600);
}

void MainWindow::OnBootGameCubeIPL(DiscIO::Region region)
{
  StartGame(std::make_unique<BootParameters>(BootParameters::IPL{region}));
}

void MainWindow::OnImportNANDBackup()
{
  auto response = QMessageBox::question(
      this, tr("Question"),
      tr("Merging a new NAND over your currently selected NAND will overwrite any channels "
         "and savegames that already exist. This process is not reversible, so it is "
         "recommended that you keep backups of both NANDs. Are you sure you want to "
         "continue?"));

  if (response == QMessageBox::No)
    return;

  QString file = QFileDialog::getOpenFileName(this, tr("Select the save file"), QDir::currentPath(),
                                              tr("BootMii NAND backup file (*.bin);;"
                                                 "All Files (*)"));

  if (file.isEmpty())
    return;

  QProgressDialog* dialog = new QProgressDialog(this);
  dialog->setMinimum(0);
  dialog->setMaximum(0);
  dialog->setLabelText(tr("Importing NAND backup"));
  dialog->setCancelButton(nullptr);

  auto beginning = QDateTime::currentDateTime().toMSecsSinceEpoch();

  auto result = std::async(std::launch::async, [&] {
    DiscIO::NANDImporter().ImportNANDBin(
        file.toStdString(),
        [&dialog, beginning] {
          QueueOnObject(dialog, [&dialog, beginning] {
            dialog->setLabelText(
                tr("Importing NAND backup\n Time elapsed: %1s")
                    .arg((QDateTime::currentDateTime().toMSecsSinceEpoch() - beginning) / 1000));
          });
        },
        [this] {
          return RunOnObject(this, [this] {
            return QFileDialog::getOpenFileName(this, tr("Select the keys file (OTP/SEEPROM dump)"),
                                                QDir::currentPath(),
                                                tr("BootMii keys file (*.bin);;"
                                                   "All Files (*)"))
                .toStdString();
          });
        });
    QueueOnObject(dialog, &QProgressDialog::close);
  });

  dialog->exec();

  result.wait();

  m_menu_bar->UpdateToolsMenu(Core::IsRunning());
}

void MainWindow::OnPlayRecording()
{
  QString dtm_file = QFileDialog::getOpenFileName(this, tr("Select the Recording File"), QString(),
                                                  tr("Dolphin TAS Movies (*.dtm)"));

  if (dtm_file.isEmpty())
    return;

  if (!Movie::IsReadOnly())
  {
    // let's make the read-only flag consistent at the start of a movie.
    Movie::SetReadOnly(true);
    emit ReadOnlyModeChanged(true);
  }

  if (Movie::PlayInput(dtm_file.toStdString()))
  {
    emit RecordingStatusChanged(true);

    Play();
  }
}

void MainWindow::OnStartRecording()
{
  if ((!Core::IsRunningAndStarted() && Core::IsRunning()) || Movie::IsRecordingInput() ||
      Movie::IsPlayingInput())
    return;

  if (Movie::IsReadOnly())
  {
    // The user just chose to record a movie, so that should take precedence
    Movie::SetReadOnly(false);
    emit ReadOnlyModeChanged(true);
  }

  int controllers = 0;

  for (int i = 0; i < 4; i++)
  {
    if (SerialInterface::SIDevice_IsGCController(SConfig::GetInstance().m_SIDevice[i]))
      controllers |= (1 << i);

    if (g_wiimote_sources[i] != WIIMOTE_SRC_NONE)
      controllers |= (1 << (i + 4));
  }

  if (Movie::BeginRecordingInput(controllers))
  {
    emit RecordingStatusChanged(true);

    if (!Core::IsRunning())
      Play();
  }
}

void MainWindow::OnStopRecording()
{
  if (Movie::IsRecordingInput())
    OnExportRecording();

  Movie::EndPlayInput(false);
  emit RecordingStatusChanged(true);
}

void MainWindow::OnExportRecording()
{
  bool was_paused = Core::GetState() == Core::State::Paused;

  if (was_paused)
    Core::SetState(Core::State::Paused);

  QString dtm_file = QFileDialog::getSaveFileName(this, tr("Select the Recording File"), QString(),
                                                  tr("Dolphin TAS Movies (*.dtm)"));

  if (was_paused)
    Core::SetState(Core::State::Running);

  if (dtm_file.isEmpty())
    return;

  Core::SetState(Core::State::Running);

  Movie::SaveRecording(dtm_file.toStdString());
}
