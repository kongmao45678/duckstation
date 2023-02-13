// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <QtWidgets/QWidget>
#include "ui_achievementsettingswidget.h"

class SettingsDialog;

class AchievementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AchievementSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~AchievementSettingsWidget();

private Q_SLOTS:
  void updateEnableState();
  void onChallengeModeStateChanged();
  void onLoginLogoutPressed();
  void onViewProfilePressed();
  void onAchievementsRefreshed(quint32 id, const QString& game_info_string, quint32 total, quint32 points);

private:
  void updateLoginState();

  Ui::AchievementSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
