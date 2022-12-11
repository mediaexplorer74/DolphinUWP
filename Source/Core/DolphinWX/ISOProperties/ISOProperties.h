// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <wx/dialog.h>
#include <wx/treebase.h>

#include "Common/IniFile.h"
#include "DolphinWX/ISOFile.h"
#include "DolphinWX/PatchAddEdit.h"

class ActionReplayCodesPanel;
class CheatWarningMessage;
class DolphinSlider;
class wxButton;
class wxCheckBox;
class wxCheckListBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticBitmap;
class wxTextCtrl;

namespace DiscIO
{
class Volume;
}

namespace Gecko
{
class CodeConfigPanel;
}

struct PHackData
{
  bool PHackSZNear;
  bool PHackSZFar;
  std::string PHZNear;
  std::string PHZFar;
};

wxDECLARE_EVENT(DOLPHIN_EVT_CHANGE_ISO_PROPERTIES_TITLE, wxCommandEvent);

class CISOProperties : public wxDialog
{
public:
  CISOProperties(const GameListItem& game_list_item, wxWindow* parent, wxWindowID id = wxID_ANY,
                 const wxString& title = _("Properties"), const wxPoint& pos = wxDefaultPosition,
                 const wxSize& size = wxDefaultSize,
                 long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  virtual ~CISOProperties();

private:
  DECLARE_EVENT_TABLE()

  std::unique_ptr<DiscIO::Volume> m_open_iso;

  std::vector<PatchEngine::Patch> m_on_frame;
  PHackData m_phack_data;

  // Core
  wxCheckBox *m_cpu_thread, *m_mmu, *m_dcbz_off, *m_fprf;
  wxCheckBox *m_sync_gpu, *m_fast_disc_speed, *m_dps_hle;

  wxArrayString m_gpu_determinism_string;
  wxChoice* m_gpu_determinism;
  // Wii
  wxCheckBox* m_enable_widescreen;

  // Stereoscopy
  DolphinSlider* m_depth_percentage;
  wxSpinCtrl* m_convergence;
  wxCheckBox* m_mono_depth;

  wxArrayString m_emustate_string;
  wxChoice* m_emustate_choice;
  wxTextCtrl* m_emu_issues;

  wxCheckListBox* m_patches;
  wxButton* m_edit_patch;
  wxButton* m_remove_patch;

  ActionReplayCodesPanel* m_ar_code_panel;
  Gecko::CodeConfigPanel* m_geckocode_panel;

  CheatWarningMessage* m_cheats_disabled_ar;
  CheatWarningMessage* m_cheats_disabled_gecko;

  enum
  {
    ID_NOTEBOOK = 1000,
    ID_GAMECONFIG,
    ID_PATCH_PAGE,
    ID_ARCODE_PAGE,
    ID_SPEEDHACK_PAGE,
    ID_INFORMATION,
    ID_FILESYSTEM,

    ID_USEDUALCORE,
    ID_MMU,
    ID_DCBZOFF,
    ID_FPRF,
    ID_SYNCGPU,
    ID_DISCSPEED,
    ID_AUDIO_DSP_HLE,
    ID_USE_BBOX,
    ID_ENABLEPROGRESSIVESCAN,
    ID_ENABLEWIDESCREEN,
    ID_EDITCONFIG,
    ID_SHOWDEFAULTCONFIG,
    ID_EMUSTATE,
    ID_EMU_ISSUES,
    ID_PATCHES_LIST,
    ID_EDITPATCH,
    ID_ADDPATCH,
    ID_REMOVEPATCH,
    ID_GPUDETERMINISM,
    ID_DEPTHPERCENTAGE,
    ID_CONVERGENCE,
    ID_MONODEPTH,
  };

  void LaunchExternalEditor(const std::string& filename, bool wait_until_closed);

  void CreateGUIControls();
  void OnClose(wxCloseEvent& event);
  void OnCloseClick(wxCommandEvent& event);
  void OnEditConfig(wxCommandEvent& event);
  void OnShowDefaultConfig(wxCommandEvent& event);
  void PatchListSelectionChanged(wxCommandEvent& event);
  void PatchButtonClicked(wxCommandEvent& event);
  void OnEmustateChanged(wxCommandEvent& event);
  void OnCheatCodeToggled(wxCommandEvent& event);
  void OnChangeTitle(wxCommandEvent& event);

  const GameListItem m_open_gamelist_item;

  IniFile m_gameini_default;
  IniFile m_gameini_local;
  std::string m_gameini_file_local;
  std::string m_game_id;

  std::set<std::string> m_default_patches;

  void LoadGameConfig();
  bool SaveGameConfig();
  void OnLocalIniModified(wxCommandEvent& ev);
  void GenerateLocalIniModified();
  void PatchList_Load();
  void PatchList_Save();

  long GetElementStyle(const char* section, const char* key);
  void SetCheckboxValueFromGameini(const char* section, const char* key, wxCheckBox* checkbox);
  void SaveGameIniValueFrom3StateCheckbox(const char* section, const char* key,
                                          wxCheckBox* checkbox);
};
