// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtCore/QSemaphore>
#include <QtCore/QThread>

#include "common/progress_callback.h"
#include "common/timer.h"

class GameListRefreshThread;

class AsyncRefreshProgressCallback : public BaseProgressCallback
{
public:
  AsyncRefreshProgressCallback(GameListRefreshThread* parent);

  void Cancel();

  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;
  void SetTitle(const char* title) override;
  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;
  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  void ModalInformation(const char* message) override;

private:
  void fireUpdate();

  GameListRefreshThread* m_parent;
  Common::Timer m_last_update_time;
  QString m_status_text;
  int m_last_range = 1;
  int m_last_value = 0;
};

class GameListRefreshThread final : public QThread
{
  Q_OBJECT

public:
  GameListRefreshThread(bool invalidate_cache);
  ~GameListRefreshThread();

  void cancel();

Q_SIGNALS:
  void refreshProgress(const QString& status, int current, int total);
  void refreshComplete();

protected:
  void run();

private:
  AsyncRefreshProgressCallback m_progress;
  bool m_invalidate_cache;
};
