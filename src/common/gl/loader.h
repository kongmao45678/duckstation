// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

// Fix glad.h including windows.h
#ifdef _WIN32
#include "../windows_headers.h"
#endif

#include "glad.h"