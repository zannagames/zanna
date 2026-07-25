//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/classes/RuntimeClasses.hpp
// Purpose: Runtime class metadata and unified signature registry for all
//   frontends.
//
// Key invariants:
//   - runtime.def remains the source of truth for catalog entries.
//   - Parsed method/property signatures stay frontend-neutral until adapted.
//
// Ownership/Lifetime:
//   - Catalog storage is immutable after initialization.
//   - RuntimeRegistry indexes are owned by function-local statics.
//
// Links: src/il/runtime/runtime.def, src/il/runtime/classes/RuntimeClasses.inc
//
//===----------------------------------------------------------------------===//
///
/// @file RuntimeClasses.hpp
/// @brief Runtime class metadata and unified signature registry for all frontends.
///
/// @details This file defines the data structures and interfaces for runtime
/// class metadata, enabling all Zanna frontends to access type information
/// about runtime library classes like Zanna.String, Zanna.File, etc.
///
/// ## Architecture Overview
///
/// Runtime class information flows through the system as follows:
///
/// ```
/// runtime.def          Source of truth for all runtime definitions
///      │
///      ▼ (rtgen tool)
/// RuntimeClasses.inc   Generated C++ macro invocations
///      │
///      ▼ (macro expansion)
/// runtimeClassCatalog()  Immutable vector of RuntimeClass descriptors
///      │
///      ▼ (builds hash indexes)
/// RuntimeRegistry       O(1) method/property lookup with parsed signatures
///      │
///      ├─────────────────┐
///      ▼                 ▼
/// BASIC Frontend    Zia Frontend
/// (toBasicType)     (toZiaType)
/// ```
///
/// ## Key Components
///
/// ### Raw Catalog (runtimeClassCatalog)
///
/// The catalog is a statically-initialized vector of RuntimeClass descriptors.
/// Each descriptor contains:
/// - Qualified name (e.g., "Zanna.String")
/// - Type ID for runtime type identification
/// - Properties with getter/setter targets
/// - Methods with signature strings
///
/// ### RuntimeRegistry (Singleton)
///
/// The registry builds hash indexes over the catalog for O(1) lookup:
/// - Methods indexed by "class|method#arity"
/// - Properties indexed by "class.property"
/// - Functions indexed by canonical extern name
///
/// ### Frontend-Agnostic Types (ILScalarType)
///
/// Parsed signatures use ILScalarType to represent types in a
/// frontend-independent way. Each frontend provides an adapter
/// (toBasicType, toZiaType) to convert to their native type systems.
///
/// ## Signature String Format
///
/// Method signatures use the format: `returnType(param1,param2,...)`
///
/// Type tokens:
/// - `i64` - 64-bit signed integer
/// - `f64` - 64-bit floating point
/// - `i1` - Boolean
/// - `str` - String reference
/// - `obj` / `ptr` - Object pointer
/// - `void` - No return value
///
/// Examples:
/// - `"str(i64,i64)"` - Returns string, takes two integers (String.Substring)
/// - `"i64()"` - Returns integer, no parameters (String.Length getter)
/// - `"void(str)"` - Returns void, takes string (StringBuilder.Append)
///
/// ## Thread Safety
///
/// The catalog and registry are built using function-local statics with
/// guaranteed thread-safe initialization. Once built, all data is immutable.
///
/// ## Invariants
///
/// - The catalog is immutable after construction
/// - All string fields point to static string literals or are nullptr
/// - Signatures omit the receiver (self/this); it's implicit arg0
/// - The registry provides case-insensitive lookup
///
/// @see RuntimeClasses.cpp - Implementation of catalog and registry
/// @see RuntimeClasses.inc - Generated class descriptors
/// @see runtime.def - Source definitions for runtime library
/// @see docs/il/il-guide.md - IL specification reference
///

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::runtime {

/// @brief Stable identifiers for runtime class types.
/// @note Only RTCLS_String is seeded for RC-1; future classes extend this enum.
enum class RuntimeTypeId : std::size_t {
    RTCLS_String = 0,
    RTCLS_StringBuilder,
    RTCLS_Object,
    RTCLS_File,
    RTCLS_Path,
    RTCLS_Dir,
    RTCLS_Glob,
    RTCLS_TempFile,
    RTCLS_List,
    RTCLS_Math,
    RTCLS_Convert,
    RTCLS_Random,
    RTCLS_Environment,
    RTCLS_SystemClipboard,
    RTCLS_Exec,
    RTCLS_Fmt,
    RTCLS_Canvas,
    RTCLS_BitmapFont,
    RTCLS_SpriteFont,
    RTCLS_Codec,
    RTCLS_Csv,
    RTCLS_Ini,
    RTCLS_Color,
    RTCLS_DateTime,
    RTCLS_TimeZone,
    RTCLS_RelativeTime,
    RTCLS_DateRange,
    RTCLS_InvariantNumberFormat,
    RTCLS_TextChar,
    RTCLS_BiMap,
    RTCLS_BitSet,
    RTCLS_BloomFilter,
    RTCLS_CountMap,
    RTCLS_DefaultMap,
    RTCLS_FrozenSet,
    RTCLS_FrozenMap,
    RTCLS_Easing,
    RTCLS_F64Buffer,
    RTCLS_I64Buffer,
    RTCLS_LruCache,
    RTCLS_Map,
    RTCLS_MultiMap,
    RTCLS_OrderedMap,
    RTCLS_Seq,
    RTCLS_Stack,
    RTCLS_TreeMap,
    RTCLS_Trie,
    RTCLS_UnionFind,
    RTCLS_SparseArray,
    RTCLS_WeakMap,
    RTCLS_ConcurrentQueue,
    RTCLS_ConcurrentMap,
    RTCLS_CancelToken,
    RTCLS_Debouncer,
    RTCLS_Throttler,
    RTCLS_Queue,
    RTCLS_Heap,
    RTCLS_PerlinNoise,
    RTCLS_Ring,
    RTCLS_Bits,
    RTCLS_Bytes,
    RTCLS_Bag,
    RTCLS_Set,
    RTCLS_BinFile,
    RTCLS_MemStream,
    RTCLS_Stream,
    RTCLS_LineReader,
    RTCLS_LineWriter,
    RTCLS_Watcher,
    RTCLS_SaveData,
    RTCLS_Compress,
    RTCLS_Archive,
    RTCLS_Assets,
    RTCLS_Log,
    RTCLS_MessageBus,
    RTCLS_Machine,
    RTCLS_Terminal,
    RTCLS_Clock,
    RTCLS_Countdown,
    RTCLS_Stopwatch,
    RTCLS_Duration,
    RTCLS_Uuid,
    RTCLS_Hash,
    RTCLS_LegacyHash,
    RTCLS_Json,
    RTCLS_KeyDerive,
    RTCLS_CryptoRand,
    RTCLS_CryptoModule,
    RTCLS_Cipher,
    RTCLS_Password,
    RTCLS_Pattern,
    RTCLS_Pluralize,
    RTCLS_Version,
    RTCLS_CompiledPattern,
    RTCLS_Template,
    RTCLS_Vec2,
    RTCLS_Vec3,
    RTCLS_Quat,
    RTCLS_Spline,
    RTCLS_Pixels,
    RTCLS_ThreadsMonitor,
    RTCLS_ThreadsSafeI64,
    RTCLS_ThreadsThread,
    RTCLS_ThreadsGate,
    RTCLS_ThreadsBarrier,
    RTCLS_ThreadsRwLock,
    RTCLS_ThreadsPromise,
    RTCLS_ThreadsFuture,
    RTCLS_ThreadsAsync,
    RTCLS_ThreadsParallel,
    RTCLS_Tcp,
    RTCLS_TcpServer,
    RTCLS_Udp,
    RTCLS_Dns,
    RTCLS_Http,
    RTCLS_HttpReq,
    RTCLS_HttpRes,
    RTCLS_Url,
    RTCLS_Tls,
    RTCLS_WebSocket,
    RTCLS_RestClient,
    RTCLS_RetryPolicy,
    RTCLS_Keyboard,
    RTCLS_InputKey,
    RTCLS_Mouse,
    RTCLS_Pad,
    RTCLS_Action,
    RTCLS_InputMgr,
    RTCLS_KeyChord,
    // Data structure classes
    RTCLS_Grid2D,
    RTCLS_Timer,
    // Game development abstractions
    RTCLS_StateMachine,
    RTCLS_Tween,
    RTCLS_ButtonGroup,
    RTCLS_SmoothValue,
    RTCLS_ParticleEmitter,
    RTCLS_ParticleSnapshot,
    RTCLS_SpriteAnimation,
    RTCLS_CollisionRect,
    RTCLS_Collision,
    RTCLS_SpriteSheet,
    RTCLS_Physics2DWorld,
    RTCLS_Physics2DBody,
    RTCLS_Physics2DCircleBody,
    RTCLS_Projectile2D,
    RTCLS_DistanceJoint,
    RTCLS_SpringJoint,
    RTCLS_HingeJoint,
    RTCLS_RopeJoint,
    RTCLS_Dialogue,
    RTCLS_Lighting2D,
    RTCLS_PlatformerController,
    RTCLS_AchievementTracker,
    RTCLS_Typewriter,
    RTCLS_ObjectPool,
    RTCLS_ScreenFX,
    RTCLS_PathFollower,
    RTCLS_Quadtree,
    RTCLS_GameQueryResult,
    RTCLS_QuadtreePairResult,
    RTCLS_DebugOverlay,
    // Audio classes
    RTCLS_Audio,
    RTCLS_Sound,
    RTCLS_Voice,
    RTCLS_Music,
    RTCLS_Playlist,
    RTCLS_SoundBank,
    RTCLS_Synth,
    RTCLS_MusicGen,
    // Graphics classes (extended)
    RTCLS_Sprite,
    RTCLS_SpriteAnimator,
    RTCLS_Tilemap,
    RTCLS_Camera,
    RTCLS_SceneNode,
    RTCLS_SceneGraph,
    RTCLS_SpriteBatch,
    // GUI classes
    RTCLS_GuiApp,
    RTCLS_GuiFont,
    RTCLS_GuiWidget,
    RTCLS_GuiLabel,
    RTCLS_GuiButton,
    RTCLS_GuiTextInput,
    RTCLS_GuiCheckbox,
    RTCLS_GuiScrollView,
    RTCLS_GuiTreeView,
    RTCLS_GuiTreeNode,
    RTCLS_GuiTabBar,
    RTCLS_GuiTab,
    RTCLS_GuiSplitPane,
    RTCLS_GuiCodeEditor,
    RTCLS_GuiEditorBuffer,
    RTCLS_GuiDropdown,
    RTCLS_GuiSlider,
    RTCLS_GuiProgressBar,
    RTCLS_GuiListBox,
    RTCLS_GuiOutputPane,
    RTCLS_GuiGrid,
    RTCLS_GuiPopupList,
    RTCLS_GuiRadioGroup,
    RTCLS_GuiRadioButton,
    RTCLS_GuiSpinner,
    RTCLS_GuiColorSwatch,
    RTCLS_GuiColorPalette,
    RTCLS_GuiColorPicker,
    RTCLS_GuiImage,
    RTCLS_GuiTheme,
    RTCLS_GuiThemePalette,
    RTCLS_GuiVBox,
    RTCLS_GuiHBox,
    RTCLS_GuiFlex,
    RTCLS_GuiLayoutGrid,
    RTCLS_GuiDockPanel,
    RTCLS_GuiMenuBar,
    RTCLS_GuiMenu,
    RTCLS_GuiMenuItem,
    RTCLS_GuiToolbar,
    RTCLS_GuiToolbarItem,
    RTCLS_GuiStatusBar,
    RTCLS_GuiStatusBarItem,
    RTCLS_GuiFindBar,
    RTCLS_GuiContextMenu,
    RTCLS_CryptoAes,
    RTCLS_LegacyCryptoAes,
    RTCLS_GuiClipboardText,
    RTCLS_GuiShortcuts,
    RTCLS_GuiCursor,
    RTCLS_GuiMessageBox,
    RTCLS_GuiFileDialog,
    RTCLS_GuiCommandPalette,
    RTCLS_GuiTooltip,
    RTCLS_GuiToast,
    RTCLS_GuiBreadcrumb,
    RTCLS_GuiMinimap,
    // Functional programming types
    RTCLS_Result,
    RTCLS_Option,
    // Additional collections
    RTCLS_Iterator,
    RTCLS_Deque,
    // Text utilities
    RTCLS_Diff,
    RTCLS_Toml,
    RTCLS_Markdown,
    RTCLS_JsonPath,
    RTCLS_JsonStream,
    RTCLS_Scanner,
    // Date/Time utilities
    RTCLS_DateOnly,
    RTCLS_TextWrapper,
    RTCLS_Lazy,
    RTCLS_LazySeq,
    RTCLS_SortedSet,
    RTCLS_Scheduler,
    RTCLS_RateLimiter,
    RTCLS_HttpRouter,
    RTCLS_RouteMatch,
    RTCLS_HttpServer,
    RTCLS_HttpsServer,
    RTCLS_ServerReq,
    RTCLS_ServerRes,
    RTCLS_ConnectionPool,
    RTCLS_Multipart,
    RTCLS_NetUtils,
    RTCLS_WsServer,
    RTCLS_WssServer,
    RTCLS_SseClient,
    RTCLS_HttpClient,
    RTCLS_SmtpClient,
    RTCLS_AsyncSocket,
    RTCLS_Html,
    RTCLS_IntMap,
    RTCLS_BinaryBuffer,
    // Newly exposed classes
    RTCLS_CoreBox,
    RTCLS_CoreDiag,
    RTCLS_Diagnostics,
    RTCLS_TrapInfo,
    RTCLS_CoreParse,
    RTCLS_RuntimeUnsafe,
    RTCLS_RuntimeGC,
    RTCLS_DataSerialize,
    RTCLS_DataXml,
    RTCLS_DataYaml,
    RTCLS_GuiContainer,
    RTCLS_MathBigInt,
    RTCLS_MathMat3,
    RTCLS_MathMat4,
    RTCLS_Memory,
    RTCLS_MemoryGC,
    RTCLS_CommandResult,
    RTCLS_ThreadsChannel,
    RTCLS_ThreadsPool,
    RTCLS_ZiaCompletion,
    RTCLS_BasicLanguageService,
    RTCLS_ZiaSemanticJob,
    RTCLS_ZiaSemanticJobHandle,
    RTCLS_ZiaProjectIndex,
    RTCLS_ZiaProjectIndexHandle,
    RTCLS_GuiFloatingPanel,
    RTCLS_GuiGroupBox,
    // Game UI widgets
    RTCLS_Pathfinder,
    RTCLS_PathResult,
    RTCLS_UIHudLabel,
    RTCLS_UIBar,
    RTCLS_UIPanel,
    RTCLS_UINineSlice,
    RTCLS_UIMenuList,
    RTCLS_UIHudTextInput,
    RTCLS_UITable,
    RTCLS_UITableClickResult,
    RTCLS_UIModal,
    RTCLS_UIHudSlider,
    RTCLS_UIHudDropdown,
    RTCLS_UIHudTooltip,
    RTCLS_GameButton,

    // Graphics 2D production surface
    RTCLS_RenderTarget2D,
    RTCLS_Surface2D,
    RTCLS_Texture2D,
    RTCLS_GpuTexture2D,
    RTCLS_Renderer2D,
    RTCLS_Material2D,
    RTCLS_Shader2D,
    RTCLS_PostProcess2D,
    RTCLS_Viewport2D,
    RTCLS_ScreenScaler,
    RTCLS_TileSet2D,
    RTCLS_TileLayer2D,
    RTCLS_ObjectLayer2D,
    RTCLS_AutoTile2D,
    RTCLS_ParticleSystem2D,
    RTCLS_Emitter2D,
    RTCLS_Path2D,
    RTCLS_ShapeRenderer2D,
    RTCLS_TextRenderer2D,
    RTCLS_SdfFont,
    RTCLS_NineSlice2D,
    RTCLS_DebugDraw2D,
    RTCLS_Transform2D,
    RTCLS_Sampler2D,
    RTCLS_BlendState2D,
    RTCLS_SpriteRenderer2D,
    RTCLS_TileChunkCache2D,
    RTCLS_TilemapRenderer2D,
    RTCLS_AnimationClip2D,
    RTCLS_AnimatedSprite2D,
    RTCLS_TextLayout2D,
    RTCLS_RenderPass2D,
    RTCLS_RenderGraph2D,
    RTCLS_CollisionMask2D,
    RTCLS_Hitbox2D,
    RTCLS_Palette2D,
    RTCLS_Gradient2D,
    RTCLS_CameraRig2D,
    RTCLS_TexturePackerAtlas,
    RTCLS_AsepriteImporter,
    RTCLS_TiledMapLoader,

    // Graphics 3D
    RTCLS_CubeMap3D,
    RTCLS_RenderTarget3D,
    RTCLS_Canvas3D,
    RTCLS_Mesh3D,
    RTCLS_Camera3D,
    RTCLS_Material3D,
    RTCLS_Light3D,
    RTCLS_Scene3D,
    RTCLS_SceneNode3D,
    RTCLS_Model3D,
    RTCLS_Skeleton3D,
    RTCLS_Animation3D,
    RTCLS_AnimPlayer3D,
    RTCLS_AssetDiagnostics3D,
    RTCLS_FBX,
    RTCLS_GLTF,
    RTCLS_MorphTarget3D,
    RTCLS_Particles3D,
    RTCLS_PostFX3D,
    RTCLS_Ray3D,
    RTCLS_RayHit3D,
    RTCLS_SpatialAudio3D,
    RTCLS_SoundListener3D,
    RTCLS_SoundSource3D,
    RTCLS_Physics3DWorld,
    RTCLS_Collider3D,
    RTCLS_PhysicsHit3D,
    RTCLS_PhysicsHitList3D,
    RTCLS_LedgeHit3D,
    RTCLS_Ragdoll3D,
    RTCLS_CollisionEvent3D,
    RTCLS_ContactPoint3D,
    RTCLS_Physics3DBody,
    RTCLS_Character3D,
    RTCLS_Trigger3D,
    RTCLS_DistanceJoint3D,
    RTCLS_SpringJoint3D,
    RTCLS_HingeJoint3D,
    RTCLS_RopeJoint3D,
    RTCLS_SixDofJoint3D,
    RTCLS_Vehicle3D,
    RTCLS_Game3DLayers,
    RTCLS_Game3DBodyShape,
    RTCLS_Game3DSyncMode,
    RTCLS_Game3DAlphaMode,
    RTCLS_Game3DShadingModel,
    RTCLS_Game3DQualityLevel,
    RTCLS_Game3DCollisionPhase,
    RTCLS_Game3DKeys,
    RTCLS_Game3DMouseButtons,
    RTCLS_Game3DLayerMask,
    RTCLS_Game3DInput3D,
    RTCLS_Game3DEntity3D,
    RTCLS_Game3DSound3D,
    RTCLS_Game3DEffectRegistry3D,
    RTCLS_Game3DEffects3D,
    RTCLS_Game3DCharacterController3D,
    RTCLS_Game3DFirstPersonController,
    RTCLS_Game3DFreeFlyController,
    RTCLS_Game3DOrbitController,
    RTCLS_Game3DFollowController,
    RTCLS_Game3DThirdPersonController,
    RTCLS_Game3DTargetLock3D,
    RTCLS_Game3DHitbox3D,
    RTCLS_Game3DHitboxKind,
    RTCLS_Game3DHitEvent3D,
    RTCLS_Game3DHealth3D,
    RTCLS_Game3DDamageEvent3D,
    RTCLS_Game3DRailCamera3D,
    RTCLS_Game3DTimeline3D,
    RTCLS_Game3DDialogue3D,
    RTCLS_Game3DLipSync3D,
    RTCLS_Game3DWorld3D,
    RTCLS_Game3DLighting,
    RTCLS_Game3DMaterials,
    RTCLS_Game3DPostFX,
    RTCLS_Game3DQuality,
    RTCLS_Game3DPrefab,
    RTCLS_Game3DBodyDef,
    RTCLS_Game3DCollision3DEvent,
    RTCLS_Game3DAssets3D,
    RTCLS_Game3DModelTemplate,
    RTCLS_Game3DBehavior3D,
    RTCLS_Game3DAnimator3D,
    RTCLS_Game3DEnvironment3D,
    RTCLS_Game3DEnvHandle,
    RTCLS_Game3DDebug3D,
    RTCLS_Game3DDiagnostics3D,
    RTCLS_Transform3D,
    RTCLS_Path3D,
    RTCLS_InstanceBatch3D,
    RTCLS_Terrain3D,
    RTCLS_LightBaker3D,
    RTCLS_LightProbeGrid3D,
    RTCLS_ReflectionProbe3D,
    RTCLS_Sky3D,
    RTCLS_TimeOfDay3D,
    RTCLS_Game3DSurfaces,
    RTCLS_Game3DSurfaceTable3D,
    RTCLS_Game3DFootsteps3D,
    RTCLS_Game3DInteractable3D,
    RTCLS_Game3DInteractor3D,
    RTCLS_Game3DPerception3D,
    RTCLS_Game3DBehaviorTree3D,
    RTCLS_Game3DBehaviorTreeInstance3D,
    RTCLS_Game3DReverbZone3D,
    RTCLS_Game3DAmbientBed3D,
    RTCLS_Cloth3D,
    RTCLS_Game3DMinimap3D,
    RTCLS_GameQuests,
    RTCLS_GameQuestState,
    RTCLS_GameQuestEventKind,
    RTCLS_Game3DRenderPass,
    RTCLS_Game3DHitchSource,
    RTCLS_NavMesh3D,
    RTCLS_NavAgent3D,
    RTCLS_AnimBlend3D,
    RTCLS_AnimController3D,
    RTCLS_Decal3D,
    RTCLS_Sprite3D,
    RTCLS_LensFlare3D,
    RTCLS_Vegetation3D,
    RTCLS_VideoPlayer,
    RTCLS_VideoWidget,
    RTCLS_Water3D,
    RTCLS_TextureAtlas3D,
    RTCLS_TextureAtlas2D,
    RTCLS_AnimStateMachine,
    RTCLS_AnimationEventBatch,
    RTCLS_AnimTimeline,
    RTCLS_Locale,
    RTCLS_LocaleInfo,
    RTCLS_LocaleManager,
    RTCLS_LocPluralRules,
    RTCLS_LocNumberFormat,
    RTCLS_LocDateFormat,
    RTCLS_LocRelTimeFormat,
    RTCLS_LocMessageBundle,
    RTCLS_LocListFormat,
    RTCLS_LocTextDirection,
    RTCLS_LocCollator,
    RTCLS_CoreValueType,
    RTCLS_MemoryWeakRef,
    RTCLS_WorldToScreenProjection,
    RTCLS_ZiaToolchain,
    RTCLS_Process,
    RTCLS_ProcessHandle,
    RTCLS_Pty,
    RTCLS_PtySession,
    RTCLS_Shutdown,
    RTCLS_GuiTestHarness,
    RTCLS_GuiVirtualList,
    RTCLS_GuiVirtualTree,
    RTCLS_GuiCommandState,
    RTCLS_GuiCommand,
    RTCLS_GuiCommandRegistry,
    RTCLS_GuiAccessibility,
    RTCLS_AssetResolver,
    RTCLS_WorkspaceFileIndex,
    RTCLS_WorkspaceWatcher,
    RTCLS_ProjectManifest,
    RTCLS_WorkspaceEdit,
    RTCLS_FuzzyMatch,
    RTCLS_GameScene,
    RTCLS_Game3DAssetHandle3D,
    RTCLS_Game3DWorldStream3D,
    RTCLS_TextureAsset3D,
    RTCLS_BlendTree3D,
    RTCLS_IKSolver3D,
    RTCLS_NodeAnimation3D,
    RTCLS_NodeAnimator3D,
    RTCLS_ZiaDocument,
    RTCLS_GuiSystem,
    RTCLS_GuiAlign,
    RTCLS_GuiJustify,
    RTCLS_GuiFlexDirection,
    RTCLS_GuiFlexWrap,
    RTCLS_GuiDock,
    RTCLS_GuiThemeMode,
    RTCLS_GuiAccessibleRole,
    RTCLS_GuiLiveRegionMode,
    RTCLS_GuiDialogButtonRole,
    RTCLS_GuiDialogStatus,
    RTCLS_GuiImageFilter,
    RTCLS_GuiSortDirection,
};

/// @brief Describes a property on a runtime class.
/// @details Properties surface as getters/setters. Setters may be null when
///          the property is read-only.
struct RuntimeProperty {
    const char *name{nullptr};   ///< Property name (e.g., "Length").
    const char *type{nullptr};   ///< IL scalar type (e.g., "i64", "i1").
    const char *getter{nullptr}; ///< Canonical extern target (e.g., "Zanna.String.get_Length").
    const char *setter{nullptr}; ///< Canonical extern target or nullptr if read-only.
    bool readonly{false};        ///< True if setter is nullptr.
};

/// @brief Describes a method on a runtime class.
struct RuntimeMethod {
    const char *name{nullptr};      ///< Method name (e.g., "Substring").
    const char *signature{nullptr}; ///< Signature string in compact IL grammar.
    const char *target{nullptr};    ///< Canonical extern target (e.g., "Zanna.String.Substring").
};

/// @brief Describes a runtime class and its members.
struct RuntimeClass {
    const char *qname{nullptr};   ///< Fully-qualified name (e.g., "Zanna.String").
    const char *layout{nullptr};  ///< Layout descriptor (opaque until object model defined).
    const char *baseQName{nullptr}; ///< Optional fully-qualified base class name.
    const char *ctor{nullptr};    ///< Optional ctor helper extern; may be nullptr.
    const char *summary{nullptr}; ///< Short authored class description.
    const char *details{nullptr}; ///< Long authored Markdown class description.
    RuntimeTypeId typeId{RuntimeTypeId::RTCLS_String}; ///< Stable type identifier.
    std::vector<RuntimeProperty> properties;           ///< Declared properties.
    std::vector<RuntimeMethod> methods;                ///< Declared methods.
};

//===----------------------------------------------------------------------===//
/// @name Frontend-Agnostic Type System
/// @brief Parsed signature types shared across all frontends.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Frontend-agnostic scalar types for runtime signatures.
/// @details Frontends map these to their native type systems.
enum class ILScalarType : std::uint8_t {
    Void,   ///< void return type
    I64,    ///< 64-bit signed integer
    F64,    ///< 64-bit floating point
    Bool,   ///< Boolean (i1)
    String, ///< String reference (str)
    Object, ///< Object pointer (obj/ptr)
    Unknown ///< Unrecognized or parse error
};

/// @brief Parsed signature with structured type information.
/// @details Extracted from signature strings like "str(i64,i64)" or "seq<str>(str,str)".
struct ParsedSignature {
    ILScalarType returnType{ILScalarType::Unknown};
    bool isOptionalReturn{false};
    bool rawPointerReturn{false};
    std::vector<ILScalarType> params;
    std::vector<bool> rawPointerParams;

    /// @brief Parameterized outer return type name (e.g. "seq" from "seq<str>").
    /// @details Empty for non-parameterized returns. Frontends can use this to
    /// recover the concrete runtime collection class behind an IL Object return.
    std::string containerTypeName;

    /// @brief Element type name for parameterized seq/list returns (e.g. "str" from "seq<str>").
    /// @details Empty for plain obj/ptr returns. When non-empty, frontends should produce
    /// a typed sequence/collection type instead of an opaque pointer.
    std::string elementTypeName;

    /// @brief Runtime class name for typed object returns (e.g. "Zanna.Audio.Sound"
    /// from "obj<Zanna.Sound.Sound>").
    /// @details Empty for plain obj/ptr returns. When non-empty, frontends should surface
    /// the return value as that concrete runtime class instead of an opaque pointer.
    std::string objectTypeName;

    /// @brief Check if the signature was parsed successfully.
    /// @return True when the return token mapped to a known scalar category.
    [[nodiscard]] bool isValid() const {
        return returnType != ILScalarType::Unknown;
    }

    /// @brief Get the number of parameters (excluding receiver).
    /// @return Number of explicit ABI parameters.
    [[nodiscard]] std::size_t arity() const {
        return params.size();
    }
};

/// @brief Extended method descriptor with parsed signature.
struct ParsedMethod {
    const char *name{nullptr};   ///< Method name (e.g., "Substring").
    const char *target{nullptr}; ///< Canonical extern target.
    ParsedSignature signature;
};

/// @brief Extended property descriptor with parsed type.
struct ParsedProperty {
    const char *name{nullptr};                ///< Property name (e.g., "Length").
    ILScalarType type{ILScalarType::Unknown}; ///< Resolved property type.
    const char *getter{nullptr};              ///< Getter extern target.
    const char *setter{nullptr};              ///< Setter extern target or nullptr.
    bool readonly{false};                     ///< True if setter is nullptr.
};

/// @brief Parse a signature string like "str(i64,i64)" into structured form.
/// @param sig The signature string from RuntimeMethod.
/// @return Parsed signature with return type and parameter types.
ParsedSignature parseRuntimeSignature(std::string_view sig);

/// @brief Recover a concrete runtime class name from a parsed object-return signature.
/// @details Returns the fully-qualified runtime class for signatures that carry
///          object or container annotations such as `obj<Zanna.Result>(...)`
///          and `seq<str>(...)`. Returns an empty string when the signature
///          does not identify a concrete runtime class.
/// @param sig Parsed runtime signature to inspect.
/// @return Concrete runtime class name or empty when the return remains opaque.
std::string concreteRuntimeReturnClassQName(const ParsedSignature &sig);

/// @brief Map IL token (i64, f64, str, etc.) to ILScalarType.
/// @param tok Token from signature parsing.
/// @return Corresponding ILScalarType, or Unknown if unrecognized.
ILScalarType mapILToken(std::string_view tok);

/// @brief Unified runtime registry with parsed signatures and lookup.
/// @details Provides O(1) lookup for methods and properties by building
/// hash indexes over the runtime class catalog. Frontends use this
/// registry and map ILScalarType to their native type systems.
///
/// ## Usage
/// ```cpp
/// const auto& reg = RuntimeRegistry::instance();
/// auto method = reg.findMethod("Zanna.String", "Substring", 2);
/// if (method) {
///     // method->signature.returnType, method->signature.params
/// }
/// ```
class RuntimeRegistry {
  public:
    /// @brief Get the singleton instance.
    static const RuntimeRegistry &instance();

    /// @brief Find a method by class, name, and arity.
    /// @param classQName Fully-qualified class name (e.g., "Zanna.String").
    /// @param methodName Method name (e.g., "Substring").
    /// @param arity Number of parameters (excluding receiver).
    /// @return Parsed method info if found, nullopt otherwise.
    /// @note Comparison is case-insensitive.
    [[nodiscard]] std::optional<ParsedMethod> findMethod(std::string_view classQName,
                                                         std::string_view methodName,
                                                         std::size_t arity) const;

    /// @brief Find a property by class and name.
    /// @param classQName Fully-qualified class name.
    /// @param propertyName Property name.
    /// @return Parsed property info if found, nullopt otherwise.
    /// @note Comparison is case-insensitive.
    [[nodiscard]] std::optional<ParsedProperty> findProperty(std::string_view classQName,
                                                             std::string_view propertyName) const;

    /// @brief Find a function signature by canonical extern name.
    /// @param canonicalName Full extern name (e.g., "Zanna.String.Substring").
    /// @return Parsed signature if found, nullopt otherwise.
    /// @note Comparison is case-insensitive.
    [[nodiscard]] std::optional<ParsedSignature> findFunction(std::string_view canonicalName) const;

    /// @brief List available method arities for diagnostics.
    /// @param classQName Fully-qualified class name.
    /// @param methodName Method name.
    /// @return List of strings like "Substring/2" for each arity found.
    [[nodiscard]] std::vector<std::string> methodCandidates(std::string_view classQName,
                                                            std::string_view methodName) const;

    /// @brief Get the underlying raw catalog.
    /// @return Reference to the runtime class catalog.
    [[nodiscard]] const std::vector<RuntimeClass> &rawCatalog() const;

  private:
    /// @brief Build the singleton's lookup indexes from the immutable catalog.
    RuntimeRegistry();
    /// @brief Populate all method, property, and canonical-function indexes.
    void buildIndexes();

    /// @brief Build a case-insensitive method-overload lookup key.
    /// @param cls Qualified runtime class name.
    /// @param method Method name.
    /// @param arity Number of explicit parameters.
    /// @return Lowercase `class|method#arity` key.
    static std::string methodKey(std::string_view cls, std::string_view method, std::size_t arity);
    /// @brief Build a case-insensitive property lookup key.
    /// @param cls Qualified runtime class name.
    /// @param prop Property name.
    /// @return Lowercase `class.property` key.
    static std::string propertyKey(std::string_view cls, std::string_view prop);
    /// @brief Build a case-insensitive canonical function lookup key.
    /// @param name Canonical runtime function name.
    /// @return Lowercase copy of @p name.
    static std::string functionKey(std::string_view name);
    /// @brief Convert an ASCII lookup component to lowercase.
    /// @param s Text to normalize.
    /// @return Owned lowercase key component.
    static std::string toLower(std::string_view s);

    std::unordered_map<std::string, ParsedMethod> methodIndex_;
    std::unordered_map<std::string, ParsedProperty> propertyIndex_;
    std::unordered_map<std::string, ParsedSignature> functionIndex_;
};

/// @}

/// @brief Expose the immutable runtime class catalog.
/// @returns Const reference to a statically initialized vector of descriptors.
/// @signature-grammar
///   ret(args) where:
///     - ret, args are IL scalars: i64, f64, i1, str, obj, void
///     - Methods implicitly take the receiver as arg0 (not spelled in signature)
///     - Example: "str(i64,i64)" for String.Substring(start,len) -> string
const std::vector<RuntimeClass> &runtimeClassCatalog();

/// @brief Find a runtime class by its fully-qualified name.
/// @param qname Fully-qualified class name (e.g., "Zanna.String").
/// @returns Pointer to the matching RuntimeClass, or nullptr if not found.
/// @note Comparison is case-insensitive.
const RuntimeClass *findRuntimeClassByQName(std::string_view qname);

} // namespace il::runtime
