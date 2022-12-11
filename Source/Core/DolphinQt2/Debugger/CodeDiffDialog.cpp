// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/CodeDiffDialog.h"

#include <algorithm>
#include <string>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Profiler.h"

#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

CodeDiffDialog::CodeDiffDialog(CodeWidget* parent) : QDialog(parent), m_code_widget(parent)
{
  setWindowTitle(tr("Code Diff Tool"));
  CreateWidgets();
  auto& settings = Settings::GetQSettings();
  restoreGeometry(settings.value(QStringLiteral("diffdialog/geometry")).toByteArray());
  ConnectWidgets();
}

void CodeDiffDialog::reject()
{
  ClearData();
  auto& settings = Settings::GetQSettings();
  settings.setValue(QStringLiteral("diffdialog/geometry"), saveGeometry());
  QDialog::reject();
}

void CodeDiffDialog::CreateWidgets()
{
  auto* btns_layout = new QGridLayout;
  m_exclude_btn = new QPushButton(tr("Code did not get executed"));
  m_include_btn = new QPushButton(tr("Code has been executed"));
  m_record_btn = new QPushButton(tr("Start Recording"));
  m_record_btn->setCheckable(true);
  m_record_btn->setStyleSheet(
      QStringLiteral("QPushButton:checked { background-color: rgb(150, 0, 0); border-style: solid; "
                     "border-width: 3px; border-color: rgb(150,0,0); color: rgb(255, 255, 255);}"));

  m_exclude_btn->setEnabled(false);
  m_include_btn->setEnabled(false);

  btns_layout->addWidget(m_exclude_btn, 0, 0);
  btns_layout->addWidget(m_include_btn, 0, 1);
  btns_layout->addWidget(m_record_btn, 0, 2);

  auto* labels_layout = new QHBoxLayout;
  m_exclude_size_label = new QLabel(tr("Excluded: 0"));
  m_include_size_label = new QLabel(tr("Included: 0"));

  btns_layout->addWidget(m_exclude_size_label, 1, 0);
  btns_layout->addWidget(m_include_size_label, 1, 1);

  m_matching_results_table = new QTableWidget();
  m_matching_results_table->setColumnCount(5);
  m_matching_results_table->setHorizontalHeaderLabels(
      {tr("Address"), tr("Total Hits"), tr("Hits"), tr("Symbol"), tr("Inspected")});
  m_matching_results_table->setSelectionMode(QAbstractItemView::SingleSelection);
  m_matching_results_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_matching_results_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_matching_results_table->setContextMenuPolicy(Qt::CustomContextMenu);
  m_matching_results_table->setColumnWidth(0, 60);
  m_matching_results_table->setColumnWidth(1, 60);
  m_matching_results_table->setColumnWidth(2, 4);
  m_matching_results_table->setColumnWidth(3, 210);
  m_matching_results_table->setColumnWidth(4, 65);
  m_reset_btn = new QPushButton(tr("Reset All"));
  m_reset_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  m_help_btn = new QPushButton(tr("Help"));
  m_help_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto* help_reset_layout = new QHBoxLayout;
  help_reset_layout->addWidget(m_reset_btn, 0, Qt::AlignLeft);
  help_reset_layout->addWidget(m_help_btn, 0, Qt::AlignRight);

  auto* layout = new QVBoxLayout();
  layout->addLayout(btns_layout);
  layout->addLayout(labels_layout);
  layout->addWidget(m_matching_results_table);
  layout->addLayout(help_reset_layout);

  setLayout(layout);
  resize(515, 400);
}

void CodeDiffDialog::ConnectWidgets()
{
  connect(m_record_btn, &QPushButton::toggled, this, &CodeDiffDialog::OnRecord);
  connect(m_include_btn, &QPushButton::pressed, [this]() { Update(true); });
  connect(m_exclude_btn, &QPushButton::pressed, [this]() { Update(false); });
  connect(m_matching_results_table, &QTableWidget::itemClicked, [this]() { OnClickItem(); });
  connect(m_reset_btn, &QPushButton::pressed, this, &CodeDiffDialog::ClearData);
  connect(m_help_btn, &QPushButton::pressed, this, &CodeDiffDialog::InfoDisp);
  connect(m_matching_results_table, &CodeDiffDialog::customContextMenuRequested, this,
          &CodeDiffDialog::OnContextMenu);
}

void CodeDiffDialog::OnClickItem()
{
  UpdateItem();
  auto address = m_matching_results_table->currentItem()->data(Qt::UserRole).toUInt();
  m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeDiffDialog::ClearData()
{
  if (m_record_btn->isChecked())
    m_record_btn->toggle();
  ClearBlockCache();
  m_matching_results_table->clear();
  m_matching_results_table->setRowCount(0);
  m_matching_results_table->setHorizontalHeaderLabels(
      {tr("Address"), tr("Total Hits"), tr("Hits"), tr("Symbol"), tr("Inspected")});
  m_matching_results_table->setEditTriggers(QAbstractItemView::EditTrigger::NoEditTriggers);
  m_exclude_size_label->setText(tr("Excluded: %1").arg(0));
  m_include_size_label->setText(tr("Included: %1").arg(0));
  m_exclude_btn->setEnabled(false);
  m_include_btn->setEnabled(false);
  m_include_active = false;
  // Swap is used instead of clear for efficiency in the case of huge m_include/m_exclude
  std::vector<Diff>().swap(m_include);
  std::vector<Diff>().swap(m_exclude);
  JitInterface::SetProfilingState(JitInterface::ProfilingState::Disabled);
}

void CodeDiffDialog::ClearBlockCache()
{
  Core::State old_state = Core::GetState();

  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Paused);

  JitInterface::ClearCache();

  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Running);
}

void CodeDiffDialog::OnRecord(bool enabled)
{
  if (m_failed_requirements)
  {
    m_failed_requirements = false;
    return;
  }

  if (Core::GetState() == Core::State::Uninitialized)
  {
    ModalMessageBox::information(this, tr("Code Diff Tool"),
                                 tr("Emulation must be started to record."));
    m_failed_requirements = true;
    m_record_btn->setChecked(false);
    return;
  }

  if (g_symbolDB.IsEmpty())
  {
    ModalMessageBox::warning(
        this, tr("Code Diff Tool"),
        tr("Symbol map not found.\n\nIf one does not exist, you can generate one from "
           "the Menu bar:\nSymbols -> Generate Symbols From ->\n\tAddress | Signature "
           "Database | RSO Modules"));
    m_failed_requirements = true;
    m_record_btn->setChecked(false);
    return;
  }

  JitInterface::ProfilingState state;

  if (enabled)
  {
    ClearBlockCache();
    m_record_btn->setText(tr("Stop Recording"));
    state = JitInterface::ProfilingState::Enabled;
    m_exclude_btn->setEnabled(true);
    m_include_btn->setEnabled(true);
  }
  else
  {
    ClearBlockCache();
    m_record_btn->setText(tr("Start Recording"));
    state = JitInterface::ProfilingState::Disabled;
    m_exclude_btn->setEnabled(false);
    m_include_btn->setEnabled(false);
  }

  m_record_btn->update();
  JitInterface::SetProfilingState(state);
}

void CodeDiffDialog::OnInclude()
{
  const auto recorded_symbols = CalculateSymbolsFromProfile();

  if (recorded_symbols.empty())
    return;

  if (m_include.empty() && m_exclude.empty())
  {
    m_include = recorded_symbols;
    m_include_active = true;
  }
  else if (m_include.empty())
  {
    // If include becomes empty after having items on it, don't refill it until after a reset.
    if (m_include_active)
      return;

    // If we are building include for the first time and we have an exlcude list, then include =
    // recorded - excluded.
    m_include = recorded_symbols;
    RemoveMatchingSymbolsFromIncludes(m_exclude);
    m_include_active = true;
  }
  else
  {
    // If include already exists, keep items that are in both include and recorded. Exclude list
    // becomes irrelevant.
    RemoveMissingSymbolsFromIncludes(recorded_symbols);
  }
}

void CodeDiffDialog::OnExclude()
{
  const auto recorded_symbols = CalculateSymbolsFromProfile();
  if (m_include.empty() && m_exclude.empty())
  {
    m_exclude = recorded_symbols;
  }
  else if (m_include.empty())
  {
    // If there is only an exclude list, update it.
    for (auto& iter : recorded_symbols)
    {
      auto pos = std::lower_bound(m_exclude.begin(), m_exclude.end(), iter.symbol);

      if (pos == m_exclude.end() || pos->symbol != iter.symbol)
        m_exclude.insert(pos, iter);
    }
  }
  else
  {
    // If include already exists, the exclude list will have been used to trim it, so the exclude
    // list is now irrelevant, as anythng not on the include list is effectively excluded.
    // Exclude/subtract recorded items from the include list.
    RemoveMatchingSymbolsFromIncludes(recorded_symbols);
  }
}

std::vector<Diff> CodeDiffDialog::CalculateSymbolsFromProfile()
{
  Profiler::ProfileStats prof_stats;
  auto& blockstats = prof_stats.block_stats;
  JitInterface::GetProfileResults(&prof_stats);
  std::vector<Diff> current;
  current.reserve(20000);

  // Convert blockstats to smaller struct Diff. Exclude repeat functions via symbols.
  for (auto& iter : blockstats)
  {
    Diff tmp_diff;
    std::string symbol = g_symbolDB.GetDescription(iter.addr);
    if (!std::any_of(current.begin(), current.end(),
                     [&symbol](Diff& v) { return v.symbol == symbol; }))
    {
      tmp_diff.symbol = symbol;
      tmp_diff.addr = iter.addr;
      tmp_diff.hits = iter.run_count;
      tmp_diff.total_hits = iter.run_count;
      current.push_back(tmp_diff);
    }
  }

  sort(current.begin(), current.end(),
       [](const Diff& v1, const Diff& v2) { return (v1.symbol < v2.symbol); });

  return current;
}

void CodeDiffDialog::RemoveMissingSymbolsFromIncludes(const std::vector<Diff>& symbol_diff)
{
  m_include.erase(std::remove_if(m_include.begin(), m_include.end(),
                                 [&](const Diff& v) {
                                   auto arg = std::none_of(
                                       symbol_diff.begin(), symbol_diff.end(), [&](const Diff& p) {
                                         return p.symbol == v.symbol || p.addr == v.addr;
                                       });
                                   return arg;
                                 }),
                  m_include.end());
  for (auto& original_includes : m_include)
  {
    auto pos = std::lower_bound(symbol_diff.begin(), symbol_diff.end(), original_includes.symbol);
    if (pos != symbol_diff.end() &&
        (pos->symbol == original_includes.symbol || pos->addr == original_includes.addr))
    {
      original_includes.total_hits += pos->hits;
      original_includes.hits = pos->hits;
    }
  }
}

void CodeDiffDialog::RemoveMatchingSymbolsFromIncludes(const std::vector<Diff>& symbol_list)
{
  m_include.erase(std::remove_if(m_include.begin(), m_include.end(),
                                 [&](const Diff& i) {
                                   return std::any_of(
                                       symbol_list.begin(), symbol_list.end(), [&](const Diff& s) {
                                         return i.symbol == s.symbol || i.addr == s.addr;
                                       });
                                 }),
                  m_include.end());
}

void CodeDiffDialog::Update(bool include)
{
  // Wrap everything in a pause
  Core::State old_state = Core::GetState();
  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Paused);

  // Main process
  if (include)
  {
    OnInclude();
  }
  else
  {
    OnExclude();
  }

  const auto create_item = [](const QString string = {}, const u32 address = 0x00000000) {
    QTableWidgetItem* item = new QTableWidgetItem(string);
    item->setData(Qt::UserRole, address);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
  };

  int i = 0;
  m_matching_results_table->clear();
  m_matching_results_table->setRowCount(i);
  m_matching_results_table->setHorizontalHeaderLabels(
      {tr("Address"), tr("Total Hits"), tr("Hits"), tr("Symbol"), tr("Inspected")});

  for (auto& iter : m_include)
  {
    m_matching_results_table->setRowCount(i + 1);

    QString fix_sym = QString::fromStdString(iter.symbol);
    fix_sym.replace(QStringLiteral("\t"), QStringLiteral("  "));

    m_matching_results_table->setItem(
        i, 0, create_item(QStringLiteral("%1").arg(iter.addr, 1, 16), iter.addr));
    m_matching_results_table->setItem(
        i, 1, create_item(QStringLiteral("%1").arg(iter.total_hits), iter.addr));
    m_matching_results_table->setItem(i, 2,
                                      create_item(QStringLiteral("%1").arg(iter.hits), iter.addr));
    m_matching_results_table->setItem(i, 3,
                                      create_item(QStringLiteral("%1").arg(fix_sym), iter.addr));
    m_matching_results_table->setItem(i, 4, create_item(QStringLiteral(""), iter.addr));
    i++;
  }

  // If we have ruled out all functions from being included.
  if (m_include_active && m_include.empty())
  {
    m_matching_results_table->setRowCount(1);
    m_matching_results_table->setItem(0, 3, create_item(tr("No possible functions left. Reset.")));
  }

  m_exclude_size_label->setText(tr("Excluded: %1").arg(m_exclude.size()));
  m_include_size_label->setText(tr("Included: %1").arg(m_include.size()));

  JitInterface::ClearCache();
  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Running);
}

void CodeDiffDialog::InfoDisp()
{
  ModalMessageBox::information(
      this, tr("Code Diff Tool Help"),
      tr("Used to find functions based on when they should be running.\nSimilar to Cheat Engine "
         "Ultimap.\n"
         "A symbol map must be loaded prior to use.\n"
         "Include/Exclude lists will persist on ending/restarting emulation.\nThese lists "
         "will not persist on Dolphin close."
         "\n\n'Start Recording': "
         "keeps track of what functions run.\n'Stop Recording': erases current "
         "recording without any change to the lists.\n'Code did not get executed': click while "
         "recording, will add recorded functions to an exclude "
         "list, then reset the recording list.\n'Code has been executed': click while recording, "
         "will add recorded function to an include list, then reset the recording list.\n\nAfter "
         "you use "
         "both exclude and include once, the exclude list will be subtracted from the include "
         "list "
         "and any includes left over will be displayed.\nYou can continue to use "
         "'Code did not get executed'/'Code has been executed' to narrow down the "
         "results."));
  ModalMessageBox::information(
      this, tr("Code Diff Tool Help"),
      tr("Example:\n"
         "You want to find a function that runs when HP is modified.\n1. Start recording and "
         "play the game without letting HP be modified, then press 'Code did not get "
         "executed'.\n2. Immediately gain/lose HP and press 'Code has been executed'.\n3. Repeat "
         "1 or 2 to "
         "narrow down the results.\nIncludes (Code has been executed) should "
         "have short recordings focusing on what you want.\n\nPressing 'Code has been "
         "executed' twice will only keep functions that ran for both recordings. Hits will update "
         "to reflect the last recording's "
         "number of Hits. Total Hits will reflect the total number of "
         "times a function has been executed until the lists are cleared with Reset.\n\nRight "
         "click -> 'Set blr' will place a "
         "blr at the top of the symbol.\n"));
}

void CodeDiffDialog::OnContextMenu()
{
  if (m_matching_results_table->currentItem() == nullptr)
    return;
  UpdateItem();
  QMenu* menu = new QMenu(this);
  menu->addAction(tr("&Go to start of function"), this, &CodeDiffDialog::OnGoTop);
  menu->addAction(tr("Set &blr"), this, &CodeDiffDialog::OnSetBLR);
  menu->addAction(tr("&Delete"), this, &CodeDiffDialog::OnDelete);
  menu->exec(QCursor::pos());
}

void CodeDiffDialog::OnGoTop()
{
  auto item = m_matching_results_table->currentItem();
  if (!item)
    return;
  Common::Symbol* symbol = g_symbolDB.GetSymbolFromAddr(item->data(Qt::UserRole).toUInt());
  if (!symbol)
    return;
  m_code_widget->SetAddress(symbol->address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeDiffDialog::OnDelete()
{
  // Delete from include list and qtable widget
  auto item = m_matching_results_table->currentItem();
  if (!item)
    return;
  int row = m_matching_results_table->row(item);
  if (row == -1)
    return;
  // TODO: If/when sorting is ever added, .erase needs to find item position instead; leaving as is
  // for performance
  if (!m_include.empty())
  {
    m_include.erase(m_include.begin() + row);
  }
  m_matching_results_table->removeRow(row);
}

void CodeDiffDialog::OnSetBLR()
{
  auto item = m_matching_results_table->currentItem();
  if (!item)
    return;

  Common::Symbol* symbol = g_symbolDB.GetSymbolFromAddr(item->data(Qt::UserRole).toUInt());
  if (!symbol)
    return;
  PowerPC::debug_interface.SetPatch(symbol->address, 0x4E800020);

  int row = item->row();
  m_matching_results_table->item(row, 0)->setForeground(QBrush(Qt::red));
  m_matching_results_table->item(row, 1)->setForeground(QBrush(Qt::red));
  m_matching_results_table->item(row, 2)->setForeground(QBrush(Qt::red));
  m_matching_results_table->item(row, 3)->setForeground(QBrush(Qt::red));
  m_matching_results_table->item(row, 4)->setForeground(QBrush(Qt::red));
  m_matching_results_table->item(row, 4)->setText(QStringLiteral("X"));

  m_code_widget->Update();
}

void CodeDiffDialog::UpdateItem()
{
  QTableWidgetItem* item = m_matching_results_table->currentItem();
  if (!item)
    return;

  int row = m_matching_results_table->row(item);
  if (row == -1)
    return;
  uint address = item->data(Qt::UserRole).toUInt();

  auto symbolName = g_symbolDB.GetDescription(address);
  if (symbolName == " --- ")
    return;

  QString newName =
      QString::fromStdString(symbolName).replace(QStringLiteral("\t"), QStringLiteral("  "));
  m_matching_results_table->item(row, 3)->setText(newName);
}
