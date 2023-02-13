// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_foldersettingswidget.h"

class SettingsDialog;

class FolderSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  FolderSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~FolderSettingsWidget();

private:
  Ui::FolderSettingsWidget m_ui;
};
