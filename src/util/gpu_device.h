// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include "window_info.h"

#include "common/bitfield.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/small_string.h"
#include "common/types.h"

#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

class Error;

enum class RenderAPI : u8
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal
};

enum class GPUVSyncMode : u8
{
  Disabled,
  FIFO,
  Mailbox,
  Count
};

class GPUSampler
{
public:
  enum class Filter
  {
    Nearest,
    Linear,

    MaxCount
  };

  enum class AddressMode
  {
    Repeat,
    ClampToEdge,
    ClampToBorder,
    MirrorRepeat,

    MaxCount
  };

  union Config
  {
    static constexpr u8 LOD_MAX = 15;

    BitField<u64, Filter, 0, 1> min_filter;
    BitField<u64, Filter, 1, 1> mag_filter;
    BitField<u64, Filter, 2, 1> mip_filter;
    BitField<u64, AddressMode, 3, 2> address_u;
    BitField<u64, AddressMode, 5, 2> address_v;
    BitField<u64, AddressMode, 7, 2> address_w;
    BitField<u64, u8, 9, 5> anisotropy;
    BitField<u64, u8, 14, 4> min_lod;
    BitField<u64, u8, 18, 4> max_lod;
    BitField<u64, u32, 32, 32> border_color;
    u64 key;

    // clang-format off
    ALWAYS_INLINE float GetBorderRed() const { return static_cast<float>(border_color.GetValue() & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderGreen() const { return static_cast<float>((border_color.GetValue() >> 8) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderBlue() const { return static_cast<float>((border_color.GetValue() >> 16) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderAlpha() const { return static_cast<float>((border_color.GetValue() >> 24) & 0xFF) / 255.0f; }
    // clang-format on
    ALWAYS_INLINE std::array<float, 4> GetBorderFloatColor() const
    {
      return std::array<float, 4>{GetBorderRed(), GetBorderGreen(), GetBorderBlue(), GetBorderAlpha()};
    }
  };

  GPUSampler();
  virtual ~GPUSampler();

  virtual void SetDebugName(std::string_view name) = 0;

  static Config GetNearestConfig();
  static Config GetLinearConfig();
};

enum class GPUShaderStage : u8
{
  Vertex,
  Fragment,
  Geometry,
  Compute,

  MaxCount
};

enum class GPUShaderLanguage : u8
{
  None,
  HLSL,
  GLSL,
  GLSLES,
  GLSLVK,
  MSL,
  SPV,
  Count
};

class GPUShader
{
public:
  GPUShader(GPUShaderStage stage);
  virtual ~GPUShader();

  static const char* GetStageName(GPUShaderStage stage);

  ALWAYS_INLINE GPUShaderStage GetStage() const { return m_stage; }

  virtual void SetDebugName(std::string_view name) = 0;

protected:
  GPUShaderStage m_stage;
};

class GPUPipeline
{
public:
  enum class Layout : u8
  {
    // 1 streamed UBO, 1 texture in PS.
    SingleTextureAndUBO,

    // 128 byte UBO via push constants, 1 texture.
    SingleTextureAndPushConstants,

    // 128 byte UBO via push constants, 1 texture buffer/SSBO.
    SingleTextureBufferAndPushConstants,

    // Multiple textures, 1 streamed UBO.
    MultiTextureAndUBO,

    // Multiple textures, 128 byte UBO via push constants.
    MultiTextureAndPushConstants,

    MaxCount
  };

  enum RenderPassFlag : u8
  {
    NoRenderPassFlags = 0,
    ColorFeedbackLoop = (1 << 0),
    SampleDepthBuffer = (1 << 1),
    BindRenderTargetsAsImages = (1 << 2),
  };

  enum class Primitive : u8
  {
    Points,
    Lines,
    Triangles,
    TriangleStrips,

    MaxCount
  };

  union VertexAttribute
  {
    static constexpr u32 MaxAttributes = 16;

    enum class Semantic : u8
    {
      Position,
      TexCoord,
      Color,

      MaxCount
    };

    enum class Type : u8
    {
      Float,
      UInt8,
      SInt8,
      UNorm8,
      UInt16,
      SInt16,
      UNorm16,
      UInt32,
      SInt32,

      MaxCount
    };

    BitField<u32, u8, 0, 4> index;
    BitField<u32, Semantic, 4, 2> semantic;
    BitField<u32, u8, 6, 2> semantic_index;
    BitField<u32, Type, 8, 4> type;
    BitField<u32, u8, 12, 3> components;
    BitField<u32, u16, 16, 16> offset;

    u32 key;

    // clang-format off
    ALWAYS_INLINE VertexAttribute() = default;
    ALWAYS_INLINE constexpr VertexAttribute(const VertexAttribute& rhs) : key(rhs.key) {}
    ALWAYS_INLINE VertexAttribute& operator=(const VertexAttribute& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const VertexAttribute& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const VertexAttribute& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const VertexAttribute& rhs) const { return key < rhs.key; }
    // clang-format on

    static constexpr VertexAttribute Make(u8 index, Semantic semantic, u8 semantic_index, Type type, u8 components,
                                          u16 offset)
    {
      // Nasty :/ can't access an inactive element of a union here..
      return VertexAttribute((static_cast<u32>(index) & 0xf) | ((static_cast<u32>(semantic) & 0x3) << 4) |
                             ((static_cast<u32>(semantic_index) & 0x3) << 6) | ((static_cast<u32>(type) & 0xf) << 8) |
                             ((static_cast<u32>(components) & 0x7) << 12) |
                             ((static_cast<u32>(offset) & 0xffff) << 16));
    }

  private:
    ALWAYS_INLINE constexpr VertexAttribute(u32 key_) : key(key_) {}
  };

  struct InputLayout
  {
    std::span<const VertexAttribute> vertex_attributes;
    u32 vertex_stride;

    bool operator==(const InputLayout& rhs) const;
    bool operator!=(const InputLayout& rhs) const;
  };

  struct InputLayoutHash
  {
    size_t operator()(const InputLayout& il) const;
  };

  enum class CullMode : u8
  {
    None,
    Front,
    Back,

    MaxCount
  };

  enum class DepthFunc : u8
  {
    Never,
    Always,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,

    MaxCount
  };

  enum class BlendFunc : u8
  {
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    DstColor,
    InvDstColor,
    SrcAlpha,
    InvSrcAlpha,
    SrcAlpha1,
    InvSrcAlpha1,
    DstAlpha,
    InvDstAlpha,
    ConstantColor,
    InvConstantColor,

    MaxCount
  };

  enum class BlendOp : u8
  {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,

    MaxCount
  };

  // TODO: purge this?
  union RasterizationState
  {
    BitField<u8, CullMode, 0, 2> cull_mode;
    u8 key;

    // clang-format off
    ALWAYS_INLINE RasterizationState() = default;
    ALWAYS_INLINE RasterizationState(const RasterizationState& rhs) : key(rhs.key) {}
    ALWAYS_INLINE RasterizationState& operator=(const RasterizationState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const RasterizationState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const RasterizationState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const RasterizationState& rhs) const { return key < rhs.key; }
    // clang-format on

    static RasterizationState GetNoCullState();
  };

  union DepthState
  {
    BitField<u8, DepthFunc, 0, 3> depth_test;
    BitField<u8, bool, 4, 1> depth_write;
    u8 key;

    // clang-format off
    ALWAYS_INLINE DepthState() = default;
    ALWAYS_INLINE DepthState(const DepthState& rhs) : key(rhs.key) {}
    ALWAYS_INLINE DepthState& operator=(const DepthState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const DepthState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const DepthState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const DepthState& rhs) const { return key < rhs.key; }
    // clang-format on

    static DepthState GetNoTestsState();
    static DepthState GetAlwaysWriteState();
  };

  union BlendState
  {
    BitField<u64, bool, 0, 1> enable;
    BitField<u64, BlendFunc, 1, 4> src_blend;
    BitField<u64, BlendFunc, 5, 4> src_alpha_blend;
    BitField<u64, BlendFunc, 9, 4> dst_blend;
    BitField<u64, BlendFunc, 13, 4> dst_alpha_blend;
    BitField<u64, BlendOp, 17, 3> blend_op;
    BitField<u64, BlendOp, 20, 3> alpha_blend_op;
    BitField<u64, bool, 24, 1> write_r;
    BitField<u64, bool, 25, 1> write_g;
    BitField<u64, bool, 26, 1> write_b;
    BitField<u64, bool, 27, 1> write_a;
    BitField<u64, u8, 24, 4> write_mask;
    BitField<u64, u32, 32, 32> constant;

    BitField<u64, u16, 1, 16> blend_factors;
    BitField<u64, u8, 17, 6> blend_ops;

    u64 key;

    // clang-format off
    ALWAYS_INLINE BlendState() = default;
    ALWAYS_INLINE BlendState(const BlendState& rhs) : key(rhs.key) {}
    ALWAYS_INLINE BlendState& operator=(const BlendState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const BlendState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const BlendState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const BlendState& rhs) const { return key < rhs.key; }
    // clang-format on

    // clang-format off
    ALWAYS_INLINE float GetConstantRed() const { return static_cast<float>(constant.GetValue() & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantGreen() const { return static_cast<float>((constant.GetValue() >> 8) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantBlue() const { return static_cast<float>((constant.GetValue() >> 16) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantAlpha() const { return static_cast<float>((constant.GetValue() >> 24) & 0xFF) / 255.0f; }
    // clang-format on
    ALWAYS_INLINE std::array<float, 4> GetConstantFloatColor() const
    {
      return std::array<float, 4>{GetConstantRed(), GetConstantGreen(), GetConstantBlue(), GetConstantAlpha()};
    }

    static BlendState GetNoBlendingState();
    static BlendState GetAlphaBlendingState();
  };

  struct GraphicsConfig
  {
    Layout layout;

    Primitive primitive;
    InputLayout input_layout;

    RasterizationState rasterization;
    DepthState depth;
    BlendState blend;

    GPUShader* vertex_shader;
    GPUShader* geometry_shader;
    GPUShader* fragment_shader;

    GPUTexture::Format color_formats[4];
    GPUTexture::Format depth_format;
    u8 samples;
    bool per_sample_shading;
    RenderPassFlag render_pass_flags;

    void SetTargetFormats(GPUTexture::Format color_format,
                          GPUTexture::Format depth_format_ = GPUTexture::Format::Unknown);
    u32 GetRenderTargetCount() const;
  };

  GPUPipeline();
  virtual ~GPUPipeline();

  virtual void SetDebugName(std::string_view name) = 0;
};

class GPUTextureBuffer
{
public:
  enum class Format
  {
    R16UI,

    MaxCount
  };

  GPUTextureBuffer(Format format, u32 size_in_elements);
  virtual ~GPUTextureBuffer();

  static u32 GetElementSize(Format format);

  ALWAYS_INLINE Format GetFormat() const { return m_format; }
  ALWAYS_INLINE u32 GetSizeInElements() const { return m_size_in_elements; }
  ALWAYS_INLINE u32 GetSizeInBytes() const { return m_size_in_elements * GetElementSize(m_format); }
  ALWAYS_INLINE u32 GetCurrentPosition() const { return m_current_position; }

  virtual void* Map(u32 required_elements) = 0;
  virtual void Unmap(u32 used_elements) = 0;

  virtual void SetDebugName(std::string_view name) = 0;

protected:
  Format m_format;
  u32 m_size_in_elements;
  u32 m_current_position = 0;
};

class GPUDevice
{
public:
  friend GPUTexture;

  // TODO: drop virtuals
  // TODO: gpu crash handling on present
  using DrawIndex = u16;

  enum FeatureMask : u32
  {
    FEATURE_MASK_DUAL_SOURCE_BLEND = (1 << 0),
    FEATURE_MASK_FEEDBACK_LOOPS = (1 << 1),
    FEATURE_MASK_FRAMEBUFFER_FETCH = (1 << 2),
    FEATURE_MASK_TEXTURE_BUFFERS = (1 << 3),
    FEATURE_MASK_GEOMETRY_SHADERS = (1 << 4),
    FEATURE_MASK_TEXTURE_COPY_TO_SELF = (1 << 5),
    FEATURE_MASK_MEMORY_IMPORT = (1 << 6),
    FEATURE_MASK_RASTER_ORDER_VIEWS = (1 << 7),
  };

  enum class DrawBarrier : u32
  {
    None,
    One,
    Full
  };

  enum class PresentResult : u32
  {
    OK,
    SkipPresent,
    DeviceLost,
  };

  struct Features
  {
    bool dual_source_blend : 1;
    bool framebuffer_fetch : 1;
    bool per_sample_shading : 1;
    bool noperspective_interpolation : 1;
    bool texture_copy_to_self : 1;
    bool supports_texture_buffers : 1;
    bool texture_buffers_emulated_with_ssbo : 1;
    bool feedback_loops : 1;
    bool geometry_shaders : 1;
    bool partial_msaa_resolve : 1;
    bool memory_import : 1;
    bool explicit_present : 1;
    bool timed_present : 1;
    bool gpu_timing : 1;
    bool shader_cache : 1;
    bool pipeline_cache : 1;
    bool prefer_unused_textures : 1;
    bool raster_order_views : 1;
  };

  struct Statistics
  {
    size_t buffer_streamed;
    u32 num_draws;
    u32 num_barriers;
    u32 num_render_passes;
    u32 num_copies;
    u32 num_downloads;
    u32 num_uploads;
  };

  struct AdapterInfo
  {
    std::string name;
    std::vector<std::string> fullscreen_modes;
    u32 max_texture_size;
    u32 max_multisamples;
    bool supports_sample_shading;
  };
  using AdapterInfoList = std::vector<AdapterInfo>;

  struct PooledTextureDeleter
  {
    void operator()(GPUTexture* const tex);
  };

  static constexpr u32 MAX_TEXTURE_SAMPLERS = 8;
  static constexpr u32 MIN_TEXEL_BUFFER_ELEMENTS = 4 * 1024 * 512;
  static constexpr u32 MAX_RENDER_TARGETS = 4;
  static constexpr u32 MAX_IMAGE_RENDER_TARGETS = 2;
  static constexpr u32 DEFAULT_CLEAR_COLOR = 0xFF000000u;
  static constexpr u32 PIPELINE_CACHE_HASH_SIZE = 20;
  static_assert(sizeof(GPUPipeline::GraphicsConfig::color_formats) == sizeof(GPUTexture::Format) * MAX_RENDER_TARGETS);

  GPUDevice();
  virtual ~GPUDevice();

  /// Returns the default/preferred API for the system.
  static RenderAPI GetPreferredAPI();

  /// Returns a string representing the specified API.
  static const char* RenderAPIToString(RenderAPI api);

  /// Returns a string representing the specified language.
  static const char* ShaderLanguageToString(GPUShaderLanguage language);

  /// Returns a new device for the specified API.
  static std::unique_ptr<GPUDevice> CreateDeviceForAPI(RenderAPI api);

  /// Returns true if the render API is the same (e.g. GLES and GL).
  static bool IsSameRenderAPI(RenderAPI lhs, RenderAPI rhs);

  /// Returns a list of adapters for the given API.
  static AdapterInfoList GetAdapterListForAPI(RenderAPI api);

  /// Parses a fullscreen mode into its components (width * height @ refresh hz)
  static bool GetRequestedExclusiveFullscreenMode(u32* width, u32* height, float* refresh_rate);

  /// Converts a fullscreen mode to a string.
  static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

  /// Returns the directory bad shaders are saved to.
  static std::string GetShaderDumpPath(std::string_view name);

  /// Dumps out a shader that failed compilation.
  static void DumpBadShader(std::string_view code, std::string_view errors);

  /// Converts a RGBA8 value to 4 floating-point values.
  static std::array<float, 4> RGBA8ToFloat(u32 rgba);

  /// Returns the number of texture bindings for a given pipeline layout.
  static constexpr u32 GetActiveTexturesForLayout(GPUPipeline::Layout layout)
  {
    constexpr std::array<u8, static_cast<u8>(GPUPipeline::Layout::MaxCount)> counts = {
      1,                    // SingleTextureAndUBO
      1,                    // SingleTextureAndPushConstants
      0,                    // SingleTextureBufferAndPushConstants
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndUBO
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndPushConstants
    };

    return counts[static_cast<u8>(layout)];
  }

  ALWAYS_INLINE const Features& GetFeatures() const { return m_features; }
  ALWAYS_INLINE RenderAPI GetRenderAPI() const { return m_render_api; }
  ALWAYS_INLINE u32 GetRenderAPIVersion() const { return m_render_api_version; }
  ALWAYS_INLINE u32 GetMaxTextureSize() const { return m_max_texture_size; }
  ALWAYS_INLINE u32 GetMaxMultisamples() const { return m_max_multisamples; }

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
  ALWAYS_INLINE s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
  ALWAYS_INLINE float GetWindowScale() const { return m_window_info.surface_scale; }
  ALWAYS_INLINE GPUTexture::Format GetWindowFormat() const { return m_window_info.surface_format; }

  ALWAYS_INLINE GPUSampler* GetLinearSampler() const { return m_linear_sampler.get(); }
  ALWAYS_INLINE GPUSampler* GetNearestSampler() const { return m_nearest_sampler.get(); }

  ALWAYS_INLINE bool IsGPUTimingEnabled() const { return m_gpu_timing_enabled; }

  bool Create(std::string_view adapter, std::string_view shader_cache_path, u32 shader_cache_version, bool debug_device,
              GPUVSyncMode vsync, bool allow_present_throttle, std::optional<bool> exclusive_fullscreen_control,
              FeatureMask disabled_features, Error* error);
  void Destroy();

  virtual bool HasSurface() const = 0;
  virtual void DestroySurface() = 0;
  virtual bool UpdateWindow() = 0;

  virtual bool SupportsExclusiveFullscreen() const;

  /// Call when the window size changes externally to recreate any resources.
  virtual void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) = 0;

  virtual std::string GetDriverInfo() const = 0;

  // Executes current command buffer, waits for its completion, and destroys all pending resources.
  virtual void ExecuteAndWaitForGPUIdle() = 0;

  virtual std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Type type, GPUTexture::Format format,
                                                    const void* data = nullptr, u32 data_stride = 0) = 0;
  virtual std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) = 0;
  virtual std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                u32 size_in_elements) = 0;

  // Texture pooling.
  std::unique_ptr<GPUTexture> FetchTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                           GPUTexture::Type type, GPUTexture::Format format, const void* data = nullptr,
                                           u32 data_stride = 0);
  std::unique_ptr<GPUTexture, PooledTextureDeleter>
  FetchAutoRecycleTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, GPUTexture::Type type,
                          GPUTexture::Format format, const void* data = nullptr, u32 data_stride = 0,
                          bool dynamic = false);
  void RecycleTexture(std::unique_ptr<GPUTexture> texture);
  void PurgeTexturePool();

  virtual std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height,
                                                                    GPUTexture::Format format) = 0;
  virtual std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                                    void* memory, size_t memory_size,
                                                                    u32 memory_stride) = 0;

  virtual void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                                 u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) = 0;
  virtual void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height) = 0;
  virtual void ClearRenderTarget(GPUTexture* t, u32 c);
  virtual void ClearDepth(GPUTexture* t, float d);
  virtual void InvalidateRenderTarget(GPUTexture* t);

  /// Shader abstraction.
  std::unique_ptr<GPUShader> CreateShader(GPUShaderStage stage, GPUShaderLanguage language, std::string_view source,
                                          Error* error = nullptr, const char* entry_point = "main");
  virtual std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config,
                                                      Error* error = nullptr) = 0;

  /// Debug messaging.
  virtual void PushDebugGroup(const char* name) = 0;
  virtual void PopDebugGroup() = 0;
  virtual void InsertDebugMessage(const char* msg) = 0;

  /// Vertex/index buffer abstraction.
  virtual void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                               u32* map_base_vertex) = 0;
  virtual void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) = 0;
  virtual void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) = 0;
  virtual void UnmapIndexBuffer(u32 used_size) = 0;

  void UploadVertexBuffer(const void* vertices, u32 vertex_size, u32 vertex_count, u32* base_vertex);
  void UploadIndexBuffer(const DrawIndex* indices, u32 index_count, u32* base_index);

  /// Uniform buffer abstraction.
  virtual void PushUniformBuffer(const void* data, u32 data_size) = 0;
  virtual void* MapUniformBuffer(u32 size) = 0;
  virtual void UnmapUniformBuffer(u32 size) = 0;
  void UploadUniformBuffer(const void* data, u32 data_size);

  /// Drawing setup abstraction.
  virtual void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags) = 0;
  virtual void SetPipeline(GPUPipeline* pipeline) = 0;
  virtual void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) = 0;
  virtual void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) = 0;
  virtual void SetViewport(const GSVector4i rc) = 0;
  virtual void SetScissor(const GSVector4i rc) = 0;
  void SetRenderTarget(GPUTexture* rt, GPUTexture* ds = nullptr,
                       GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags);
  void SetViewport(s32 x, s32 y, s32 width, s32 height);
  void SetScissor(s32 x, s32 y, s32 width, s32 height);
  void SetViewportAndScissor(s32 x, s32 y, s32 width, s32 height);
  void SetViewportAndScissor(const GSVector4i rc);

  // Drawing abstraction.
  virtual void Draw(u32 vertex_count, u32 base_vertex) = 0;
  virtual void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) = 0;
  virtual void DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type) = 0;

  /// Returns false if the window was completely occluded.
  virtual PresentResult BeginPresent(u32 clear_color = DEFAULT_CLEAR_COLOR) = 0;
  virtual void EndPresent(bool explicit_submit, u64 submit_time = 0) = 0;
  virtual void SubmitPresent() = 0;

  /// Renders ImGui screen elements. Call before EndPresent().
  void RenderImGui();

  ALWAYS_INLINE GPUVSyncMode GetVSyncMode() const { return m_vsync_mode; }
  ALWAYS_INLINE bool IsVSyncModeBlocking() const { return (m_vsync_mode == GPUVSyncMode::FIFO); }
  ALWAYS_INLINE bool IsPresentThrottleAllowed() const { return m_allow_present_throttle; }
  virtual void SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle) = 0;

  ALWAYS_INLINE bool IsDebugDevice() const { return m_debug_device; }
  ALWAYS_INLINE size_t GetVRAMUsage() const { return s_total_vram_usage; }

  bool UpdateImGuiFontTexture();
  bool UsesLowerLeftOrigin() const;
  static GSVector4i FlipToLowerLeft(GSVector4i rc, s32 target_height);
  bool ResizeTexture(std::unique_ptr<GPUTexture>* tex, u32 new_width, u32 new_height, GPUTexture::Type type,
                     GPUTexture::Format format, bool preserve = true);
  bool ShouldSkipPresentingFrame();
  void ThrottlePresentation();

  virtual bool SupportsTextureFormat(GPUTexture::Format format) const = 0;

  /// Enables/disables GPU frame timing.
  virtual bool SetGPUTimingEnabled(bool enabled);

  /// Returns the amount of GPU time utilized since the last time this method was called.
  virtual float GetAndResetAccumulatedGPUTime();

  ALWAYS_INLINE static Statistics& GetStatistics() { return s_stats; }
  static void ResetStatistics();

protected:
  virtual bool CreateDevice(std::string_view adapter, std::optional<bool> exclusive_fullscreen_control,
                            FeatureMask disabled_features, Error* error) = 0;
  virtual void DestroyDevice() = 0;

  std::string GetShaderCacheBaseName(std::string_view type) const;
  virtual bool OpenPipelineCache(const std::string& path, Error* error);
  virtual bool CreatePipelineCache(const std::string& path, Error* error);
  virtual bool ReadPipelineCache(DynamicHeapArray<u8> data, Error* error);
  virtual bool GetPipelineCacheData(DynamicHeapArray<u8>* data, Error* error);
  virtual bool ClosePipelineCache(const std::string& path, Error* error);

  virtual std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                            Error* error) = 0;
  virtual std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                            std::string_view source, const char* entry_point,
                                                            DynamicHeapArray<u8>* out_binary, Error* error) = 0;

  bool AcquireWindow(bool recreate_window);

  void TrimTexturePool();

  bool CompileGLSLShaderToVulkanSpv(GPUShaderStage stage, GPUShaderLanguage source_language, std::string_view source,
                                    const char* entry_point, bool optimization, bool nonsemantic_debug_info,
                                    DynamicHeapArray<u8>* out_binary, Error* error);
  bool TranslateVulkanSpvToLanguage(const std::span<const u8> spirv, GPUShaderStage stage,
                                    GPUShaderLanguage target_language, u32 target_version, std::string* output,
                                    Error* error);
  std::unique_ptr<GPUShader> TranspileAndCreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage source_language,
                                                                std::string_view source, const char* entry_point,
                                                                GPUShaderLanguage target_language, u32 target_version,
                                                                DynamicHeapArray<u8>* out_binary, Error* error);
  static std::optional<DynamicHeapArray<u8>> OptimizeVulkanSpv(const std::span<const u8> spirv, Error* error);

  Features m_features = {};
  RenderAPI m_render_api = RenderAPI::None;
  u32 m_render_api_version = 0;
  u32 m_max_texture_size = 0;
  u32 m_max_multisamples = 0;

  WindowInfo m_window_info;
  u64 m_last_frame_displayed_time = 0;

  GPUShaderCache m_shader_cache;

  std::unique_ptr<GPUSampler> m_nearest_sampler;
  std::unique_ptr<GPUSampler> m_linear_sampler;

private:
  static constexpr u32 MAX_TEXTURE_POOL_SIZE = 125;
  static constexpr u32 MAX_TARGET_POOL_SIZE = 50;
  static constexpr u32 POOL_PURGE_DELAY = 300;

  struct TexturePoolKey
  {
    u16 width;
    u16 height;
    u8 layers;
    u8 levels;
    u8 samples;
    GPUTexture::Type type;
    GPUTexture::Format format;
    u8 pad;

    ALWAYS_INLINE bool operator==(const TexturePoolKey& rhs) const
    {
      return std::memcmp(this, &rhs, sizeof(TexturePoolKey)) == 0;
    }
    ALWAYS_INLINE bool operator!=(const TexturePoolKey& rhs) const
    {
      return std::memcmp(this, &rhs, sizeof(TexturePoolKey)) != 0;
    }
  };
  struct TexturePoolEntry
  {
    std::unique_ptr<GPUTexture> texture;
    u32 use_counter;
    TexturePoolKey key;
  };

  using TexturePool = std::deque<TexturePoolEntry>;

#ifdef __APPLE__
  // We have to define these in the base class, because they're in Objective C++.
  static std::unique_ptr<GPUDevice> WrapNewMetalDevice();
  static AdapterInfoList WrapGetMetalAdapterList();
#endif

  void OpenShaderCache(std::string_view base_path, u32 version);
  void CloseShaderCache();
  bool CreateResources(Error* error);
  void DestroyResources();

  static bool IsTexturePoolType(GPUTexture::Type type);

  static size_t s_total_vram_usage;

  std::unique_ptr<GPUPipeline> m_imgui_pipeline;
  std::unique_ptr<GPUTexture> m_imgui_font_texture;

  TexturePool m_texture_pool;
  TexturePool m_target_pool;
  size_t m_pool_vram_usage = 0;
  u32 m_texture_pool_counter = 0;

protected:
  static Statistics s_stats;

  GPUVSyncMode m_vsync_mode = GPUVSyncMode::Disabled;
  bool m_allow_present_throttle = false;
  bool m_gpu_timing_enabled = false;
  bool m_debug_device = false;
};

extern std::unique_ptr<GPUDevice> g_gpu_device;

ALWAYS_INLINE void GPUDevice::PooledTextureDeleter::operator()(GPUTexture* const tex)
{
  g_gpu_device->RecycleTexture(std::unique_ptr<GPUTexture>(tex));
}

namespace Host {
/// Called when the core is creating a render device.
/// This could also be fullscreen transition.
std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window);

/// Called when the core is finished with a render window.
void ReleaseRenderWindow();

/// Returns true if the hosting application is currently fullscreen.
bool IsFullscreen();

/// Alters fullscreen state of hosting application.
void SetFullscreen(bool enabled);
} // namespace Host

// Macros for debug messages.
#ifdef _DEBUG
struct GLAutoPop
{
  GLAutoPop(int dummy) {}
  ~GLAutoPop() { g_gpu_device->PopDebugGroup(); }
};

#define GL_SCOPE(name) GLAutoPop gl_auto_pop((g_gpu_device->PushDebugGroup(name), 0))
#define GL_PUSH(name) g_gpu_device->PushDebugGroup(name)
#define GL_POP() g_gpu_device->PopDebugGroup()
#define GL_INS(msg) g_gpu_device->InsertDebugMessage(msg)
#define GL_OBJECT_NAME(obj, name) (obj)->SetDebugName(name)

#define GL_SCOPE_FMT(...)                                                                                              \
  GLAutoPop gl_auto_pop((g_gpu_device->PushDebugGroup(SmallString::from_format(__VA_ARGS__)), 0))
#define GL_PUSH_FMT(...) g_gpu_device->PushDebugGroup(SmallString::from_format(__VA_ARGS__))
#define GL_INS_FMT(...) g_gpu_device->InsertDebugMessage(SmallString::from_format(__VA_ARGS__))
#define GL_OBJECT_NAME_FMT(obj, ...) (obj)->SetDebugName(SmallString::from_format(__VA_ARGS__))
#else
#define GL_SCOPE(name) (void)0
#define GL_PUSH(name) (void)0
#define GL_POP() (void)0
#define GL_INS(msg) (void)0
#define GL_OBJECT_NAME(obj, name) (void)0

#define GL_SCOPE_FMT(...) (void)0
#define GL_PUSH_FMT(...) (void)0
#define GL_INS_FMT(...) (void)0
#define GL_OBJECT_NAME_FMT(obj, ...) (void)0
#endif