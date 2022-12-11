// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/SI/SI_DeviceGCAdapter.h"

#include <cstring>

#include "Common/CommonTypes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/NetPlayProto.h"
#include "InputCommon/GCAdapter.h"

namespace SerialInterface
{
CSIDevice_GCAdapter::CSIDevice_GCAdapter(SIDevices device, int device_number)
    : CSIDevice_GCController(device, device_number)
{
  // get the correct pad number that should rumble locally when using netplay
  const int pad_num = NetPlay_InGamePadToLocalPad(m_device_number);
  if (pad_num < 4)
    m_simulate_konga = SConfig::GetInstance().m_AdapterKonga[pad_num];
}

GCPadStatus CSIDevice_GCAdapter::GetPadStatus()
{
  GCPadStatus pad_status = {};

  // For netplay, the local controllers are polled in GetNetPads(), and
  // the remote controllers receive their status there as well
  if (!NetPlay::IsNetPlayRunning())
  {
    pad_status = GCAdapter::Input(m_device_number);
  }

  HandleMoviePadStatus(&pad_status);

  return pad_status;
}

int CSIDevice_GCAdapter::RunBuffer(u8* buffer, int length)
{
  if (!Core::WantsDeterminism())
  {
    // The previous check is a hack to prevent a desync due to SI devices
    // being different and returning different values on RunBuffer();
    // the corresponding code in GCAdapter.cpp has the same check.

    // This returns an error value if there is no controller plugged
    // into this port on the hardware gc adapter, exposing it to the game.
    if (!GCAdapter::DeviceConnected(m_device_number))
    {
      TSIDevices device = SI_NONE;
      memcpy(buffer, &device, sizeof(device));
      return 4;
    }
  }
  return CSIDevice_GCController::RunBuffer(buffer, length);
}

void CSIDevice_GCController::Rumble(int pad_num, ControlState strength)
{
  SIDevices device = SConfig::GetInstance().m_SIDevice[pad_num];
  if (device == SIDEVICE_WIIU_ADAPTER)
    GCAdapter::Output(pad_num, static_cast<u8>(strength));
  else if (SIDevice_IsGCController(device))
    Pad::Rumble(pad_num, strength);
}
}  // namespace SerialInterface
