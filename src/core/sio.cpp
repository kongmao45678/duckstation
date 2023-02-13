// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "sio.h"
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/log.h"
#include "controller.h"
#include "interrupt_controller.h"
#include "memory_card.h"
#include "util/state_wrapper.h"
#include <array>
#include <memory>
Log_SetChannel(SIO);

namespace SIO {

union SIO_CTRL
{
  u16 bits;

  BitField<u16, bool, 0, 1> TXEN;
  BitField<u16, bool, 1, 1> DTROUTPUT;
  BitField<u16, bool, 2, 1> RXEN;
  BitField<u16, bool, 3, 1> TXOUTPUT;
  BitField<u16, bool, 4, 1> ACK;
  BitField<u16, bool, 5, 1> RTSOUTPUT;
  BitField<u16, bool, 6, 1> RESET;
  BitField<u16, u8, 8, 2> RXIMODE;
  BitField<u16, bool, 10, 1> TXINTEN;
  BitField<u16, bool, 11, 1> RXINTEN;
  BitField<u16, bool, 12, 1> ACKINTEN;
};

union SIO_STAT
{
  u32 bits;

  BitField<u32, bool, 0, 1> TXRDY;
  BitField<u32, bool, 1, 1> RXFIFONEMPTY;
  BitField<u32, bool, 2, 1> TXDONE;
  BitField<u32, bool, 3, 1> RXPARITY;
  BitField<u32, bool, 4, 1> RXFIFOOVERRUN;
  BitField<u32, bool, 5, 1> RXBADSTOPBIT;
  BitField<u32, bool, 6, 1> RXINPUTLEVEL;
  BitField<u32, bool, 7, 1> DSRINPUTLEVEL;
  BitField<u32, bool, 8, 1> CTSINPUTLEVEL;
  BitField<u32, bool, 9, 1> INTR;
  BitField<u32, u32, 11, 15> TMR;
};

union SIO_MODE
{
  u16 bits;

  BitField<u16, u8, 0, 2> reload_factor;
  BitField<u16, u8, 2, 2> character_length;
  BitField<u16, bool, 4, 1> parity_enable;
  BitField<u16, u8, 5, 1> parity_type;
  BitField<u16, u8, 6, 2> stop_bit_length;
};

static void SoftReset();

static SIO_CTRL s_SIO_CTRL = {};
static SIO_STAT s_SIO_STAT = {};
static SIO_MODE s_SIO_MODE = {};
static u16 s_SIO_BAUD = 0;

} // namespace SIO

void SIO::Initialize()
{
  Reset();
}

void SIO::Shutdown() {}

void SIO::Reset()
{
  SoftReset();
}

bool SIO::DoState(StateWrapper& sw)
{
  sw.Do(&s_SIO_CTRL.bits);
  sw.Do(&s_SIO_STAT.bits);
  sw.Do(&s_SIO_MODE.bits);
  sw.Do(&s_SIO_BAUD);

  return !sw.HasError();
}

u32 SIO::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      Log_ErrorPrintf("Read SIO_DATA");

      const u8 value = 0xFF;
      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // SIO_STAT
    {
      const u32 bits = s_SIO_STAT.bits;
      return bits;
    }

    case 0x08: // SIO_MODE
      return ZeroExtend32(s_SIO_MODE.bits);

    case 0x0A: // SIO_CTRL
      return ZeroExtend32(s_SIO_CTRL.bits);

    case 0x0E: // SIO_BAUD
      return ZeroExtend32(s_SIO_BAUD);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void SIO::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      Log_WarningPrintf("SIO_DATA (W) <- 0x%02X", value);
      return;
    }

    case 0x0A: // SIO_CTRL
    {
      Log_DebugPrintf("SIO_CTRL <- 0x%04X", value);

      s_SIO_CTRL.bits = Truncate16(value);
      if (s_SIO_CTRL.RESET)
        SoftReset();

      return;
    }

    case 0x08: // SIO_MODE
    {
      Log_DebugPrintf("SIO_MODE <- 0x%08X", value);
      s_SIO_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DebugPrintf("SIO_BAUD <- 0x%08X", value);
      s_SIO_BAUD = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

void SIO::SoftReset()
{
  s_SIO_CTRL.bits = 0;
  s_SIO_STAT.bits = 0;
  s_SIO_STAT.DSRINPUTLEVEL = true;
  s_SIO_STAT.CTSINPUTLEVEL = true;
  s_SIO_STAT.TXDONE = true;
  s_SIO_STAT.TXRDY = true;
  s_SIO_MODE.bits = 0;
  s_SIO_BAUD = 0xDC;
}
