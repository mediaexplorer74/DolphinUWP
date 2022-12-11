// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt2/Settings/InterfacePane.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QWidget>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"

#include "DolphinQt2/Settings.h"

static QComboBox* MakeLanguageComboBox()
{
  static const struct
  {
    const QString name;
    const char* id;
  } languages[] = {
      {QStringLiteral(u"Bahasa Melayu"), "ms"},               // Malay
      {QStringLiteral(u"Catal\u00E0"), "ca"},                 // Catalan
      {QStringLiteral(u"\u010Ce\u0161tina"), "cs"},           // Czech
      {QStringLiteral(u"Dansk"), "da"},                       // Danish
      {QStringLiteral(u"Deutsch"), "de"},                     // German
      {QStringLiteral(u"English"), "en"},                     // English
      {QStringLiteral(u"Espa\u00F1ol"), "es"},                // Spanish
      {QStringLiteral(u"Fran\u00E7ais"), "fr"},               // French
      {QStringLiteral(u"Hrvatski"), "hr"},                    // Croatian
      {QStringLiteral(u"Italiano"), "it"},                    // Italian
      {QStringLiteral(u"Magyar"), "hu"},                      // Hungarian
      {QStringLiteral(u"Nederlands"), "nl"},                  // Dutch
      {QStringLiteral(u"Norsk bokm\u00E5l"), "nb"},           // Norwegian
      {QStringLiteral(u"Polski"), "pl"},                      // Polish
      {QStringLiteral(u"Portugu\u00EAs"), "pt"},              // Portuguese
      {QStringLiteral(u"Portugu\u00EAs (Brasil)"), "pt_BR"},  // Portuguese (Brazil)
      {QStringLiteral(u"Rom\u00E2n\u0103"), "ro"},            // Romanian
      {QStringLiteral(u"Srpski"), "sr"},                      // Serbian
      {QStringLiteral(u"Svenska"), "sv"},                     // Swedish
      {QStringLiteral(u"T\u00FCrk\u00E7e"), "tr"},            // Turkish
      {QStringLiteral(u"\u0395\u03BB\u03BB\u03B7\u03BD\u03B9\u03BA\u03AC"), "el"},  // Greek
      {QStringLiteral(u"\u0420\u0443\u0441\u0441\u043A\u0438\u0439"), "ru"},        // Russian
      {QStringLiteral(u"\u0627\u0644\u0639\u0631\u0628\u064A\u0629"), "ar"},        // Arabic
      {QStringLiteral(u"\u0641\u0627\u0631\u0633\u06CC"), "fa"},                    // Farsi
      {QStringLiteral(u"\uD55C\uAD6D\uC5B4"), "ko"},                                // Korean
      {QStringLiteral(u"\u65E5\u672C\u8A9E"), "ja"},                                // Japanese
      {QStringLiteral(u"\u7B80\u4F53\u4E2D\u6587"), "zh_CN"},  // Simplified Chinese
      {QStringLiteral(u"\u7E41\u9AD4\u4E2D\u6587"), "zh_TW"},  // Traditional Chinese
  };

  auto* combobox = new QComboBox();
  combobox->addItem(QObject::tr("<System Language>"), QStringLiteral(""));
  for (const auto& lang : languages)
    combobox->addItem(lang.name, QString::fromLatin1(lang.id));

  // The default, QComboBox::AdjustToContentsOnFirstShow, causes a noticeable pause when opening the
  // SettingWindow for the first time. The culprit seems to be non-Latin graphemes in the above
  // list. QComboBox::AdjustToContents still has some lag but it's much less noticeable.
  combobox->setSizeAdjustPolicy(QComboBox::AdjustToContents);

  return combobox;
}

InterfacePane::InterfacePane(QWidget* parent) : QWidget(parent)
{
  CreateLayout();
  ConnectLayout();
  LoadConfig();
}

void InterfacePane::CreateLayout()
{
  m_main_layout = new QVBoxLayout;
  // Create layout here
  CreateUI();
  CreateInGame();

  m_main_layout->setContentsMargins(0, 0, 0, 0);
  m_main_layout->addStretch(1);
  setLayout(m_main_layout);
}

void InterfacePane::CreateUI()
{
  auto* groupbox = new QGroupBox(tr("User Interface"));
  auto* groupbox_layout = new QVBoxLayout;
  groupbox->setLayout(groupbox_layout);
  m_main_layout->addWidget(groupbox);

  auto* combobox_layout = new QFormLayout;
  groupbox_layout->addLayout(combobox_layout);

  m_combobox_language = MakeLanguageComboBox();
  combobox_layout->addRow(tr("&Language:"), m_combobox_language);

  // Theme Combobox
  m_combobox_theme = new QComboBox;
  combobox_layout->addRow(tr("&Theme:"), m_combobox_theme);

  // List avalable themes
  auto file_search_results =
      Common::DoFileSearch({File::GetUserPath(D_THEMES_IDX), File::GetSysDirectory() + THEMES_DIR});
  for (const std::string& filename : file_search_results)
  {
    std::string name, ext;
    SplitPath(filename, nullptr, &name, &ext);
    name += ext;
    QString qt_name = QString::fromStdString(name);
    m_combobox_theme->addItem(qt_name);
  }

  // Checkboxes
  m_checkbox_auto_window = new QCheckBox(tr("Auto-Adjust Window Size"));
  m_checkbox_top_window = new QCheckBox(tr("Keep Window on Top"));
  m_checkbox_render_to_window = new QCheckBox(tr("Render to Main Window"));
  m_checkbox_use_builtin_title_database = new QCheckBox(tr("Use Built-In Database of Game Names"));
  groupbox_layout->addWidget(m_checkbox_auto_window);
  groupbox_layout->addWidget(m_checkbox_top_window);
  groupbox_layout->addWidget(m_checkbox_render_to_window);
  groupbox_layout->addWidget(m_checkbox_use_builtin_title_database);
}

void InterfacePane::CreateInGame()
{
  auto* groupbox = new QGroupBox(tr("In Game"));
  auto* groupbox_layout = new QVBoxLayout;
  groupbox->setLayout(groupbox_layout);
  m_main_layout->addWidget(groupbox);

  m_checkbox_confirm_on_stop = new QCheckBox(tr("Confirm on Stop"));
  m_checkbox_use_panic_handlers = new QCheckBox(tr("Use Panic Handlers"));
  m_checkbox_enable_osd = new QCheckBox(tr("Show On-Screen Messages"));
  m_checkbox_show_active_title = new QCheckBox(tr("Show Active Title in Window Title"));
  m_checkbox_pause_on_focus_lost = new QCheckBox(tr("Pause on Focus Loss"));
  m_checkbox_hide_mouse = new QCheckBox(tr("Always Hide Mouse Cursor"));

  groupbox_layout->addWidget(m_checkbox_confirm_on_stop);
  groupbox_layout->addWidget(m_checkbox_use_panic_handlers);
  groupbox_layout->addWidget(m_checkbox_enable_osd);
  groupbox_layout->addWidget(m_checkbox_show_active_title);
  groupbox_layout->addWidget(m_checkbox_pause_on_focus_lost);
  groupbox_layout->addWidget(m_checkbox_hide_mouse);
}

void InterfacePane::ConnectLayout()
{
  connect(m_checkbox_auto_window, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_top_window, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_render_to_window, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_use_builtin_title_database, &QCheckBox::clicked, this,
          &InterfacePane::OnSaveConfig);
  connect(m_combobox_theme, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::activated),
          &Settings::Instance(), &Settings::SetThemeName);
  connect(m_combobox_language, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated), this,
          &InterfacePane::OnSaveConfig);
  connect(m_checkbox_confirm_on_stop, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_use_panic_handlers, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_enable_osd, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_pause_on_focus_lost, &QCheckBox::clicked, this, &InterfacePane::OnSaveConfig);
  connect(m_checkbox_hide_mouse, &QCheckBox::clicked, &Settings::Instance(),
          &Settings::SetHideCursor);
}

void InterfacePane::LoadConfig()
{
  const SConfig& startup_params = SConfig::GetInstance();
  m_checkbox_auto_window->setChecked(startup_params.bRenderWindowAutoSize);
  m_checkbox_top_window->setChecked(startup_params.bKeepWindowOnTop);
  m_checkbox_render_to_window->setChecked(startup_params.bRenderToMain);
  m_checkbox_use_builtin_title_database->setChecked(startup_params.m_use_builtin_title_database);
  m_combobox_language->setCurrentIndex(m_combobox_language->findData(
      QString::fromStdString(SConfig::GetInstance().m_InterfaceLanguage)));
  m_combobox_theme->setCurrentIndex(
      m_combobox_theme->findText(QString::fromStdString(SConfig::GetInstance().theme_name)));

  // In Game Options
  m_checkbox_confirm_on_stop->setChecked(startup_params.bConfirmStop);
  m_checkbox_use_panic_handlers->setChecked(startup_params.bUsePanicHandlers);
  m_checkbox_enable_osd->setChecked(startup_params.bOnScreenDisplayMessages);
  m_checkbox_show_active_title->setChecked(startup_params.m_show_active_title);
  m_checkbox_pause_on_focus_lost->setChecked(startup_params.m_PauseOnFocusLost);
  m_checkbox_hide_mouse->setChecked(Settings::Instance().GetHideCursor());
}

void InterfacePane::OnSaveConfig()
{
  SConfig& settings = SConfig::GetInstance();
  settings.bRenderWindowAutoSize = m_checkbox_auto_window->isChecked();
  settings.bKeepWindowOnTop = m_checkbox_top_window->isChecked();
  settings.bRenderToMain = m_checkbox_render_to_window->isChecked();
  settings.m_use_builtin_title_database = m_checkbox_use_builtin_title_database->isChecked();

  // In Game Options
  settings.bConfirmStop = m_checkbox_confirm_on_stop->isChecked();
  settings.bUsePanicHandlers = m_checkbox_use_panic_handlers->isChecked();
  settings.bOnScreenDisplayMessages = m_checkbox_enable_osd->isChecked();
  settings.m_show_active_title = m_checkbox_show_active_title->isChecked();
  settings.m_PauseOnFocusLost = m_checkbox_pause_on_focus_lost->isChecked();

  auto new_language = m_combobox_language->currentData().toString().toStdString();
  if (new_language != SConfig::GetInstance().m_InterfaceLanguage)
  {
    SConfig::GetInstance().m_InterfaceLanguage = new_language;
    QMessageBox::information(
        this, tr("Restart Required"),
        tr("You must restart Dolphin in order for the change to take effect."));
  }

  settings.SaveSettings();
}
