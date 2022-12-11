// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "VideoCommon/VideoCommon.h"

class PostProcessingShaderConfiguration
{
public:
  struct ConfigurationOption
  {
    enum OptionType
    {
      OPTION_BOOL = 0,
      OPTION_FLOAT,
      OPTION_INTEGER,
    };

    bool m_bool_value;

    std::vector<float> m_float_values;
    std::vector<s32> m_integer_values;

    std::vector<float> m_float_min_values;
    std::vector<s32> m_integer_min_values;

    std::vector<float> m_float_max_values;
    std::vector<s32> m_integer_max_values;

    std::vector<float> m_float_step_values;
    std::vector<s32> m_integer_step_values;

    OptionType m_type;

    std::string m_gui_name;
    std::string m_option_name;
    std::string m_dependent_option;
    bool m_dirty;
  };

  typedef std::map<std::string, ConfigurationOption> ConfigMap;

  PostProcessingShaderConfiguration() : m_current_shader("") {}
  virtual ~PostProcessingShaderConfiguration() {}
  // Loads the configuration with a shader
  // If the argument is "" the class will load the shader from the g_activeConfig option.
  // Returns the loaded shader source from file
  std::string LoadShader(std::string shader = "");
  void SaveOptionsConfiguration();
  void ReloadShader();
  std::string GetShader() { return m_current_shader; }
  bool IsDirty() { return m_any_options_dirty; }
  void SetDirty(bool dirty) { m_any_options_dirty = dirty; }
  bool HasOptions() { return m_options.size() > 0; }
  const ConfigMap& GetOptions() const { return m_options; }
  ConfigMap& GetOptions() { return m_options; }
  const ConfigurationOption& GetOption(const std::string& option) { return m_options[option]; }
  // For updating option's values
  void SetOptionf(const std::string& option, int index, float value);
  void SetOptioni(const std::string& option, int index, s32 value);
  void SetOptionb(const std::string& option, bool value);

private:
  bool m_any_options_dirty;
  std::string m_current_shader;
  ConfigMap m_options;

  void LoadOptions(const std::string& code);
  void LoadOptionsConfiguration();
};

class PostProcessingShaderImplementation
{
public:
  PostProcessingShaderImplementation();
  virtual ~PostProcessingShaderImplementation();

  static std::vector<std::string> GetShaderList(APIType api_type);
  static std::vector<std::string> GetAnaglyphShaderList(APIType api_type);

  PostProcessingShaderConfiguration* GetConfig() { return &m_config; }
protected:
  // Timer for determining our time value
  Common::Timer m_timer;

  PostProcessingShaderConfiguration m_config;
};
