// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_displaysettingswidget.h"

class PostProcessingChainConfigWidget;
class SettingsDialog;

class DisplaySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  DisplaySettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~DisplaySettingsWidget();

private Q_SLOTS:
  void populateGPUAdaptersAndResolutions();
  void onGPUAdapterIndexChanged();
  void onGPUFullscreenModeIndexChanged();
  void onIntegerFilteringChanged();
  void onAspectRatioChanged();

private:
  void setupAdditionalUi();

  Ui::DisplaySettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
