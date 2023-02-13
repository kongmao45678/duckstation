// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "enhancementsettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

EnhancementSettingsWidget::EnhancementSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuDownsampleMode, "GPU", "DownsampleMode",
                                               &Settings::ParseDownsampleModeName, &Settings::GetDownsampleModeName,
                                               Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.trueColor, "GPU", "TrueColor", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.scaledDithering, "GPU", "ScaledDithering", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableInterlacing, "GPU", "DisableInterlacing", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.forceNTSCTimings, "GPU", "ForceNTSCTimings", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.force43For24Bit, "Display", "Force4_3For24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.chromaSmoothingFor24Bit, "GPU", "ChromaSmoothing24Bit", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.textureFiltering, "GPU", "TextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.widescreenHack, "GPU", "WidescreenHack", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useSoftwareRendererForReadbacks, "GPU",
                                               "UseSoftwareRendererForReadbacks", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCulling, "GPU", "PGXPCulling", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpTextureCorrection, "GPU", "PGXPTextureCorrection", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpColorCorrection, "GPU", "PGXPColorCorrection", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDepthBuffer, "GPU", "PGXPDepthBuffer", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpPreserveProjPrecision, "GPU", "PGXPPreserveProjFP", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCPU, "GPU", "PGXPCPU", false);

  connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EnhancementSettingsWidget::updateScaledDitheringEnabled);
  connect(m_ui.trueColor, &QCheckBox::stateChanged, this, &EnhancementSettingsWidget::updateScaledDitheringEnabled);
  updateScaledDitheringEnabled();

  connect(m_ui.pgxpEnable, &QCheckBox::stateChanged, this, &EnhancementSettingsWidget::updatePGXPSettingsEnabled);
  connect(m_ui.pgxpTextureCorrection, &QCheckBox::stateChanged, this,
          &EnhancementSettingsWidget::updatePGXPSettingsEnabled);
  updatePGXPSettingsEnabled();

  dialog->registerWidgetHelp(
    m_ui.gpuDownsampleMode, tr("Downsampling"), tr("Disabled"),
    tr("Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games, "
       "but should be disabled for pure 3D games. Only applies to the hardware renderers."));
  dialog->registerWidgetHelp(
    m_ui.disableInterlacing, tr("Disable Interlacing (force progressive render/scan)"), tr("Unchecked"),
    tr(
      "Forces the rendering and display of frames to progressive mode. <br>This removes the \"combing\" effect seen in "
      "480i games by rendering them in 480p. Usually safe to enable.<br> "
      "<b><u>May not be compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(
    m_ui.resolutionScale, tr("Resolution Scale"), "1x",
    tr("Setting this beyond 1x will enhance the resolution of rendered 3D polygons and lines. Only applies "
       "to the hardware backends. <br>This option is usually safe, with most games looking fine at "
       "higher resolutions. Higher resolutions require a more powerful GPU."));
  dialog->registerWidgetHelp(
    m_ui.trueColor, tr("True Color Rendering (24-bit, disables dithering)"), tr("Unchecked"),
    tr("Forces the precision of colours output to the console's framebuffer to use the full 8 bits of precision per "
       "channel. This produces nicer looking gradients at the cost of making some colours look slightly different. "
       "Disabling the option also enables dithering, which makes the transition between colours less sharp by applying "
       "a pattern around those pixels. Most games are compatible with this option, but there is a number which aren't "
       "and will have broken effects with it enabled. Only applies to the hardware renderers."));
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, tr("Scaled Dithering (scale dither pattern to resolution)"), tr("Checked"),
    tr("Scales the dither pattern to the resolution scale of the emulated GPU. This makes the dither pattern much less "
       "obvious at higher resolutions. <br>Usually safe to enable, and only supported by the hardware renderers."));
  dialog->registerWidgetHelp(m_ui.forceNTSCTimings, tr("Force NTSC Timings (60hz-on-PAL)"), tr("Unchecked"),
                             tr("Uses NTSC frame timings when the console is in PAL mode, forcing PAL games to run at "
                                "60hz. <br>For most games which "
                                "have a speed tied to the framerate, this will result in the game running "
                                "approximately 17% faster. <br>For variable "
                                "frame rate games, it may not affect the speed."));
  dialog->registerWidgetHelp(
    m_ui.force43For24Bit, tr("Force 4:3 For 24-bit Display"), tr("Unchecked"),
    tr("Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs."));
  dialog->registerWidgetHelp(m_ui.chromaSmoothingFor24Bit, tr("Chroma Smoothing For 24-Bit Display"), tr("Unchecked"),
                             tr("Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. "
                                "Only applies to the hardware renderers."));
  dialog->registerWidgetHelp(
    m_ui.textureFiltering, tr("Texture Filtering"),
    qApp->translate("GPUTextureFilter", Settings::GetTextureFilterDisplayName(GPUTextureFilter::Nearest)),
    tr("Smooths out the blockiness of magnified textures on 3D object by using filtering. <br>Will have a "
       "greater effect on higher resolution scales. Only applies to the hardware renderers. <br>The JINC2 and "
       "especially xBR filtering modes are very demanding, and may not be worth the speed penalty."));
  dialog->registerWidgetHelp(
    m_ui.widescreenHack, tr("Widescreen Hack"), tr("Unchecked"),
    tr("Scales vertex positions in screen-space to a widescreen aspect ratio, essentially "
       "increasing the field of view from 4:3 to the chosen display aspect ratio in 3D games. <br>For 2D games, or "
       "games which use pre-rendered backgrounds, this enhancement will not work as expected. <br><b><u>May not be "
       "compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(
    m_ui.useSoftwareRendererForReadbacks, tr("Use Software Renderer For Readbacks"), tr("Unchecked"),
    tr("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater "
       "performance when using graphical enhancements with the hardware renderer."));
  dialog->registerWidgetHelp(
    m_ui.pgxpEnable, tr("Geometry Correction"), tr("Unchecked"),
    tr("Reduces \"wobbly\" polygons and \"warping\" textures that are common in PS1 games. <br>Only "
       "works with the hardware renderers. <b><u>May not be compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(m_ui.pgxpCulling, tr("Culling Correction"), tr("Checked"),
                             tr("Increases the precision of polygon culling, reducing the number of holes in geometry. "
                                "Requires geometry correction enabled."));
  dialog->registerWidgetHelp(m_ui.pgxpTextureCorrection, tr("Perspective Correct Textures"), tr("Checked"),
                             tr("Uses perspective-correct interpolation for texture coordinates, straightening out "
                                "warped textures. Requires geometry correction enabled."));
  dialog->registerWidgetHelp(
    m_ui.pgxpColorCorrection, tr("Perspective Correct Colors"), tr("Unchecked"),
    tr("Uses perspective-correct interpolation for vertex colors, which can improve visuals in some games, but cause "
       "rendering errors in others. Requires geometry correction enabled."));
  dialog->registerWidgetHelp(
    m_ui.pgxpDepthBuffer, tr("Depth Buffer (Low Compatibility)"), tr("Unchecked"),
    tr("Attempts to reduce polygon Z-fighting by testing pixels against the depth values from PGXP. Low compatibility, "
       "but can work well in some games. Other games may need a threshold adjustment."));
  dialog->registerWidgetHelp(
    m_ui.pgxpPreserveProjPrecision, tr("Preserve Projection Precision"), tr("Unchecked"),
    tr("Adds additional precision to PGXP data post-projection. May improve visuals in some games."));
  dialog->registerWidgetHelp(m_ui.pgxpCPU, tr("CPU Mode (Very Slow)"), tr("Unchecked"),
                             tr("Uses PGXP for all instructions, not just memory operations. Required for PGXP to "
                                "correct wobble in some games, but has a very high performance cost."));
}

EnhancementSettingsWidget::~EnhancementSettingsWidget() = default;

void EnhancementSettingsWidget::updateScaledDitheringEnabled()
{
  const int resolution_scale = m_ui.resolutionScale->currentIndex();
  const bool true_color = m_ui.trueColor->isChecked();
  const bool allow_scaled_dithering = (resolution_scale != 1 && !true_color);
  m_ui.scaledDithering->setEnabled(allow_scaled_dithering);
}

void EnhancementSettingsWidget::setupAdditionalUi()
{
  QtUtils::FillComboBoxWithResolutionScales(m_ui.resolutionScale);

  for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
  {
    m_ui.textureFiltering->addItem(
      qApp->translate("GPUTextureFilter", Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
  {
    m_ui.gpuDownsampleMode->addItem(
      qApp->translate("GPUDownsampleMode", Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i))));
  }
}

void EnhancementSettingsWidget::updatePGXPSettingsEnabled()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("GPU", "PGXPEnable", false);
  const bool tc_enabled = enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPTextureCorrection", true);
  m_ui.pgxpCulling->setEnabled(enabled);
  m_ui.pgxpTextureCorrection->setEnabled(enabled);
  m_ui.pgxpColorCorrection->setEnabled(tc_enabled);
  m_ui.pgxpDepthBuffer->setEnabled(enabled);
  m_ui.pgxpPreserveProjPrecision->setEnabled(enabled);
  m_ui.pgxpCPU->setEnabled(enabled);
}
