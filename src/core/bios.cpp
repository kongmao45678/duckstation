// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "bios.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "cpu_disasm.h"
#include "host.h"
#include "host_settings.h"
#include "settings.h"
#include <cerrno>
Log_SetChannel(BIOS);

static constexpr BIOS::Hash MakeHashFromString(const char str[])
{
  BIOS::Hash h{};
  for (int i = 0; str[i] != '\0'; i++)
  {
    u8 nibble = 0;
    char ch = str[i];
    if (ch >= '0' && ch <= '9')
      nibble = str[i] - '0';
    else if (ch >= 'a' && ch <= 'z')
      nibble = 0xA + (str[i] - 'a');
    else if (ch >= 'A' && ch <= 'Z')
      nibble = 0xA + (str[i] - 'A');

    h.bytes[i / 2] |= nibble << (((i & 1) ^ 1) * 4);
  }
  return h;
}

std::string BIOS::Hash::ToString() const
{
  char str[33];
  std::snprintf(str, sizeof(str), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", bytes[0],
                bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return str;
}

static constexpr const BIOS::ImageInfo s_image_info_by_hash[] = {
  {"SCPH-1000, DTL-H1000 (v1.0)", ConsoleRegion::NTSC_J, MakeHashFromString("239665b1a3dade1b5a52c06338011044"), true},
  {"SCPH-1001, 5003, DTL-H1201, H3001 (v2.2 12-04-95 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("924e392ed05558ffdb115408c263dccf"), true},
  {"SCPH-1002, DTL-H1002 (v2.0 05-10-95 E)", ConsoleRegion::PAL, MakeHashFromString("54847e693405ffeb0359c6287434cbef"),
   true},
  {"SCPH-1002, DTL-H1102 (v2.1 07-17-95 E)", ConsoleRegion::PAL, MakeHashFromString("417b34706319da7cf001e76e40136c23"),
   true},
  {"SCPH-1002, DTL-H1202, H3002 (v2.2 12-04-95 E)", ConsoleRegion::PAL,
   MakeHashFromString("e2110b8a2b97a8e0b857a45d32f7e187"), true},
  {"DTL-H1100 (v2.2 03-06-96 D)", ConsoleRegion::NTSC_J, MakeHashFromString("ca5cfc321f916756e3f0effbfaeba13b"), true},
  {"SCPH-3000, DTL-H1000H (v1.1 01-22-95)", ConsoleRegion::NTSC_J,
   MakeHashFromString("849515939161e62f6b866f6853006780"), true},
  {"SCPH-1001, DTL-H1001 (v2.0 05-07-95 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("dc2b9bf8da62ec93e868cfd29f0d067d"), true},
  {"SCPH-3500 (v2.1 07-17-95 J)", ConsoleRegion::NTSC_J, MakeHashFromString("cba733ceeff5aef5c32254f1d617fa62"), true},
  {"SCPH-1001, DTL-H1101 (v2.1 07-17-95 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("da27e8b6dab242d8f91a9b25d80c63b8"), true},
  {"SCPH-5000, DTL-H1200, H3000 (v2.2 12-04-95 J)", ConsoleRegion::NTSC_J,
   MakeHashFromString("57a06303dfa9cf9351222dfcbb4a29d9"), true},
  {"SCPH-5500 (v3.0 09-09-96 J)", ConsoleRegion::NTSC_J, MakeHashFromString("8dd7d5296a650fac7319bce665a6a53c"), true},
  {"SCPH-5501, 5503, 7003 (v3.0 11-18-96 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("490f666e1afb15b7362b406ed1cea246"), true},
  {"SCPH-5502, 5552 (v3.0 01-06-97 E)", ConsoleRegion::PAL, MakeHashFromString("32736f17079d0b2b7024407c39bd3050"),
   true},
  {"SCPH-7000, 7500, 9000 (v4.0 08-18-97 J)", ConsoleRegion::NTSC_J,
   MakeHashFromString("8e4c14f567745eff2f0408c8129f72a6"), true},
  {"SCPH-7000W (v4.1 11-14-97 A)", ConsoleRegion::NTSC_J, MakeHashFromString("b84be139db3ee6cbd075630aa20a6553"), true},
  {"SCPH-7001, 7501, 7503, 9001, 9003, 9903 (v4.1 12-16-97 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("1e68c231d0896b7eadcad1d7d8e76129"), true},
  {"SCPH-7002, 7502, 9002 (v4.1 12-16-97 E)", ConsoleRegion::PAL,
   MakeHashFromString("b9d9a0286c33dc6b7237bb13cd46fdee"), true},
  {"SCPH-100 (v4.3 03-11-00 J)", ConsoleRegion::NTSC_J, MakeHashFromString("8abc1b549a4a80954addc48ef02c4521"), true},
  {"SCPH-101 (v4.4 03-24-00 A)", ConsoleRegion::NTSC_U, MakeHashFromString("9a09ab7e49b422c007e6d54d7c49b965"), true},
  {"SCPH-101 (v4.5 05-25-00 A)", ConsoleRegion::NTSC_U, MakeHashFromString("6e3735ff4c7dc899ee98981385f6f3d0"), true},
  {"SCPH-102 (v4.4 03-24-00 E)", ConsoleRegion::PAL, MakeHashFromString("b10f5e0e3d9eb60e5159690680b1e774"), true},
  {"SCPH-102 (v4.5 05-25-00 E)", ConsoleRegion::PAL, MakeHashFromString("de93caec13d1a141a40a79f5c86168d6"), true},
  {"PS2, SCPH-18000 (v5.0 10-27-00 J)", ConsoleRegion::NTSC_J, MakeHashFromString("d8f485717a5237285e4d7c5f881b7f32"),
   true},
  {"PS2, SCPH-30003 (v5.0 09-02-00 E)", ConsoleRegion::PAL, MakeHashFromString("71f50ef4f4e17c163c78908e16244f7d"),
   true},
  {"PSP, SCPH-1000R (v4.5 05-25-00 J)", ConsoleRegion::Auto, MakeHashFromString("c53ca5908936d412331790f4426c6c33"),
   true},
  {"SCPH-1000R (v4.5 05-25-00 J)", ConsoleRegion::NTSC_J, MakeHashFromString("476d68a94ccec3b9c8303bbd1daf2810"), true},
  {"PS3 (v5.0 06-23-03 A)", ConsoleRegion::Auto, MakeHashFromString("c02a6fbb1b27359f84e92fae8bc21316"), false},
  {"PS3 (v5.0 06-23-03 A)", ConsoleRegion::Auto, MakeHashFromString("81bbe60ba7a3d1cea1d48c14cbcc647b"), false},
};

// OpenBIOS is separate, because there's no fixed hash for it. So just in case something collides with a hash of zero...
// which would be unlikely.
static constexpr const BIOS::ImageInfo s_openbios_info = {"OpenBIOS", ConsoleRegion::Auto, {}, false};
static constexpr const char s_openbios_signature[] = {'O', 'p', 'e', 'n', 'B', 'I', 'O', 'S'};
static constexpr u32 s_openbios_signature_offset = 0x78;

static BIOS::Hash GetHash(const BIOS::Image& image)
{
  BIOS::Hash hash;
  MD5Digest digest;
  digest.Update(image.data(), static_cast<u32>(image.size()));
  digest.Final(hash.bytes);
  return hash;
}

std::optional<BIOS::Image> BIOS::LoadImageFromFile(const char* filename)
{
  Image ret(BIOS_SIZE);
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open BIOS image '%s', errno=%d", filename, errno);
    return std::nullopt;
  }

  std::fseek(fp.get(), 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp.get()));
  std::fseek(fp.get(), 0, SEEK_SET);

  if (size != BIOS_SIZE && size != BIOS_SIZE_PS2 && size != BIOS_SIZE_PS3)
  {
    Log_ErrorPrintf("BIOS image '%s' size mismatch, expecting either %u or %u or %u bytes but got %u bytes", filename,
                    BIOS_SIZE, BIOS_SIZE_PS2, BIOS_SIZE_PS3, size);
    return std::nullopt;
  }

  if (std::fread(ret.data(), 1, ret.size(), fp.get()) != ret.size())
  {
    Log_ErrorPrintf("Failed to read BIOS image '%s'", filename);
    return std::nullopt;
  }

  Log_DevPrint(fmt::format("Hash for BIOS '{}': {}", FileSystem::GetDisplayNameFromPath(filename), GetHash(ret).ToString()).c_str());
  return ret;
}

const BIOS::ImageInfo* BIOS::GetInfoForImage(const Image& image)
{
  const Hash hash(GetHash(image));

  // check for openbios
  if (image.size() >= (s_openbios_signature_offset + std::size(s_openbios_signature)) &&
    std::memcmp(&image[s_openbios_signature_offset], s_openbios_signature, std::size(s_openbios_signature)) == 0)
  {
    return &s_openbios_info;
  }

  for (const ImageInfo& ii : s_image_info_by_hash)
  {
    if (ii.hash == hash)
      return &ii;
  }

  Log_WarningPrintf("Unknown BIOS hash: %s", hash.ToString().c_str());
  return nullptr;
}

bool BIOS::IsValidBIOSForRegion(ConsoleRegion console_region, ConsoleRegion bios_region)
{
  return (bios_region == ConsoleRegion::Auto || bios_region == console_region);
}

void BIOS::PatchBIOS(u8* image, u32 image_size, u32 address, u32 value, u32 mask /*= UINT32_C(0xFFFFFFFF)*/)
{
  const u32 phys_address = address & UINT32_C(0x1FFFFFFF);
  const u32 offset = phys_address - BIOS_BASE;
  Assert(phys_address >= BIOS_BASE && (offset + sizeof(u32)) <= image_size);

  u32 existing_value;
  std::memcpy(&existing_value, &image[offset], sizeof(existing_value));
  u32 new_value = (existing_value & ~mask) | value;
  std::memcpy(&image[offset], &new_value, sizeof(new_value));

  SmallString old_disasm, new_disasm;
  CPU::DisassembleInstruction(&old_disasm, address, existing_value);
  CPU::DisassembleInstruction(&new_disasm, address, new_value);
  Log_DevPrintf("BIOS-Patch 0x%08X (+0x%X): 0x%08X %s -> %08X %s", address, offset, existing_value,
                old_disasm.GetCharArray(), new_value, new_disasm.GetCharArray());
}

bool BIOS::PatchBIOSEnableTTY(u8* image, u32 image_size)
{
  Log_InfoPrintf("Patching BIOS to enable TTY/printf");
  PatchBIOS(image, image_size, 0x1FC06F0C, 0x24010001);
  PatchBIOS(image, image_size, 0x1FC06F14, 0xAF81A9C0);
  return true;
}

bool BIOS::PatchBIOSFastBoot(u8* image, u32 image_size)
{
  // Replace the shell entry point with a return back to the bootstrap.
  Log_InfoPrintf("Patching BIOS to skip intro");
  PatchBIOS(image, image_size, 0x1FC18000, 0x3C011F80); // lui at, 1f80
  PatchBIOS(image, image_size, 0x1FC18004, 0x3C0A0300); // lui t2, 0300h
  PatchBIOS(image, image_size, 0x1FC18008, 0xAC2A1814); // sw zero, 1814h(at)        ; turn the display on
  PatchBIOS(image, image_size, 0x1FC1800C, 0x03E00008); // jr ra
  PatchBIOS(image, image_size, 0x1FC18010, 0x00000000); // nop
  return true;
}

bool BIOS::PatchBIOSForEXE(u8* image, u32 image_size, u32 r_pc, u32 r_gp, u32 r_sp, u32 r_fp)
{
#define PATCH(offset, value) PatchBIOS(image, image_size, (offset), (value))

  // pc has to be done first because we can't load it in the delay slot
  PATCH(0xBFC06FF0, UINT32_C(0x3C080000) | r_pc >> 16);                // lui $t0, (r_pc >> 16)
  PATCH(0xBFC06FF4, UINT32_C(0x35080000) | (r_pc & UINT32_C(0xFFFF))); // ori $t0, $t0, (r_pc & 0xFFFF)
  PATCH(0xBFC06FF8, UINT32_C(0x3C1C0000) | r_gp >> 16);                // lui $gp, (r_gp >> 16)
  PATCH(0xBFC06FFC, UINT32_C(0x379C0000) | (r_gp & UINT32_C(0xFFFF))); // ori $gp, $gp, (r_gp & 0xFFFF)

  if (r_sp != 0)
  {
    PATCH(0xBFC07000, UINT32_C(0x3C1D0000) | r_sp >> 16);                // lui $sp, (r_sp >> 16)
    PATCH(0xBFC07004, UINT32_C(0x37BD0000) | (r_sp & UINT32_C(0xFFFF))); // ori $sp, $sp, (r_sp & 0xFFFF)
  }
  else
  {
    PATCH(0xBFC07000, UINT32_C(0x00000000)); // nop
    PATCH(0xBFC07004, UINT32_C(0x00000000)); // nop
  }
  if (r_fp != 0)
  {
    PATCH(0xBFC07008, UINT32_C(0x3C1E0000) | r_fp >> 16);                // lui $fp, (r_fp >> 16)
    PATCH(0xBFC0700C, UINT32_C(0x01000008));                             // jr $t0
    PATCH(0xBFC07010, UINT32_C(0x37DE0000) | (r_fp & UINT32_C(0xFFFF))); // ori $fp, $fp, (r_fp & 0xFFFF)
  }
  else
  {
    PATCH(0xBFC07008, UINT32_C(0x00000000)); // nop
    PATCH(0xBFC0700C, UINT32_C(0x01000008)); // jr $t0
    PATCH(0xBFC07010, UINT32_C(0x00000000)); // nop
  }

#undef PATCH

  return true;
}

bool BIOS::IsValidPSExeHeader(const PSEXEHeader& header, u32 file_size)
{
  static constexpr char expected_id[] = {'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  if (std::memcmp(header.id, expected_id, sizeof(expected_id)) != 0)
    return false;

  if ((header.file_size + sizeof(PSEXEHeader)) > file_size)
  {
    Log_WarningPrintf("Incorrect file size in PS-EXE header: %u bytes should not be greater than %u bytes",
                      header.file_size, static_cast<unsigned>(file_size - sizeof(PSEXEHeader)));
  }

  return true;
}

DiscRegion BIOS::GetPSExeDiscRegion(const PSEXEHeader& header)
{
  static constexpr char ntsc_u_id[] = "Sony Computer Entertainment Inc. for North America area";
  static constexpr char ntsc_j_id[] = "Sony Computer Entertainment Inc. for Japan area";
  static constexpr char pal_id[] = "Sony Computer Entertainment Inc. for Europe area";

  if (std::memcmp(header.marker, ntsc_u_id, sizeof(ntsc_u_id) - 1) == 0)
    return DiscRegion::NTSC_U;
  else if (std::memcmp(header.marker, ntsc_j_id, sizeof(ntsc_j_id) - 1) == 0)
    return DiscRegion::NTSC_J;
  else if (std::memcmp(header.marker, pal_id, sizeof(pal_id) - 1) == 0)
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

std::optional<std::vector<u8>> BIOS::GetBIOSImage(ConsoleRegion region)
{
  std::string bios_name;
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      bios_name = Host::GetStringSettingValue("BIOS", "PathNTSCJ", "");
      break;

    case ConsoleRegion::PAL:
      bios_name = Host::GetStringSettingValue("BIOS", "PathPAL", "");
      break;

    case ConsoleRegion::NTSC_U:
    default:
      bios_name = Host::GetStringSettingValue("BIOS", "PathNTSCU", "");
      break;
  }

  if (bios_name.empty())
  {
    // auto-detect
    return FindBIOSImageInDirectory(region, EmuFolders::Bios.c_str());
  }

  // try the configured path
  std::optional<Image> image = LoadImageFromFile(Path::Combine(EmuFolders::Bios, bios_name).c_str());
  if (!image.has_value())
  {
    Host::ReportFormattedErrorAsync(
      "Error", Host::TranslateString("HostInterface", "Failed to load configured BIOS file '%s'"), bios_name.c_str());
    return std::nullopt;
  }

  const ImageInfo* ii = GetInfoForImage(image.value());
  if (!ii || !IsValidBIOSForRegion(region, ii->region))
    Log_WarningPrintf("BIOS '%s' does not match region. This may cause issues.", bios_name.c_str());

  return image;
}

std::optional<std::vector<u8>> BIOS::FindBIOSImageInDirectory(ConsoleRegion region, const char* directory)
{
  Log_InfoPrintf("Searching for a %s BIOS in '%s'...", Settings::GetConsoleRegionDisplayName(region), directory);

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(
    directory, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  std::string fallback_path;
  std::optional<Image> fallback_image;
  const ImageInfo* fallback_info = nullptr;

  for (const FILESYSTEM_FIND_DATA& fd : results)
  {
    if (fd.Size != BIOS_SIZE && fd.Size != BIOS_SIZE_PS2 && fd.Size != BIOS_SIZE_PS3)
    {
      Log_WarningPrintf("Skipping '%s': incorrect size", fd.FileName.c_str());
      continue;
    }

    std::string full_path(Path::Combine(directory, fd.FileName));
    std::optional<Image> found_image = LoadImageFromFile(full_path.c_str());
    if (!found_image)
      continue;

    const ImageInfo* ii = GetInfoForImage(found_image.value());
    if (ii && IsValidBIOSForRegion(region, ii->region))
    {
      Log_InfoPrintf("Using BIOS '%s': %s", fd.FileName.c_str(), ii->description);
      return found_image;
    }

    // don't let an unknown bios take precedence over a known one
    if (!fallback_path.empty() && (fallback_info || !ii))
      continue;

    fallback_path = std::move(full_path);
    fallback_image = std::move(found_image);
    fallback_info = ii;
  }

  if (!fallback_image.has_value())
  {
    Host::ReportFormattedErrorAsync("Error",
                                    Host::TranslateString("HostInterface", "No BIOS image found for %s region"),
                                    Settings::GetConsoleRegionDisplayName(region));
    return std::nullopt;
  }

  if (!fallback_info)
  {
    Log_WarningPrintf("Using unknown BIOS '%s'. This may crash.", fallback_path.c_str());
  }
  else
  {
    Log_WarningPrintf("Falling back to possibly-incompatible image '%s': %s", fallback_path.c_str(),
                      fallback_info->description);
  }

  return fallback_image;
}

std::vector<std::pair<std::string, const BIOS::ImageInfo*>> BIOS::FindBIOSImagesInDirectory(const char* directory)
{
  std::vector<std::pair<std::string, const ImageInfo*>> results;

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(directory, "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if (fd.Size != BIOS_SIZE && fd.Size != BIOS_SIZE_PS2 && fd.Size != BIOS_SIZE_PS3)
      continue;

    std::string full_path(Path::Combine(directory, fd.FileName));
    std::optional<Image> found_image = LoadImageFromFile(full_path.c_str());
    if (!found_image)
      continue;

    const ImageInfo* ii = GetInfoForImage(found_image.value());
    results.emplace_back(std::move(fd.FileName), ii);
  }

  return results;
}

bool BIOS::HasAnyBIOSImages()
{
  return FindBIOSImageInDirectory(ConsoleRegion::Auto, EmuFolders::Bios.c_str()).has_value();
}
