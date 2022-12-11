// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"

#include "Common/Config/Config.h"

class GraphicsChoice : public ToolTipComboBox
{
  Q_OBJECT
public:
  GraphicsChoice(const QStringList& options, const Config::Info<int>& setting);

private:
  void Update(int choice);

  Config::Info<int> m_setting;
};
