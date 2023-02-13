// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "dma.h"
#include "bus.h"
#include "cdrom.h"
#include "common/bitfield.h"
#include "common/log.h"
#include "common/string_util.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "gpu.h"
#include "host.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <array>
#include <memory>
#include <vector>
Log_SetChannel(DMA);

namespace DMA {
enum class SyncMode : u32
{
  Manual = 0,
  Request = 1,
  LinkedList = 2,
  Reserved = 3
};

static constexpr PhysicalMemoryAddress BASE_ADDRESS_MASK = UINT32_C(0x00FFFFFF);
static constexpr PhysicalMemoryAddress ADDRESS_MASK = UINT32_C(0x001FFFFC);

static u32 GetAddressMask();
static void ClearState();

// is everything enabled for a channel to operate?
static bool CanTransferChannel(Channel channel, bool ignore_halt);
static bool IsTransferHalted();
static void UpdateIRQ();

// returns false if the DMA should now be halted
static TickCount GetTransferSliceTicks();
static TickCount GetTransferHaltTicks();
static bool TransferChannel(Channel channel);
static void HaltTransfer(TickCount duration);
static void UnhaltTransfer(void*, TickCount ticks, TickCount ticks_late);

// from device -> memory
static TickCount TransferDeviceToMemory(Channel channel, u32 address, u32 increment, u32 word_count);

// from memory -> device
static TickCount TransferMemoryToDevice(Channel channel, u32 address, u32 increment, u32 word_count);

// configuration
static TickCount s_max_slice_ticks = 1000;
static TickCount s_halt_ticks = 100;

static std::vector<u32> s_transfer_buffer;
static std::unique_ptr<TimingEvent> s_unhalt_event;
static TickCount s_halt_ticks_remaining = 0;

struct ChannelState
{
  u32 base_address = 0;

  union BlockControl
  {
    u32 bits;
    union
    {
      BitField<u32, u32, 0, 16> word_count;

      u32 GetWordCount() const { return (word_count == 0) ? 0x10000 : word_count; }
    } manual;
    union
    {
      BitField<u32, u32, 0, 16> block_size;
      BitField<u32, u32, 16, 16> block_count;

      u32 GetBlockSize() const { return (block_size == 0) ? 0x10000 : block_size; }
      u32 GetBlockCount() const { return (block_count == 0) ? 0x10000 : block_count; }
    } request;
  } block_control = {};

  union ChannelControl
  {
    u32 bits;
    BitField<u32, bool, 0, 1> copy_to_device;
    BitField<u32, bool, 1, 1> address_step_reverse;
    BitField<u32, bool, 8, 1> chopping_enable;
    BitField<u32, SyncMode, 9, 2> sync_mode;
    BitField<u32, u32, 16, 3> chopping_dma_window_size;
    BitField<u32, u32, 20, 3> chopping_cpu_window_size;
    BitField<u32, bool, 24, 1> enable_busy;
    BitField<u32, bool, 28, 1> start_trigger;

    static constexpr u32 WRITE_MASK = 0b01110001'01110111'00000111'00000011;
  } channel_control = {};

  bool request = false;
};

static std::array<ChannelState, NUM_CHANNELS> s_state;

union DPCR
{
  u32 bits;

  BitField<u32, u8, 0, 3> MDECin_priority;
  BitField<u32, bool, 3, 1> MDECin_master_enable;
  BitField<u32, u8, 4, 3> MDECout_priority;
  BitField<u32, bool, 7, 1> MDECout_master_enable;
  BitField<u32, u8, 8, 3> GPU_priority;
  BitField<u32, bool, 10, 1> GPU_master_enable;
  BitField<u32, u8, 12, 3> CDROM_priority;
  BitField<u32, bool, 15, 1> CDROM_master_enable;
  BitField<u32, u8, 16, 3> SPU_priority;
  BitField<u32, bool, 19, 1> SPU_master_enable;
  BitField<u32, u8, 20, 3> PIO_priority;
  BitField<u32, bool, 23, 1> PIO_master_enable;
  BitField<u32, u8, 24, 3> OTC_priority;
  BitField<u32, bool, 27, 1> OTC_master_enable;
  BitField<u32, u8, 28, 3> priority_offset;
  BitField<u32, bool, 31, 1> unused;

  u8 GetPriority(Channel channel) const { return ((bits >> (static_cast<u8>(channel) * 4)) & u32(3)); }
  bool GetMasterEnable(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) * 4 + 3)) & u32(1));
  }
};

static DPCR s_DPCR = {};

static constexpr u32 DICR_WRITE_MASK = 0b00000000'11111111'10000000'00111111;
static constexpr u32 DICR_RESET_MASK = 0b01111111'00000000'00000000'00000000;
union DICR
{
  u32 bits;

  BitField<u32, bool, 15, 1> force_irq;
  BitField<u32, bool, 16, 1> MDECin_irq_enable;
  BitField<u32, bool, 17, 1> MDECout_irq_enable;
  BitField<u32, bool, 18, 1> GPU_irq_enable;
  BitField<u32, bool, 19, 1> CDROM_irq_enable;
  BitField<u32, bool, 20, 1> SPU_irq_enable;
  BitField<u32, bool, 21, 1> PIO_irq_enable;
  BitField<u32, bool, 22, 1> OTC_irq_enable;
  BitField<u32, bool, 23, 1> master_enable;
  BitField<u32, bool, 24, 1> MDECin_irq_flag;
  BitField<u32, bool, 25, 1> MDECout_irq_flag;
  BitField<u32, bool, 26, 1> GPU_irq_flag;
  BitField<u32, bool, 27, 1> CDROM_irq_flag;
  BitField<u32, bool, 28, 1> SPU_irq_flag;
  BitField<u32, bool, 29, 1> PIO_irq_flag;
  BitField<u32, bool, 30, 1> OTC_irq_flag;
  BitField<u32, bool, 31, 1> master_flag;

  bool IsIRQEnabled(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 16)) & u32(1));
  }

  bool GetIRQFlag(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 24)) & u32(1));
  }

  void SetIRQFlag(Channel channel) { bits |= (u32(1) << (static_cast<u8>(channel) + 24)); }
  void ClearIRQFlag(Channel channel) { bits &= ~(u32(1) << (static_cast<u8>(channel) + 24)); }

  void UpdateMasterFlag()
  {
    master_flag = master_enable && ((((bits >> 16) & u32(0b1111111)) & ((bits >> 24) & u32(0b1111111))) != 0);
  }
};

static DICR s_DICR = {};
}; // namespace DMA

u32 DMA::GetAddressMask()
{
  return Bus::g_ram_mask & 0xFFFFFFFCu;
}

void DMA::Initialize()
{
  s_max_slice_ticks = g_settings.dma_max_slice_ticks;
  s_halt_ticks = g_settings.dma_halt_ticks;

  s_transfer_buffer.resize(32);
  s_unhalt_event =
    TimingEvents::CreateTimingEvent("DMA Transfer Unhalt", 1, s_max_slice_ticks, &DMA::UnhaltTransfer, nullptr, false);
  Reset();
}

void DMA::Shutdown()
{
  ClearState();
  s_unhalt_event.reset();
}

void DMA::Reset()
{
  ClearState();
  s_unhalt_event->Deactivate();
}

void DMA::ClearState()
{
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state[i];
    cs.base_address = 0;
    cs.block_control.bits = 0;
    cs.channel_control.bits = 0;
    cs.request = false;
  }

  s_DPCR.bits = 0x07654321;
  s_DICR.bits = 0;

  s_halt_ticks_remaining = 0;
}

bool DMA::DoState(StateWrapper& sw)
{
  sw.Do(&s_halt_ticks_remaining);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state[i];
    sw.Do(&cs.base_address);
    sw.Do(&cs.block_control.bits);
    sw.Do(&cs.channel_control.bits);
    sw.Do(&cs.request);
  }

  sw.Do(&s_DPCR.bits);
  sw.Do(&s_DICR.bits);

  if (sw.IsReading())
  {
    if (s_halt_ticks_remaining > 0)
      s_unhalt_event->SetIntervalAndSchedule(s_halt_ticks_remaining);
    else
      s_unhalt_event->Deactivate();
  }

  return !sw.HasError();
}

u32 DMA::ReadRegister(u32 offset)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        Log_TracePrintf("DMA%u base address -> 0x%08X", channel_index, s_state[channel_index].base_address);
        return s_state[channel_index].base_address;
      }
      case 0x04:
      {
        Log_TracePrintf("DMA%u block control -> 0x%08X", channel_index, s_state[channel_index].block_control.bits);
        return s_state[channel_index].block_control.bits;
      }
      case 0x08:
      {
        Log_TracePrintf("DMA%u channel control -> 0x%08X", channel_index, s_state[channel_index].channel_control.bits);
        return s_state[channel_index].channel_control.bits;
      }
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
    {
      Log_TracePrintf("DPCR -> 0x%08X", s_DPCR.bits);
      return s_DPCR.bits;
    }
    else if (offset == 0x74)
    {
      Log_TracePrintf("DPCR -> 0x%08X", s_DPCR.bits);
      return s_DICR.bits;
    }
  }

  Log_ErrorPrintf("Unhandled register read: %02X", offset);
  return UINT32_C(0xFFFFFFFF);
}

void DMA::WriteRegister(u32 offset, u32 value)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    ChannelState& state = s_state[channel_index];
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        state.base_address = value & BASE_ADDRESS_MASK;
        Log_TracePrintf("DMA channel %u base address <- 0x%08X", channel_index, state.base_address);
        return;
      }
      case 0x04:
      {
        Log_TracePrintf("DMA channel %u block control <- 0x%08X", channel_index, value);
        state.block_control.bits = value;
        return;
      }

      case 0x08:
      {
        // HACK: Due to running DMA in slices, we can't wait for the current halt time to finish before running the
        // first block of a new channel. This affects games like FF8, where they kick a SPU transfer while a GPU
        // transfer is happening, and the SPU transfer gets delayed until the GPU transfer unhalts and finishes, and
        // breaks the interrupt.
        const bool ignore_halt = !state.channel_control.enable_busy && (value & (1u << 24));

        state.channel_control.bits = (state.channel_control.bits & ~ChannelState::ChannelControl::WRITE_MASK) |
                                     (value & ChannelState::ChannelControl::WRITE_MASK);
        Log_TracePrintf("DMA channel %u channel control <- 0x%08X", channel_index, state.channel_control.bits);

        // start/trigger bit must be enabled for OTC
        if (static_cast<Channel>(channel_index) == Channel::OTC)
          SetRequest(static_cast<Channel>(channel_index), state.channel_control.start_trigger);

        if (CanTransferChannel(static_cast<Channel>(channel_index), ignore_halt))
          TransferChannel(static_cast<Channel>(channel_index));
        return;
      }

      default:
        break;
    }
  }
  else
  {
    switch (offset)
    {
      case 0x70:
      {
        Log_TracePrintf("DPCR <- 0x%08X", value);
        s_DPCR.bits = value;

        for (u32 i = 0; i < NUM_CHANNELS; i++)
        {
          if (CanTransferChannel(static_cast<Channel>(i), false))
          {
            if (!TransferChannel(static_cast<Channel>(i)))
              break;
          }
        }

        return;
      }

      case 0x74:
      {
        Log_TracePrintf("DCIR <- 0x%08X", value);
        s_DICR.bits = (s_DICR.bits & ~DICR_WRITE_MASK) | (value & DICR_WRITE_MASK);
        s_DICR.bits = s_DICR.bits & ~(value & DICR_RESET_MASK);
        s_DICR.UpdateMasterFlag();
        return;
      }

      default:
        break;
    }
  }

  Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
}

void DMA::SetRequest(Channel channel, bool request)
{
  ChannelState& cs = s_state[static_cast<u32>(channel)];
  if (cs.request == request)
    return;

  cs.request = request;
  if (CanTransferChannel(channel, false))
    TransferChannel(channel);
}

void DMA::SetMaxSliceTicks(TickCount ticks)
{
  s_max_slice_ticks = ticks;
}

void DMA::SetHaltTicks(TickCount ticks)
{
  s_halt_ticks = ticks;
}

bool DMA::CanTransferChannel(Channel channel, bool ignore_halt)
{
  if (!s_DPCR.GetMasterEnable(channel))
    return false;

  const ChannelState& cs = s_state[static_cast<u32>(channel)];
  if (!cs.channel_control.enable_busy)
    return false;

  if (cs.channel_control.sync_mode != SyncMode::Manual && (IsTransferHalted() && !ignore_halt))
    return false;

  return cs.request;
}

bool DMA::IsTransferHalted()
{
  return s_unhalt_event->IsActive();
}

void DMA::UpdateIRQ()
{
  s_DICR.UpdateMasterFlag();
  if (s_DICR.master_flag)
  {
    Log_TracePrintf("Firing DMA master interrupt");
    InterruptController::InterruptRequest(InterruptController::IRQ::DMA);
  }
}

// Plenty of games seem to suffer from this issue where they have a linked list DMA going while polling the
// controller. Using a too-large slice size will result in the serial timing being off, and the game thinking
// the controller is disconnected. So we don't hurt performance too much for the general case, we reduce this
// to equal CPU and DMA time when the controller is transferring, but otherwise leave it at the higher size.
enum : u32
{
  SLICE_SIZE_WHEN_TRANSMITTING_PAD = 100,
  HALT_TICKS_WHEN_TRANSMITTING_PAD = 100
};

TickCount DMA::GetTransferSliceTicks()
{
#ifdef _DEBUG
  if (Pad::IsTransmitting())
  {
    Log_DebugPrintf("DMA transfer while transmitting pad - using lower slice size of %u vs %u",
                    SLICE_SIZE_WHEN_TRANSMITTING_PAD, s_max_slice_ticks);
  }
#endif

  return Pad::IsTransmitting() ? SLICE_SIZE_WHEN_TRANSMITTING_PAD : s_max_slice_ticks;
}

TickCount DMA::GetTransferHaltTicks()
{
  return Pad::IsTransmitting() ? HALT_TICKS_WHEN_TRANSMITTING_PAD : s_halt_ticks;
}

bool DMA::TransferChannel(Channel channel)
{
  ChannelState& cs = s_state[static_cast<u32>(channel)];
  const u32 mask = GetAddressMask();

  const bool copy_to_device = cs.channel_control.copy_to_device;

  // start/trigger bit is cleared on beginning of transfer
  cs.channel_control.start_trigger = false;

  PhysicalMemoryAddress current_address = cs.base_address;
  const PhysicalMemoryAddress increment = cs.channel_control.address_step_reverse ? static_cast<u32>(-4) : UINT32_C(4);
  switch (cs.channel_control.sync_mode)
  {
    case SyncMode::Manual:
    {
      const u32 word_count = cs.block_control.manual.GetWordCount();
      Log_DebugPrintf("DMA%u: Copying %u words %s 0x%08X", static_cast<u32>(channel), word_count,
                      copy_to_device ? "from" : "to", current_address & mask);

      TickCount used_ticks;
      if (copy_to_device)
        used_ticks = TransferMemoryToDevice(channel, current_address & mask, increment, word_count);
      else
        used_ticks = TransferDeviceToMemory(channel, current_address & mask, increment, word_count);

      CPU::AddPendingTicks(used_ticks);
    }
    break;

    case SyncMode::LinkedList:
    {
      if (!copy_to_device)
      {
        Panic("Linked list not implemented for DMA reads");
        return true;
      }

      Log_DebugPrintf("DMA%u: Copying linked list starting at 0x%08X to device", static_cast<u32>(channel),
                      current_address & mask);

      u8* ram_pointer = Bus::g_ram;
      TickCount remaining_ticks = GetTransferSliceTicks();
      while (cs.request && remaining_ticks > 0)
      {
        u32 header;
        std::memcpy(&header, &ram_pointer[current_address & mask], sizeof(header));
        CPU::AddPendingTicks(10);
        remaining_ticks -= 10;

        const u32 word_count = header >> 24;
        const u32 next_address = header & UINT32_C(0x00FFFFFF);
        Log_TracePrintf(" .. linked list entry at 0x%08X size=%u(%u words) next=0x%08X", current_address & mask,
                        word_count * UINT32_C(4), word_count, next_address);
        if (word_count > 0)
        {
          CPU::AddPendingTicks(5);
          remaining_ticks -= 5;

          const TickCount block_ticks =
            TransferMemoryToDevice(channel, (current_address + sizeof(header)) & mask, 4, word_count);
          CPU::AddPendingTicks(block_ticks);
          remaining_ticks -= block_ticks;
        }

        current_address = next_address;
        if (current_address & UINT32_C(0x800000))
          break;
      }

      cs.base_address = current_address;

      if (current_address & UINT32_C(0x800000))
        break;

      if (cs.request)
      {
        // stall the transfer for a bit if we ran for too long
        HaltTransfer(GetTransferHaltTicks());
        return false;
      }
      else
      {
        // linked list not yet complete
        return true;
      }
    }
    break;

    case SyncMode::Request:
    {
      Log_DebugPrintf("DMA%u: Copying %u blocks of size %u (%u total words) %s 0x%08X", static_cast<u32>(channel),
                      cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                      cs.block_control.request.GetBlockCount() * cs.block_control.request.GetBlockSize(),
                      copy_to_device ? "from" : "to", current_address & mask);

      const u32 block_size = cs.block_control.request.GetBlockSize();
      u32 blocks_remaining = cs.block_control.request.GetBlockCount();
      TickCount ticks_remaining = GetTransferSliceTicks();

      if (copy_to_device)
      {
        do
        {
          blocks_remaining--;

          const TickCount ticks = TransferMemoryToDevice(channel, current_address & mask, increment, block_size);
          CPU::AddPendingTicks(ticks);
          ticks_remaining -= ticks;

          current_address = (current_address + (increment * block_size));
        } while (cs.request && blocks_remaining > 0 && ticks_remaining > 0);
      }
      else
      {
        do
        {
          blocks_remaining--;

          const TickCount ticks = TransferDeviceToMemory(channel, current_address & mask, increment, block_size);
          CPU::AddPendingTicks(ticks);
          ticks_remaining -= ticks;

          current_address = (current_address + (increment * block_size));
        } while (cs.request && blocks_remaining > 0 && ticks_remaining > 0);
      }

      cs.base_address = current_address & BASE_ADDRESS_MASK;
      cs.block_control.request.block_count = blocks_remaining;

      // finish transfer later if the request was cleared
      if (blocks_remaining > 0)
      {
        if (cs.request)
        {
          // we got halted
          if (!s_unhalt_event->IsActive())
            HaltTransfer(GetTransferHaltTicks());

          return false;
        }

        return true;
      }
    }
    break;

    default:
      Panic("Unimplemented sync mode");
      break;
  }

  // start/busy bit is cleared on end of transfer
  cs.channel_control.enable_busy = false;
  if (s_DICR.IsIRQEnabled(channel))
  {
    Log_DebugPrintf("Set DMA interrupt for channel %u", static_cast<u32>(channel));
    s_DICR.SetIRQFlag(channel);
    UpdateIRQ();
  }

  return true;
}

void DMA::HaltTransfer(TickCount duration)
{
  s_halt_ticks_remaining += duration;
  Log_DebugPrintf("Halting DMA for %d ticks", s_halt_ticks_remaining);
  if (s_unhalt_event->IsActive())
    return;

  DebugAssert(!s_unhalt_event->IsActive());
  s_unhalt_event->SetIntervalAndSchedule(s_halt_ticks_remaining);
}

void DMA::UnhaltTransfer(void*, TickCount ticks, TickCount ticks_late)
{
  Log_DebugPrintf("Resuming DMA after %d ticks, %d ticks late", ticks, -(s_halt_ticks_remaining - ticks));
  s_halt_ticks_remaining -= ticks;
  s_unhalt_event->Deactivate();

  // TODO: Use channel priority. But doing it in ascending order is probably good enough.
  // Main thing is that OTC happens after GPU, because otherwise it'll wipe out the LL.
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    if (CanTransferChannel(static_cast<Channel>(i), false))
    {
      if (!TransferChannel(static_cast<Channel>(i)))
        return;
    }
  }

  // We didn't run too long, so reset timer.
  s_halt_ticks_remaining = 0;
}

TickCount DMA::TransferMemoryToDevice(Channel channel, u32 address, u32 increment, u32 word_count)
{
  const u32* src_pointer = reinterpret_cast<u32*>(Bus::g_ram + address);
  const u32 mask = GetAddressMask();
  if (channel != Channel::GPU &&
      (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & mask) <= address))
  {
    // Use temp buffer if it's wrapping around
    if (s_transfer_buffer.size() < word_count)
      s_transfer_buffer.resize(word_count);
    src_pointer = s_transfer_buffer.data();

    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&s_transfer_buffer[i], &ram_pointer[address], sizeof(u32));
      address = (address + increment) & mask;
    }
  }

  switch (channel)
  {
    case Channel::GPU:
    {
      if (g_gpu->BeginDMAWrite())
      {
        u8* ram_pointer = Bus::g_ram;
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value;
          std::memcpy(&value, &ram_pointer[address], sizeof(u32));
          g_gpu->DMAWrite(address, value);
          address = (address + increment) & mask;
        }
        g_gpu->EndDMAWrite();
      }
    }
    break;

    case Channel::SPU:
      SPU::DMAWrite(src_pointer, word_count);
      break;

    case Channel::MDECin:
      MDEC::DMAWrite(src_pointer, word_count);
      break;

    case Channel::CDROM:
    case Channel::MDECout:
    case Channel::PIO:
    default:
      Log_ErrorPrintf("Unhandled DMA channel %u for device write", static_cast<u32>(channel));
      break;
  }

  return Bus::GetDMARAMTickCount(word_count);
}

TickCount DMA::TransferDeviceToMemory(Channel channel, u32 address, u32 increment, u32 word_count)
{
  const u32 mask = GetAddressMask();

  if (channel == Channel::OTC)
  {
    // clear ordering table
    u8* ram_pointer = Bus::g_ram;
    const u32 word_count_less_1 = word_count - 1;
    for (u32 i = 0; i < word_count_less_1; i++)
    {
      u32 value = ((address - 4) & mask);
      std::memcpy(&ram_pointer[address], &value, sizeof(value));
      address = (address - 4) & mask;
    }

    const u32 terminator = UINT32_C(0xFFFFFF);
    std::memcpy(&ram_pointer[address], &terminator, sizeof(terminator));
    CPU::CodeCache::InvalidateCodePages(address, word_count);
    return Bus::GetDMARAMTickCount(word_count);
  }

  u32* dest_pointer = reinterpret_cast<u32*>(&Bus::g_ram[address]);
  if (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & mask) <= address)
  {
    // Use temp buffer if it's wrapping around
    if (s_transfer_buffer.size() < word_count)
      s_transfer_buffer.resize(word_count);
    dest_pointer = s_transfer_buffer.data();
  }

  // Read from device.
  switch (channel)
  {
    case Channel::GPU:
      g_gpu->DMARead(dest_pointer, word_count);
      break;

    case Channel::CDROM:
      CDROM::DMARead(dest_pointer, word_count);
      break;

    case Channel::SPU:
      SPU::DMARead(dest_pointer, word_count);
      break;

    case Channel::MDECout:
      MDEC::DMARead(dest_pointer, word_count);
      break;

    default:
      Log_ErrorPrintf("Unhandled DMA channel %u for device read", static_cast<u32>(channel));
      std::fill_n(dest_pointer, word_count, UINT32_C(0xFFFFFFFF));
      break;
  }

  if (dest_pointer == s_transfer_buffer.data())
  {
    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&ram_pointer[address], &s_transfer_buffer[i], sizeof(u32));
      address = (address + increment) & mask;
    }
  }

  CPU::CodeCache::InvalidateCodePages(address, word_count);
  return Bus::GetDMARAMTickCount(word_count);
}

void DMA::DrawDebugStateWindow()
{
  static constexpr u32 NUM_COLUMNS = 10;
  static constexpr std::array<const char*, NUM_COLUMNS> column_names = {
    {"#", "Req", "Direction", "Chopping", "Mode", "Busy", "Enable", "Priority", "IRQ", "Flag"}};
  static constexpr std::array<const char*, NUM_CHANNELS> channel_names = {
    {"MDECin", "MDECout", "GPU", "CDROM", "SPU", "PIO", "OTC"}};
  static constexpr std::array<const char*, 4> sync_mode_names = {{"Manual", "Request", "LinkedList", "Reserved"}};

  const float framebuffer_scale = Host::GetOSDScale();

  ImGui::SetNextWindowSize(ImVec2(850.0f * framebuffer_scale, 250.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("DMA State", nullptr))
  {
    ImGui::End();
    return;
  }

  ImGui::Columns(NUM_COLUMNS);
  ImGui::SetColumnWidth(0, 100.0f * framebuffer_scale);
  ImGui::SetColumnWidth(1, 50.0f * framebuffer_scale);
  ImGui::SetColumnWidth(2, 100.0f * framebuffer_scale);
  ImGui::SetColumnWidth(3, 150.0f * framebuffer_scale);
  ImGui::SetColumnWidth(4, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(5, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(6, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(7, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(8, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(9, 80.0f * framebuffer_scale);

  for (const char* title : column_names)
  {
    ImGui::TextUnformatted(title);
    ImGui::NextColumn();
  }

  const ImVec4 active(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 inactive(0.5f, 0.5f, 0.5f, 1.0f);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    const ChannelState& cs = s_state[i];

    ImGui::TextColored(cs.channel_control.enable_busy ? active : inactive, "%u[%s]", i, channel_names[i]);
    ImGui::NextColumn();
    ImGui::TextColored(cs.request ? active : inactive, cs.request ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::Text("%s%s", cs.channel_control.copy_to_device ? "FromRAM" : "ToRAM",
                cs.channel_control.address_step_reverse ? " Addr+" : " Addr-");
    ImGui::NextColumn();
    ImGui::TextColored(cs.channel_control.chopping_enable ? active : inactive, "%s/%u/%u",
                       cs.channel_control.chopping_enable ? "Yes" : "No",
                       cs.channel_control.chopping_cpu_window_size.GetValue(),
                       cs.channel_control.chopping_dma_window_size.GetValue());
    ImGui::NextColumn();
    ImGui::Text("%s", sync_mode_names[static_cast<u8>(cs.channel_control.sync_mode.GetValue())]);
    ImGui::NextColumn();
    ImGui::TextColored(cs.channel_control.enable_busy ? active : inactive, "%s%s",
                       cs.channel_control.enable_busy ? "Busy" : "Idle",
                       cs.channel_control.start_trigger ? " (Trigger)" : "");
    ImGui::NextColumn();
    ImGui::TextColored(s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive,
                       s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive, "%u",
                       s_DPCR.GetPriority(static_cast<Channel>(i)));
    ImGui::NextColumn();
    ImGui::TextColored(s_DICR.IsIRQEnabled(static_cast<Channel>(i)) ? active : inactive,
                       s_DICR.IsIRQEnabled(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_DICR.GetIRQFlag(static_cast<Channel>(i)) ? active : inactive,
                       s_DICR.GetIRQFlag(static_cast<Channel>(i)) ? "IRQ" : "");
    ImGui::NextColumn();
  }

  ImGui::Columns(1);
  ImGui::End();
}
