// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinWX/ISOProperties/ISOProperties.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>
#include <wx/app.h>
#include <wx/artprov.h>
#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/mimetype.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PatchEngine.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DolphinWX/Cheats/ActionReplayCodesPanel.h"
#include "DolphinWX/Cheats/GeckoCodeDiag.h"
#include "DolphinWX/Config/ConfigMain.h"
#include "DolphinWX/DolphinSlider.h"
#include "DolphinWX/Frame.h"
#include "DolphinWX/Globals.h"
#include "DolphinWX/ISOFile.h"
#include "DolphinWX/ISOProperties/FilesystemPanel.h"
#include "DolphinWX/ISOProperties/InfoPanel.h"
#include "DolphinWX/Main.h"
#include "DolphinWX/PatchAddEdit.h"
#include "DolphinWX/WxUtils.h"

// A warning message displayed on the ARCodes and GeckoCodes pages when cheats are
// disabled globally to explain why turning cheats on does not work.
// Also displays a different warning when the game is currently running to explain
// that toggling codes has no effect while the game is already running.
class CheatWarningMessage final : public wxPanel
{
public:
  CheatWarningMessage(wxWindow* parent, std::string game_id)
      : wxPanel(parent), m_game_id(std::move(game_id))
  {
    SetExtraStyle(GetExtraStyle() | wxWS_EX_BLOCK_EVENTS);
    CreateGUI();
    wxTheApp->Bind(wxEVT_IDLE, &CheatWarningMessage::OnAppIdle, this);
    Hide();
  }

  void UpdateState()
  {
    // If cheats are disabled then show the notification about that.
    // If cheats are enabled and the game is currently running then display that warning.
    State new_state = State::Hidden;
    if (!SConfig::GetInstance().bEnableCheats)
      new_state = State::DisabledCheats;
    else if (Core::IsRunning() && SConfig::GetInstance().GetGameID() == m_game_id)
      new_state = State::GameRunning;
    ApplyState(new_state);
  }

private:
  enum class State
  {
    Inactive,
    Hidden,
    DisabledCheats,
    GameRunning
  };

  void CreateGUI()
  {
    int space10 = FromDIP(10);
    int space15 = FromDIP(15);

    wxStaticBitmap* icon =
        new wxStaticBitmap(this, wxID_ANY, wxArtProvider::GetMessageBoxIcon(wxICON_WARNING));
    m_message = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                 wxST_NO_AUTORESIZE);
    m_btn_configure = new wxButton(this, wxID_ANY, _("Configure Dolphin"));

    m_btn_configure->Bind(wxEVT_BUTTON, &CheatWarningMessage::OnConfigureClicked, this);

    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, space15);
    sizer->Add(m_message, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, space15);
    sizer->Add(m_btn_configure, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, space10);
    sizer->AddSpacer(space10);

    SetSizer(sizer);
  }

  void OnConfigureClicked(wxCommandEvent&)
  {
    main_frame->OpenGeneralConfiguration(CConfigMain::ID_GENERALPAGE);
    UpdateState();
  }

  void OnAppIdle(wxIdleEvent& ev)
  {
    ev.Skip();
    // Only respond to setting changes if we've been triggered once already.
    if (m_state != State::Inactive)
      UpdateState();
  }

  void ApplyState(State new_state)
  {
    // The purpose of this function is to prevent unnecessary UI updates which cause flickering.
    if (new_state == m_state || (m_state == State::Inactive && new_state == State::Hidden))
      return;

    bool visible = true;
    switch (new_state)
    {
    case State::Inactive:
    case State::Hidden:
      visible = false;
      break;

    case State::DisabledCheats:
      m_btn_configure->Show();
      m_message->SetLabelText(_("Dolphin's cheat system is currently disabled."));
      break;

    case State::GameRunning:
      m_btn_configure->Hide();
      m_message->SetLabelText(
          _("Changing cheats will only take effect when the game is restarted."));
      break;
    }
    m_state = new_state;
    Show(visible);
    GetParent()->Layout();
    if (visible)
    {
      m_message->Wrap(m_message->GetSize().GetWidth());
      m_message->InvalidateBestSize();
      GetParent()->Layout();
    }
  }

  std::string m_game_id;
  wxStaticText* m_message = nullptr;
  wxButton* m_btn_configure = nullptr;
  State m_state = State::Inactive;
};

wxDEFINE_EVENT(DOLPHIN_EVT_CHANGE_ISO_PROPERTIES_TITLE, wxCommandEvent);

BEGIN_EVENT_TABLE(CISOProperties, wxDialog)
EVT_CLOSE(CISOProperties::OnClose)
EVT_BUTTON(wxID_OK, CISOProperties::OnCloseClick)
EVT_BUTTON(ID_EDITCONFIG, CISOProperties::OnEditConfig)
EVT_BUTTON(ID_SHOWDEFAULTCONFIG, CISOProperties::OnShowDefaultConfig)
EVT_CHOICE(ID_EMUSTATE, CISOProperties::OnEmustateChanged)
EVT_LISTBOX(ID_PATCHES_LIST, CISOProperties::PatchListSelectionChanged)
EVT_BUTTON(ID_EDITPATCH, CISOProperties::PatchButtonClicked)
EVT_BUTTON(ID_ADDPATCH, CISOProperties::PatchButtonClicked)
EVT_BUTTON(ID_REMOVEPATCH, CISOProperties::PatchButtonClicked)
END_EVENT_TABLE()

CISOProperties::CISOProperties(const GameListItem& game_list_item, wxWindow* parent, wxWindowID id,
                               const wxString& title, const wxPoint& position, const wxSize& size,
                               long style)
    : wxDialog(parent, id, title, position, size, style), m_open_gamelist_item(game_list_item)
{
  Bind(DOLPHIN_EVT_CHANGE_ISO_PROPERTIES_TITLE, &CISOProperties::OnChangeTitle, this);

  // Load ISO data
  m_open_iso = DiscIO::CreateVolumeFromFilename(m_open_gamelist_item.GetFileName());

  m_game_id = m_open_iso->GetGameID();

  // Load game INIs
  m_gameini_file_local = File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini";
  m_gameini_default = SConfig::LoadDefaultGameIni(m_game_id, m_open_iso->GetRevision());
  m_gameini_local = SConfig::LoadLocalGameIni(m_game_id, m_open_iso->GetRevision());

  // Setup GUI
  CreateGUIControls();
  LoadGameConfig();

  wxTheApp->Bind(DOLPHIN_EVT_LOCAL_INI_CHANGED, &CISOProperties::OnLocalIniModified, this);
}

CISOProperties::~CISOProperties()
{
}

long CISOProperties::GetElementStyle(const char* section, const char* key)
{
  // Disable 3rd state if default gameini overrides the setting
  if (m_gameini_default.Exists(section, key))
    return 0;

  return wxCHK_3STATE | wxCHK_ALLOW_3RD_STATE_FOR_USER;
}

void CISOProperties::CreateGUIControls()
{
  const int space5 = FromDIP(5);

  wxButton* const edit_config = new wxButton(this, ID_EDITCONFIG, _("Edit Config"));
  edit_config->SetToolTip(_("This will let you manually edit the INI config file."));

  wxButton* const edit_default_config =
      new wxButton(this, ID_SHOWDEFAULTCONFIG, _("Show Defaults"));
  edit_default_config->SetToolTip(
      _("Opens the default (read-only) configuration for this game in an external text editor."));

  // Notebook
  wxNotebook* const notebook = new wxNotebook(this, ID_NOTEBOOK);
  wxPanel* const m_GameConfig = new wxPanel(notebook, ID_GAMECONFIG);
  notebook->AddPage(m_GameConfig, _("GameConfig"));
  wxPanel* const m_PatchPage = new wxPanel(notebook, ID_PATCH_PAGE);
  notebook->AddPage(m_PatchPage, _("Patches"));
  wxPanel* const m_CheatPage = new wxPanel(notebook, ID_ARCODE_PAGE);
  notebook->AddPage(m_CheatPage, _("AR Codes"));
  wxPanel* const gecko_cheat_page = new wxPanel(notebook);
  notebook->AddPage(gecko_cheat_page, _("Gecko Codes"));
  notebook->AddPage(new InfoPanel(notebook, ID_INFORMATION, m_open_gamelist_item, m_open_iso),
                    _("Info"));

  // GameConfig editing - Overrides and emulation state
  wxStaticText* const OverrideText = new wxStaticText(
      m_GameConfig, wxID_ANY, _("These settings override core Dolphin settings.\nUndetermined "
                                "means the game uses Dolphin's setting."));

  // Core
  m_cpu_thread =
      new wxCheckBox(m_GameConfig, ID_USEDUALCORE, _("Enable Dual Core"), wxDefaultPosition,
                     wxDefaultSize, GetElementStyle("Core", "CPUThread"));
  m_mmu = new wxCheckBox(m_GameConfig, ID_MMU, _("Enable MMU"), wxDefaultPosition, wxDefaultSize,
                         GetElementStyle("Core", "MMU"));
  m_mmu->SetToolTip(_(
      "Enables the Memory Management Unit, needed for some games. (ON = Compatible, OFF = Fast)"));
  m_dcbz_off = new wxCheckBox(m_GameConfig, ID_DCBZOFF, _("Skip DCBZ clearing"), wxDefaultPosition,
                              wxDefaultSize, GetElementStyle("Core", "DCBZ"));
  m_dcbz_off->SetToolTip(_("Bypass the clearing of the data cache by the DCBZ instruction. Usually "
                           "leave this option disabled."));
  m_fprf = new wxCheckBox(m_GameConfig, ID_FPRF, _("Enable FPRF"), wxDefaultPosition, wxDefaultSize,
                          GetElementStyle("Core", "FPRF"));
  m_fprf->SetToolTip(
      _("Enables Floating Point Result Flag calculation, needed for a few games. (ON "
        "= Compatible, OFF = Fast)"));
  m_sync_gpu = new wxCheckBox(m_GameConfig, ID_SYNCGPU, _("Synchronize GPU thread"),
                              wxDefaultPosition, wxDefaultSize, GetElementStyle("Core", "SyncGPU"));
  m_sync_gpu->SetToolTip(_("Synchronizes the GPU and CPU threads to help prevent random freezes in "
                           "Dual Core mode. (ON = Compatible, OFF = Fast)"));
  m_fast_disc_speed =
      new wxCheckBox(m_GameConfig, ID_DISCSPEED, _("Speed up Disc Transfer Rate"),
                     wxDefaultPosition, wxDefaultSize, GetElementStyle("Core", "FastDiscSpeed"));
  m_fast_disc_speed->SetToolTip(
      _("Enable fast disc access. This can cause crashes and other problems "
        "in some games. (ON = Fast, OFF = Compatible)"));
  m_dps_hle = new wxCheckBox(m_GameConfig, ID_AUDIO_DSP_HLE, _("DSP HLE Emulation (fast)"),
                             wxDefaultPosition, wxDefaultSize, GetElementStyle("Core", "DSPHLE"));

  wxBoxSizer* const gpu_determinism_sizer = new wxBoxSizer(wxHORIZONTAL);
  wxStaticText* const gpu_determinism_text =
      new wxStaticText(m_GameConfig, wxID_ANY, _("Deterministic dual core: "));
  m_gpu_determinism_string.Add(_("Not Set"));
  m_gpu_determinism_string.Add(_("auto"));
  m_gpu_determinism_string.Add(_("none"));
  m_gpu_determinism_string.Add(_("fake-completion"));
  m_gpu_determinism = new wxChoice(m_GameConfig, ID_GPUDETERMINISM, wxDefaultPosition,
                                   wxDefaultSize, m_gpu_determinism_string);
  gpu_determinism_sizer->Add(gpu_determinism_text, 0, wxALIGN_CENTER_VERTICAL);
  gpu_determinism_sizer->Add(m_gpu_determinism, 0, wxALIGN_CENTER_VERTICAL);

  // Wii Console
  m_enable_widescreen =
      new wxCheckBox(m_GameConfig, ID_ENABLEWIDESCREEN, _("Enable WideScreen"), wxDefaultPosition,
                     wxDefaultSize, GetElementStyle("Wii", "Widescreen"));

  // Stereoscopy
  wxBoxSizer* const depth_percentage = new wxBoxSizer(wxHORIZONTAL);
  wxStaticText* const depth_percentage_text =
      new wxStaticText(m_GameConfig, wxID_ANY, _("Depth Percentage: "));
  m_depth_percentage = new DolphinSlider(m_GameConfig, ID_DEPTHPERCENTAGE, 100, 0, 200);
  m_depth_percentage->SetToolTip(
      _("This value is multiplied with the depth set in the graphics configuration."));
  depth_percentage->Add(depth_percentage_text);
  depth_percentage->Add(m_depth_percentage);

  wxBoxSizer* const convergence_sizer = new wxBoxSizer(wxHORIZONTAL);
  wxStaticText* const convergence_text =
      new wxStaticText(m_GameConfig, wxID_ANY, _("Convergence: "));
  m_convergence = new wxSpinCtrl(m_GameConfig, ID_CONVERGENCE);
  m_convergence->SetRange(0, INT32_MAX);
  m_convergence->SetToolTip(
      _("This value is added to the convergence value set in the graphics configuration."));
  convergence_sizer->Add(convergence_text);
  convergence_sizer->Add(m_convergence);

  m_mono_depth =
      new wxCheckBox(m_GameConfig, ID_MONODEPTH, _("Monoscopic Shadows"), wxDefaultPosition,
                     wxDefaultSize, GetElementStyle("Video_Stereoscopy", "StereoEFBMonoDepth"));
  m_mono_depth->SetToolTip(_("Use a single depth buffer for both eyes. Needed for a few games."));

  wxBoxSizer* const emustate_sizer = new wxBoxSizer(wxHORIZONTAL);
  wxStaticText* const emustate_text =
      new wxStaticText(m_GameConfig, wxID_ANY, _("Emulation State: "));
  m_emustate_string.Add(_("Not Set"));
  m_emustate_string.Add(_("Broken"));
  m_emustate_string.Add(_("Intro"));
  m_emustate_string.Add(_("In Game"));
  m_emustate_string.Add(_("Playable"));
  m_emustate_string.Add(_("Perfect"));
  m_emustate_choice =
      new wxChoice(m_GameConfig, ID_EMUSTATE, wxDefaultPosition, wxDefaultSize, m_emustate_string);
  m_emu_issues = new wxTextCtrl(m_GameConfig, ID_EMU_ISSUES, wxEmptyString);
  emustate_sizer->Add(emustate_text, 0, wxALIGN_CENTER_VERTICAL);
  emustate_sizer->Add(m_emustate_choice, 0, wxALIGN_CENTER_VERTICAL);
  emustate_sizer->Add(m_emu_issues, 1, wxEXPAND);

  wxStaticBoxSizer* const core_overrides_sizer =
      new wxStaticBoxSizer(wxVERTICAL, m_GameConfig, _("Core"));
  core_overrides_sizer->Add(m_cpu_thread, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_mmu, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_dcbz_off, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_fprf, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_sync_gpu, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_fast_disc_speed, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->Add(m_dps_hle, 0, wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->AddSpacer(space5);
  core_overrides_sizer->Add(gpu_determinism_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  core_overrides_sizer->AddSpacer(space5);

  wxStaticBoxSizer* const wii_overrides_sizer =
      new wxStaticBoxSizer(wxVERTICAL, m_GameConfig, _("Wii Console"));
  if (m_open_iso->GetVolumeType() == DiscIO::Platform::GAMECUBE_DISC)
  {
    wii_overrides_sizer->ShowItems(false);
    m_enable_widescreen->Hide();
  }
  wii_overrides_sizer->Add(m_enable_widescreen, 0, wxLEFT, space5);

  wxStaticBoxSizer* const stereo_overrides_sizer =
      new wxStaticBoxSizer(wxVERTICAL, m_GameConfig, _("Stereoscopy"));
  stereo_overrides_sizer->Add(depth_percentage);
  stereo_overrides_sizer->Add(convergence_sizer);
  stereo_overrides_sizer->Add(m_mono_depth);

  wxStaticBoxSizer* const game_config_sizer =
      new wxStaticBoxSizer(wxVERTICAL, m_GameConfig, _("Game-Specific Settings"));
  game_config_sizer->AddSpacer(space5);
  game_config_sizer->Add(OverrideText, 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  game_config_sizer->AddSpacer(space5);
  game_config_sizer->Add(core_overrides_sizer, 0, wxEXPAND);
  game_config_sizer->Add(wii_overrides_sizer, 0, wxEXPAND);
  game_config_sizer->Add(stereo_overrides_sizer, 0, wxEXPAND);

  wxBoxSizer* const config_page_sizer = new wxBoxSizer(wxVERTICAL);
  config_page_sizer->AddSpacer(space5);
  config_page_sizer->Add(game_config_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  config_page_sizer->AddSpacer(space5);
  config_page_sizer->Add(emustate_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  config_page_sizer->AddSpacer(space5);
  m_GameConfig->SetSizer(config_page_sizer);

  // Patches
  wxBoxSizer* const patches_sizer = new wxBoxSizer(wxVERTICAL);
  m_patches = new wxCheckListBox(m_PatchPage, ID_PATCHES_LIST, wxDefaultPosition, wxDefaultSize, 0,
                                 nullptr, wxLB_HSCROLL);
  wxBoxSizer* const sPatchButtons = new wxBoxSizer(wxHORIZONTAL);
  m_edit_patch = new wxButton(m_PatchPage, ID_EDITPATCH, _("Edit..."));
  wxButton* const AddPatch = new wxButton(m_PatchPage, ID_ADDPATCH, _("Add..."));
  m_remove_patch = new wxButton(m_PatchPage, ID_REMOVEPATCH, _("Remove"));
  m_edit_patch->Disable();
  m_remove_patch->Disable();

  wxBoxSizer* patch_page_sizer = new wxBoxSizer(wxVERTICAL);
  patches_sizer->Add(m_patches, 1, wxEXPAND);
  sPatchButtons->Add(m_edit_patch, 0, wxEXPAND);
  sPatchButtons->AddStretchSpacer();
  sPatchButtons->Add(AddPatch, 0, wxEXPAND);
  sPatchButtons->Add(m_remove_patch, 0, wxEXPAND);
  patches_sizer->Add(sPatchButtons, 0, wxEXPAND);
  patch_page_sizer->AddSpacer(space5);
  patch_page_sizer->Add(patches_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, space5);
  patch_page_sizer->AddSpacer(space5);
  m_PatchPage->SetSizer(patch_page_sizer);

  // Action Replay Cheats
  m_ar_code_panel =
      new ActionReplayCodesPanel(m_CheatPage, ActionReplayCodesPanel::STYLE_MODIFY_BUTTONS);
  m_cheats_disabled_ar = new CheatWarningMessage(m_CheatPage, m_game_id);

  m_ar_code_panel->Bind(DOLPHIN_EVT_ARCODE_TOGGLED, &CISOProperties::OnCheatCodeToggled, this);

  wxBoxSizer* const cheat_page_sizer = new wxBoxSizer(wxVERTICAL);
  cheat_page_sizer->Add(m_cheats_disabled_ar, 0, wxEXPAND | wxTOP, space5);
  cheat_page_sizer->Add(m_ar_code_panel, 1, wxEXPAND | wxALL, space5);
  m_CheatPage->SetSizer(cheat_page_sizer);

  // Gecko Cheats
  m_geckocode_panel = new Gecko::CodeConfigPanel(gecko_cheat_page);
  m_cheats_disabled_gecko = new CheatWarningMessage(gecko_cheat_page, m_game_id);

  m_geckocode_panel->Bind(DOLPHIN_EVT_GECKOCODE_TOGGLED, &CISOProperties::OnCheatCodeToggled, this);

  wxBoxSizer* gecko_layout = new wxBoxSizer(wxVERTICAL);
  gecko_layout->Add(m_cheats_disabled_gecko, 0, wxEXPAND | wxTOP, space5);
  gecko_layout->Add(m_geckocode_panel, 1, wxEXPAND);
  gecko_cheat_page->SetSizer(gecko_layout);

  if (DiscIO::IsDisc(m_open_iso->GetVolumeType()))
  {
    notebook->AddPage(new FilesystemPanel(notebook, ID_FILESYSTEM, m_open_iso), _("Filesystem"));
  }

  wxStdDialogButtonSizer* buttons_sizer = CreateStdDialogButtonSizer(wxOK | wxNO_DEFAULT);
  buttons_sizer->Prepend(edit_default_config);
  buttons_sizer->Prepend(edit_config);
  buttons_sizer->GetAffirmativeButton()->SetLabel(_("Close"));

  // If there is no default gameini, disable the button.
  const std::vector<std::string> ini_names =
      ConfigLoaders::GetGameIniFilenames(m_game_id, m_open_iso->GetRevision());
  const bool game_ini_exists =
      std::any_of(ini_names.cbegin(), ini_names.cend(), [](const std::string& name) {
        return File::Exists(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name);
      });
  if (!game_ini_exists)
    edit_default_config->Disable();

  // Add notebook and buttons to the dialog
  wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
  main_sizer->AddSpacer(space5);
  main_sizer->Add(notebook, 1, wxEXPAND | wxLEFT | wxRIGHT, space5);
  main_sizer->AddSpacer(space5);
  main_sizer->Add(buttons_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  main_sizer->AddSpacer(space5);
  main_sizer->SetMinSize(FromDIP(wxSize(500, -1)));

  SetLayoutAdaptationMode(wxDIALOG_ADAPTATION_MODE_ENABLED);
  SetLayoutAdaptationLevel(wxDIALOG_ADAPTATION_STANDARD_SIZER);
  SetSizerAndFit(main_sizer);
  Center();
  SetFocus();
}

void CISOProperties::OnClose(wxCloseEvent& WXUNUSED(event))
{
  if (!SaveGameConfig())
    WxUtils::ShowErrorDialog(
        wxString::Format(_("Could not save %s."), m_gameini_file_local.c_str()));
  Destroy();
}

void CISOProperties::OnCloseClick(wxCommandEvent& WXUNUSED(event))
{
  Close();
}

void CISOProperties::OnEmustateChanged(wxCommandEvent& event)
{
  m_emu_issues->Enable(event.GetSelection() != 0);
}

void CISOProperties::SetCheckboxValueFromGameini(const char* section, const char* key,
                                                 wxCheckBox* checkbox)
{
  // Prefer local gameini value over default gameini value.
  bool value;
  if (m_gameini_local.GetOrCreateSection(section)->Get(key, &value))
    checkbox->Set3StateValue((wxCheckBoxState)value);
  else if (m_gameini_default.GetOrCreateSection(section)->Get(key, &value))
    checkbox->Set3StateValue((wxCheckBoxState)value);
  else
    checkbox->Set3StateValue(wxCHK_UNDETERMINED);
}

void CISOProperties::LoadGameConfig()
{
  SetCheckboxValueFromGameini("Core", "CPUThread", m_cpu_thread);
  SetCheckboxValueFromGameini("Core", "MMU", m_mmu);
  SetCheckboxValueFromGameini("Core", "DCBZ", m_dcbz_off);
  SetCheckboxValueFromGameini("Core", "FPRF", m_fprf);
  SetCheckboxValueFromGameini("Core", "SyncGPU", m_sync_gpu);
  SetCheckboxValueFromGameini("Core", "FastDiscSpeed", m_fast_disc_speed);
  SetCheckboxValueFromGameini("Core", "DSPHLE", m_dps_hle);
  SetCheckboxValueFromGameini("Wii", "Widescreen", m_enable_widescreen);
  SetCheckboxValueFromGameini("Video_Stereoscopy", "StereoEFBMonoDepth", m_mono_depth);

  IniFile::Section* default_video = m_gameini_default.GetOrCreateSection("Video");

  int iTemp;
  default_video->Get("ProjectionHack", &iTemp);
  default_video->Get("PH_SZNear", &m_phack_data.PHackSZNear);
  if (m_gameini_local.GetIfExists("Video", "PH_SZNear", &iTemp))
    m_phack_data.PHackSZNear = !!iTemp;
  default_video->Get("PH_SZFar", &m_phack_data.PHackSZFar);
  if (m_gameini_local.GetIfExists("Video", "PH_SZFar", &iTemp))
    m_phack_data.PHackSZFar = !!iTemp;

  std::string sTemp;
  default_video->Get("PH_ZNear", &m_phack_data.PHZNear);
  if (m_gameini_local.GetIfExists("Video", "PH_ZNear", &sTemp))
    m_phack_data.PHZNear = sTemp;
  default_video->Get("PH_ZFar", &m_phack_data.PHZFar);
  if (m_gameini_local.GetIfExists("Video", "PH_ZFar", &sTemp))
    m_phack_data.PHZFar = sTemp;

  IniFile::Section* default_emustate = m_gameini_default.GetOrCreateSection("EmuState");
  default_emustate->Get("EmulationStateId", &iTemp, 0 /*Not Set*/);
  m_emustate_choice->SetSelection(iTemp);
  if (m_gameini_local.GetIfExists("EmuState", "EmulationStateId", &iTemp))
    m_emustate_choice->SetSelection(iTemp);

  default_emustate->Get("EmulationIssues", &sTemp);
  if (!sTemp.empty())
    m_emu_issues->SetValue(StrToWxStr(sTemp));
  if (m_gameini_local.GetIfExists("EmuState", "EmulationIssues", &sTemp))
    m_emu_issues->SetValue(StrToWxStr(sTemp));

  m_emu_issues->Enable(m_emustate_choice->GetSelection() != 0);

  sTemp = "";
  if (!m_gameini_local.GetIfExists("Core", "GPUDeterminismMode", &sTemp))
    m_gameini_default.GetIfExists("Core", "GPUDeterminismMode", &sTemp);

  if (sTemp == "")
    m_gpu_determinism->SetSelection(0);
  else if (sTemp == "auto")
    m_gpu_determinism->SetSelection(1);
  else if (sTemp == "none")
    m_gpu_determinism->SetSelection(2);
  else if (sTemp == "fake-completion")
    m_gpu_determinism->SetSelection(3);

  IniFile::Section* default_stereoscopy = m_gameini_default.GetOrCreateSection("Video_Stereoscopy");
  default_stereoscopy->Get("StereoDepthPercentage", &iTemp, 100);
  m_gameini_local.GetIfExists("Video_Stereoscopy", "StereoDepthPercentage", &iTemp);
  m_depth_percentage->SetValue(iTemp);
  default_stereoscopy->Get("StereoConvergence", &iTemp, 0);
  m_gameini_local.GetIfExists("Video_Stereoscopy", "StereoConvergence", &iTemp);
  m_convergence->SetValue(iTemp);

  PatchList_Load();
  m_ar_code_panel->LoadCodes(m_gameini_default, m_gameini_local);
  m_geckocode_panel->LoadCodes(m_gameini_default, m_gameini_local, m_open_iso->GetGameID());
}

void CISOProperties::SaveGameIniValueFrom3StateCheckbox(const char* section, const char* key,
                                                        wxCheckBox* checkbox)
{
  // Delete any existing entries from the local gameini if checkbox is undetermined.
  // Otherwise, write the current value to the local gameini if the value differs from the default
  // gameini values.
  // Delete any existing entry from the local gameini if the value does not differ from the default
  // gameini value.
  bool checkbox_val = (checkbox->Get3StateValue() == wxCHK_CHECKED);

  if (checkbox->Get3StateValue() == wxCHK_UNDETERMINED)
    m_gameini_local.DeleteKey(section, key);
  else if (!m_gameini_default.Exists(section, key))
    m_gameini_local.GetOrCreateSection(section)->Set(key, checkbox_val);
  else
  {
    bool default_value;
    m_gameini_default.GetOrCreateSection(section)->Get(key, &default_value);
    if (default_value != checkbox_val)
      m_gameini_local.GetOrCreateSection(section)->Set(key, checkbox_val);
    else
      m_gameini_local.DeleteKey(section, key);
  }
}

bool CISOProperties::SaveGameConfig()
{
  SaveGameIniValueFrom3StateCheckbox("Core", "CPUThread", m_cpu_thread);
  SaveGameIniValueFrom3StateCheckbox("Core", "MMU", m_mmu);
  SaveGameIniValueFrom3StateCheckbox("Core", "DCBZ", m_dcbz_off);
  SaveGameIniValueFrom3StateCheckbox("Core", "FPRF", m_fprf);
  SaveGameIniValueFrom3StateCheckbox("Core", "SyncGPU", m_sync_gpu);
  SaveGameIniValueFrom3StateCheckbox("Core", "FastDiscSpeed", m_fast_disc_speed);
  SaveGameIniValueFrom3StateCheckbox("Core", "DSPHLE", m_dps_hle);
  SaveGameIniValueFrom3StateCheckbox("Wii", "Widescreen", m_enable_widescreen);
  SaveGameIniValueFrom3StateCheckbox("Video_Stereoscopy", "StereoEFBMonoDepth", m_mono_depth);

#define SAVE_IF_NOT_DEFAULT(section, key, val, def)                                                \
  do                                                                                               \
  {                                                                                                \
    if (m_gameini_default.Exists((section), (key)))                                                \
    {                                                                                              \
      std::remove_reference<decltype((val))>::type tmp__;                                          \
      m_gameini_default.GetOrCreateSection((section))->Get((key), &tmp__);                         \
      if ((val) != tmp__)                                                                          \
        m_gameini_local.GetOrCreateSection((section))->Set((key), (val));                          \
      else                                                                                         \
        m_gameini_local.DeleteKey((section), (key));                                               \
    }                                                                                              \
    else if ((val) != (def))                                                                       \
      m_gameini_local.GetOrCreateSection((section))->Set((key), (val));                            \
    else                                                                                           \
      m_gameini_local.DeleteKey((section), (key));                                                 \
  } while (0)

  SAVE_IF_NOT_DEFAULT("Video", "PH_SZNear", (m_phack_data.PHackSZNear ? 1 : 0), 0);
  SAVE_IF_NOT_DEFAULT("Video", "PH_SZFar", (m_phack_data.PHackSZFar ? 1 : 0), 0);
  SAVE_IF_NOT_DEFAULT("Video", "PH_ZNear", m_phack_data.PHZNear, "");
  SAVE_IF_NOT_DEFAULT("Video", "PH_ZFar", m_phack_data.PHZFar, "");
  SAVE_IF_NOT_DEFAULT("EmuState", "EmulationStateId", m_emustate_choice->GetSelection(), 0);

  std::string emu_issues = m_emu_issues->GetValue().ToStdString();
  SAVE_IF_NOT_DEFAULT("EmuState", "EmulationIssues", emu_issues, "");

  std::string tmp;
  if (m_gpu_determinism->GetSelection() == 0)
    tmp = "Not Set";
  else if (m_gpu_determinism->GetSelection() == 1)
    tmp = "auto";
  else if (m_gpu_determinism->GetSelection() == 2)
    tmp = "none";
  else if (m_gpu_determinism->GetSelection() == 3)
    tmp = "fake-completion";

  SAVE_IF_NOT_DEFAULT("Core", "GPUDeterminismMode", tmp, "Not Set");

  int depth = m_depth_percentage->GetValue() > 0 ? m_depth_percentage->GetValue() : 100;
  SAVE_IF_NOT_DEFAULT("Video_Stereoscopy", "StereoDepthPercentage", depth, 100);
  SAVE_IF_NOT_DEFAULT("Video_Stereoscopy", "StereoConvergence", m_convergence->GetValue(), 0);

  PatchList_Save();
  m_ar_code_panel->SaveCodes(&m_gameini_local);
  Gecko::SaveCodes(m_gameini_local, m_geckocode_panel->GetCodes());

  bool success = m_gameini_local.Save(m_gameini_file_local);

  // If the resulting file is empty, delete it. Kind of a hack, but meh.
  if (success && File::GetSize(m_gameini_file_local) == 0)
    File::Delete(m_gameini_file_local);

  if (success)
    GenerateLocalIniModified();

  return success;
}

void CISOProperties::LaunchExternalEditor(const std::string& filename, bool wait_until_closed)
{
#ifdef __APPLE__
  // GetOpenCommand does not work for wxCocoa
  const char* OpenCommandConst[] = {"open", "-a", "TextEdit", filename.c_str(), NULL};
  char** OpenCommand = const_cast<char**>(OpenCommandConst);
#else
  wxFileType* file_type = wxTheMimeTypesManager->GetFileTypeFromExtension("ini");
  if (file_type == nullptr)  // From extension failed, trying with MIME type now
  {
    file_type = wxTheMimeTypesManager->GetFileTypeFromMimeType("text/plain");
    if (file_type == nullptr)  // MIME type failed, aborting mission
    {
      WxUtils::ShowErrorDialog(_("Filetype 'ini' is unknown! Will not open!"));
      return;
    }
  }

  wxString OpenCommand = file_type->GetOpenCommand(StrToWxStr(filename));
  if (OpenCommand.IsEmpty())
  {
    WxUtils::ShowErrorDialog(_("Couldn't find open command for extension 'ini'!"));
    return;
  }
#endif

  long result;

  if (wait_until_closed)
    result = wxExecute(OpenCommand, wxEXEC_SYNC);
  else
    result = wxExecute(OpenCommand);

  if (result == -1)
  {
    WxUtils::ShowErrorDialog(_("wxExecute returned -1 on application run!"));
    return;
  }
}

void CISOProperties::GenerateLocalIniModified()
{
  wxCommandEvent event_update(DOLPHIN_EVT_LOCAL_INI_CHANGED);
  event_update.SetString(StrToWxStr(m_game_id));
  event_update.SetInt(m_open_gamelist_item.GetRevision());
  wxTheApp->ProcessEvent(event_update);
}

void CISOProperties::OnLocalIniModified(wxCommandEvent& ev)
{
  ev.Skip();
  if (WxStrToStr(ev.GetString()) != m_game_id)
    return;

  m_gameini_local.Load(m_gameini_file_local);
  LoadGameConfig();
}

void CISOProperties::OnEditConfig(wxCommandEvent& WXUNUSED(event))
{
  SaveGameConfig();
  // Create blank file to prevent editor from prompting to create it.
  if (!File::Exists(m_gameini_file_local))
  {
    std::fstream blank_file(m_gameini_file_local, std::ios::out);
    blank_file.close();
  }
  LaunchExternalEditor(m_gameini_file_local, true);
  GenerateLocalIniModified();
}

void CISOProperties::OnCheatCodeToggled(wxCommandEvent&)
{
  m_cheats_disabled_ar->UpdateState();
  m_cheats_disabled_gecko->UpdateState();
}

void CISOProperties::OnChangeTitle(wxCommandEvent& event)
{
  SetTitle(event.GetString());
}

// Opens all pre-defined INIs for the game. If there are multiple ones,
// they will all be opened, but there is usually only one
void CISOProperties::OnShowDefaultConfig(wxCommandEvent& WXUNUSED(event))
{
  for (const std::string& filename :
       ConfigLoaders::GetGameIniFilenames(m_game_id, m_open_iso->GetRevision()))
  {
    std::string path = File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + filename;
    if (File::Exists(path))
      LaunchExternalEditor(path, false);
  }
}

void CISOProperties::PatchListSelectionChanged(wxCommandEvent& event)
{
  if (m_patches->GetSelection() == wxNOT_FOUND ||
      m_default_patches.find(m_patches->GetString(m_patches->GetSelection()).ToStdString()) !=
          m_default_patches.end())
  {
    m_edit_patch->Disable();
    m_remove_patch->Disable();
  }
  else
  {
    m_edit_patch->Enable();
    m_remove_patch->Enable();
  }
}

void CISOProperties::PatchList_Load()
{
  m_on_frame.clear();
  m_patches->Clear();

  PatchEngine::LoadPatchSection("OnFrame", m_on_frame, m_gameini_default, m_gameini_local);

  u32 index = 0;
  for (PatchEngine::Patch& p : m_on_frame)
  {
    m_patches->Append(StrToWxStr(p.name));
    m_patches->Check(index, p.active);
    if (!p.user_defined)
      m_default_patches.insert(p.name);
    ++index;
  }
}

void CISOProperties::PatchList_Save()
{
  std::vector<std::string> lines;
  std::vector<std::string> enabled_lines;
  u32 index = 0;
  for (PatchEngine::Patch& p : m_on_frame)
  {
    if (m_patches->IsChecked(index))
      enabled_lines.push_back("$" + p.name);

    // Do not save default patches.
    if (m_default_patches.find(p.name) == m_default_patches.end())
    {
      lines.push_back("$" + p.name);
      for (const PatchEngine::PatchEntry& entry : p.entries)
      {
        std::string temp = StringFromFormat("0x%08X:%s:0x%08X", entry.address,
                                            PatchEngine::PatchTypeStrings[entry.type], entry.value);
        lines.push_back(temp);
      }
    }
    ++index;
  }
  m_gameini_local.SetLines("OnFrame_Enabled", enabled_lines);
  m_gameini_local.SetLines("OnFrame", lines);
}

void CISOProperties::PatchButtonClicked(wxCommandEvent& event)
{
  int selection = m_patches->GetSelection();

  switch (event.GetId())
  {
  case ID_EDITPATCH:
  {
    CPatchAddEdit dlg(selection, &m_on_frame, this);
    dlg.ShowModal();
    Raise();
  }
  break;
  case ID_ADDPATCH:
  {
    CPatchAddEdit dlg(-1, &m_on_frame, this, 1, _("Add Patch"));
    int res = dlg.ShowModal();
    Raise();
    if (res == wxID_OK)
    {
      m_patches->Append(StrToWxStr(m_on_frame.back().name));
      m_patches->Check((unsigned int)(m_on_frame.size() - 1), m_on_frame.back().active);
    }
  }
  break;
  case ID_REMOVEPATCH:
    m_on_frame.erase(m_on_frame.begin() + m_patches->GetSelection());
    m_patches->Delete(m_patches->GetSelection());
    break;
  }

  PatchList_Save();
  m_patches->Clear();
  PatchList_Load();

  m_edit_patch->Disable();
  m_remove_patch->Disable();
}
