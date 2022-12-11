// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/GraphicsModListWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <set>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "DolphinQt/Config/GraphicsModWarningWidget.h"
#include "DolphinQt/Settings.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/VideoConfig.h"

GraphicsModListWidget::GraphicsModListWidget(const UICommon::GameFile& game)
    : m_game_id(game.GetGameID()), m_mod_group(m_game_id)
{
  CalculateGameRunning(Core::GetState());
  if (m_loaded_game_is_running && g_Config.graphics_mod_config)
  {
    m_mod_group.SetChangeCount(g_Config.graphics_mod_config->GetChangeCount());
  }
  CreateWidgets();
  ConnectWidgets();

  RefreshModList();
  OnModChanged(std::nullopt);
}

GraphicsModListWidget::~GraphicsModListWidget()
{
  if (m_needs_save)
  {
    m_mod_group.Save();
  }
}

void GraphicsModListWidget::CreateWidgets()
{
  auto* main_v_layout = new QVBoxLayout(this);

  auto* main_layout = new QHBoxLayout;

  auto* left_v_layout = new QVBoxLayout;

  m_mod_list = new QListWidget;
  m_mod_list->setSortingEnabled(false);
  m_mod_list->setSelectionBehavior(QAbstractItemView::SelectionBehavior::SelectItems);
  m_mod_list->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
  m_mod_list->setSelectionRectVisible(true);
  m_mod_list->setDragDropMode(QAbstractItemView::InternalMove);

  m_refresh = new QPushButton(tr("&Refresh List"));
  QHBoxLayout* hlayout = new QHBoxLayout;
  hlayout->addStretch();
  hlayout->addWidget(m_refresh);

  left_v_layout->addWidget(m_mod_list);
  left_v_layout->addLayout(hlayout);

  auto* right_v_layout = new QVBoxLayout;

  m_selected_mod_name = new QLabel();
  right_v_layout->addWidget(m_selected_mod_name);

  m_mod_meta_layout = new QVBoxLayout;
  right_v_layout->addLayout(m_mod_meta_layout);
  right_v_layout->addStretch();

  main_layout->addLayout(left_v_layout);
  main_layout->addLayout(right_v_layout, 1);

  m_warning = new GraphicsModWarningWidget(this);
  main_v_layout->addWidget(m_warning);
  main_v_layout->addLayout(main_layout);

  setLayout(main_v_layout);
}

void GraphicsModListWidget::ConnectWidgets()
{
  connect(m_warning, &GraphicsModWarningWidget::GraphicsModEnableSettings, this,
          &GraphicsModListWidget::OpenGraphicsSettings);

  connect(m_mod_list, &QListWidget::itemSelectionChanged, this,
          &GraphicsModListWidget::ModSelectionChanged);

  connect(m_mod_list, &QListWidget::itemChanged, this, &GraphicsModListWidget::ModItemChanged);

  connect(m_mod_list->model(), &QAbstractItemModel::rowsMoved, this,
          &GraphicsModListWidget::SaveModList);

  connect(m_refresh, &QPushButton::clicked, this, &GraphicsModListWidget::RefreshModList);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GraphicsModListWidget::CalculateGameRunning);
}

void GraphicsModListWidget::RefreshModList()
{
  if (m_needs_save)
  {
    SaveToDisk();
  }

  m_mod_list->setCurrentItem(nullptr);
  m_mod_list->clear();

  m_mod_group = GraphicsModGroupConfig(m_game_id);
  m_mod_group.Load();

  std::set<std::string> groups;

  for (const GraphicsModConfig& mod : m_mod_group.GetMods())
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
      groups.insert(group.m_name);
  }

  for (const GraphicsModConfig& mod : m_mod_group.GetMods())
  {
    // If no group matches the mod's features, or if the mod has no features, skip it
    if (std::none_of(mod.m_features.begin(), mod.m_features.end(),
                     [&groups](const GraphicsModFeatureConfig& feature) {
                       return groups.count(feature.m_group) == 1;
                     }))
    {
      continue;
    }

    QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(mod.m_title));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setData(Qt::UserRole, QString::fromStdString(mod.GetAbsolutePath()));
    item->setCheckState(mod.m_enabled ? Qt::Checked : Qt::Unchecked);

    m_mod_list->addItem(item);
  }
}

void GraphicsModListWidget::ModSelectionChanged()
{
  if (m_mod_list->currentItem() == nullptr)
    return;
  if (m_mod_list->count() == 0)
    return;
  const auto absolute_path = m_mod_list->currentItem()->data(Qt::UserRole).toString().toStdString();
  OnModChanged(absolute_path);
}

void GraphicsModListWidget::ModItemChanged(QListWidgetItem* item)
{
  const auto absolute_path = item->data(Qt::UserRole).toString();
  GraphicsModConfig* mod = m_mod_group.GetMod(absolute_path.toStdString());
  if (!mod)
    return;

  const bool was_enabled = mod->m_enabled;
  const bool should_enable = item->checkState() == Qt::Checked;
  mod->m_enabled = should_enable;
  if (was_enabled == should_enable)
    return;

  m_mod_group.SetChangeCount(m_mod_group.GetChangeCount() + 1);
  if (m_loaded_game_is_running)
  {
    g_Config.graphics_mod_config = m_mod_group;
  }
  m_needs_save = true;
}

void GraphicsModListWidget::OnModChanged(std::optional<std::string> absolute_path)
{
  ClearLayoutRecursively(m_mod_meta_layout);

  adjustSize();

  if (!absolute_path)
  {
    m_selected_mod_name->setText(QStringLiteral("No graphics mod selected"));
    m_selected_mod_name->setAlignment(Qt::AlignCenter);
    return;
  }

  GraphicsModConfig* mod = m_mod_group.GetMod(*absolute_path);
  if (!mod)
    return;

  m_selected_mod_name->setText(QString::fromStdString(mod->m_title));
  m_selected_mod_name->setAlignment(Qt::AlignLeft);
  QFont font = m_selected_mod_name->font();
  font.setWeight(QFont::Bold);
  m_selected_mod_name->setFont(font);

  if (!mod->m_author.empty())
  {
    auto* author_label = new QLabel(tr("By:  ") + QString::fromStdString(mod->m_author));
    m_mod_meta_layout->addWidget(author_label);
  }

  if (!mod->m_description.empty())
  {
    auto* description_label =
        new QLabel(tr("Description:  ") + QString::fromStdString(mod->m_description));
    description_label->setWordWrap(true);
    m_mod_meta_layout->addWidget(description_label);
  }
}

void GraphicsModListWidget::SaveModList()
{
  for (int i = 0; i < m_mod_list->count(); i++)
  {
    const auto absolute_path = m_mod_list->model()
                                   ->data(m_mod_list->model()->index(i, 0), Qt::UserRole)
                                   .toString()
                                   .toStdString();
    m_mod_group.GetMod(absolute_path)->m_weight = i;
  }

  if (m_loaded_game_is_running)
  {
    g_Config.graphics_mod_config = m_mod_group;
  }
  m_needs_save = true;
}

void GraphicsModListWidget::ClearLayoutRecursively(QLayout* layout)
{
  while (QLayoutItem* child = layout->takeAt(0))
  {
    if (child == nullptr)
      continue;

    if (child->widget())
    {
      layout->removeWidget(child->widget());
      delete child->widget();
    }
    else if (child->layout())
    {
      ClearLayoutRecursively(child->layout());
      layout->removeItem(child);
    }
    else
    {
      layout->removeItem(child);
    }
    delete child;
  }
}

void GraphicsModListWidget::SaveToDisk()
{
  m_needs_save = false;
  m_mod_group.Save();
}

const GraphicsModGroupConfig& GraphicsModListWidget::GetGraphicsModConfig() const
{
  return m_mod_group;
}

void GraphicsModListWidget::CalculateGameRunning(Core::State state)
{
  m_loaded_game_is_running =
      state == Core::State::Running ? m_game_id == SConfig::GetInstance().GetGameID() : false;
}
