//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/RuntimeSignatures.cpp
// Purpose: Build and expose the runtime descriptor registry used by IL
//          consumers to marshal calls into the C runtime. Contains the
//          descriptor table definitions, lookup APIs, and validation logic.
//
// Generated Data: This file includes generated/RuntimeSignatures.inc which
//                 contains DescriptorRow entries for all runtime functions.
//                 See docs/internals/generated-files.md for regeneration instructions.
//
// Key invariants: The descriptor table is immutable, matches runtime helpers
//                 one-to-one, and is initialised lazily in a thread-safe manner.
// Ownership/Lifetime: All descriptors have static storage duration and remain
//                     valid for the lifetime of the process.
// Links: docs/internals/generated-files.md, docs/il/il-guide.md#reference
//
// Related files:
//   - generated/RuntimeSignatures.inc: Generated descriptor rows
//   - RuntimeSignatures_Handlers.hpp: Handler templates (DirectHandler, etc.)
//   - RuntimeSignatures_Handlers.cpp: Adapter function implementations
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Runtime descriptor registry and public lookup APIs.
/// @details Contains the descriptor table definitions mapping runtime symbol
///          names to their signatures, handlers, and lowering metadata. Handler
///          templates and adapter functions are defined in the companion
///          RuntimeSignatures_Handlers files.

#include "il/runtime/RuntimeSignatures.hpp"

#include <atomic>

#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeOwnership.hpp"
#include "il/runtime/RuntimeSignatureParser.hpp"
#include "il/runtime/RuntimeSignaturesData.hpp"
#include "il/runtime/RuntimeSignatures_Handlers.hpp"

#include "rt.hpp"
#include "rt_achievement.h"
#include "rt_action.h"
#include "rt_aes.h"
#include "rt_animation_events.h"
#include "rt_animcontroller3d.h"
#include "rt_archive.h"
#include "rt_args.h"
#include "rt_array.h"
#include "rt_array_f64.h"
#include "rt_array_i64.h"
#include "rt_array_obj.h"
#include "rt_array_str.h"
#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_async.h"
#include "rt_audio.h"
#include "rt_bag.h"
#include "rt_behavior.h"
#include "rt_bigint.h"
#include "rt_bimap.h"
#include "rt_binbuf.h"
#include "rt_binfile.h"
#include "rt_bitmapfont.h"
#include "rt_ttf_font.h"
#include "rt_bits.h"
#include "rt_bitset.h"
#include "rt_blendtree3d.h"
#include "rt_bloomfilter.h"
#include "rt_box.h"
#include "rt_buttongroup.h"
#include "rt_bytes.h"
#include "rt_camera.h"
#include "rt_cancellation.h"
#include "rt_canvas3d.h"
#include "rt_channel.h"
#include "rt_cipher.h"
#include "rt_cloth3d.h"
#include "rt_codec.h"
#include "rt_collator.h"
#include "rt_collider3d.h"
#include "rt_collision.h"
#include "rt_compiled_pattern.h"
#include "rt_compress.h"
#include "rt_concmap.h"
#include "rt_concqueue.h"
#include "rt_config.h"
#include "rt_context.h"
#include "rt_convert_coll.h"
#include "rt_countdown.h"
#include "rt_countmap.h"
#include "rt_crypto_module.h"
#include "rt_csv.h"
#include "rt_dateformat.h"
#include "rt_dateonly.h"
#include "rt_daterange.h"
#include "rt_datetime.h"
#include "rt_debounce.h"
#include "rt_debug.h"
#include "rt_debugoverlay.h"
#include "rt_decal3d.h"
#include "rt_defaultmap.h"
#include "rt_deque.h"
#include "rt_dialogue.h"
#include "rt_diff.h"
#include "rt_dir.h"
#include "rt_duration.h"
#include "rt_easing.h"
#include "rt_entity.h"
#include "rt_error.h"
#include "rt_exc.h"
#include "rt_exec.h"
#include "rt_fbx_loader.h"
#include "rt_file.h"
#include "rt_file_ext.h"
#include "rt_file_path.h"
#include "rt_fmt.h"
#include "rt_format.h"
#include "rt_fp.h"
#include "rt_frozenmap.h"
#include "rt_frozenset.h"
#include "rt_future.h"
#include "rt_fuzzy_match.h"
#include "rt_game3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_gameui.h"
#include "rt_gc.h"
#include "rt_glob.h"
#include "rt_gltf.h"
#include "rt_graphics.h"
#include "rt_graphics2d.h"
#include "rt_grid2d.h"
#include "rt_gui.h"
#include "rt_gui_constants.h"
#include "rt_gui_ide.h"
#include "rt_guid.h"
#include "rt_hash.h"
#include "rt_heap.h"
#include "rt_html.h"
#include "rt_ide_primitives.h"
#include "rt_iksolver3d.h"
#include "rt_ini.h"
#include "rt_input.h"
#include "rt_inputmgr.h"
#include "rt_instbatch3d.h"
#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_intmap.h"
#include "rt_iter.h"
#include "rt_joints3d.h"
#include "rt_json.h"
#include "rt_json_stream.h"
#include "rt_jsonpath.h"
#include "rt_keychord.h"
#include "rt_keyderive.h"
#include "rt_lazy.h"
#include "rt_lazyseq.h"
#include "rt_lensflare3d.h"
#include "rt_leveldata.h"
#include "rt_lightbaker3d.h"
#include "rt_lighting2d.h"
#include "rt_linereader.h"
#include "rt_linewriter.h"
#include "rt_list.h"
#include "rt_list_format.h"
#include "rt_locale.h"
#include "rt_locale_info.h"
#include "rt_locale_manager.h"
#include "rt_log.h"
#include "rt_lrucache.h"
#include "rt_machine.h"
#include "rt_map.h"
#include "rt_markdown.h"
#include "rt_mat3.h"
#include "rt_mat4.h"
#include "rt_math.h"
#include "rt_memstream.h"
#include "rt_mesh_simplify.h"
#include "rt_message_bundle.h"
#include "rt_mixgroup.h"
#include "rt_model3d.h"
#include "rt_modvar.h"
#include "rt_morphtarget3d.h"
#include "rt_msgbus.h"
#include "rt_multimap.h"
#include "rt_musicgen.h"
#include "rt_navagent3d.h"
#include "rt_navmesh3d.h"
#include "rt_network.h"
#include "rt_numbuf.h"
#include "rt_numeric.h"
#include "rt_numfmt.h"
#include "rt_numformat.h"
#include "rt_object.h"
#include "rt_objpool.h"
#include "rt_oop.h"
#include "rt_option.h"
#include "rt_orderedmap.h"
#include "rt_output.h"
#include "rt_parallel.h"
#include "rt_parse.h"
#include "rt_particle.h"
#include "rt_particles3d.h"
#include "rt_password.h"
#include "rt_path.h"
#include "rt_path3d.h"
#include "rt_pathfinder.h"
#include "rt_pathfollow.h"
#include "rt_perlin.h"
#include "rt_physics2d.h"
#include "rt_physics2d_joint.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platformer_ctrl.h"
#include "rt_playlist.h"
#include "rt_plural_rules.h"
#include "rt_pluralize.h"
#include "rt_postfx3d.h"
#include "rt_pqueue.h"
#include "rt_printf_compat.h"
#include "rt_embed_host.h"
#include "rt_process.h"
#include "rt_pty.h"
#include "rt_quadtree.h"
#include "rt_quat.h"
#include "rt_quests.h"
#include "rt_queue.h"
#include "rt_ragdoll3d.h"
#include "rt_rand.h"
#include "rt_random.h"
#include "rt_ratelimit.h"
#include "rt_raycast2d.h"
#include "rt_raycast3d.h"
#include "rt_reflectionprobe3d.h"
#include "rt_reltime_format.h"
#include "rt_scenemanager.h"
#include "rt_sky3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_sprite3d.h"
#include "rt_terrain3d.h"
#include "rt_texatlas3d.h"
#include "rt_text_direction.h"
#include "rt_timeofday3d.h"
#include "rt_transform3d.h"
#include "rt_vegetation3d.h"
#include "rt_videoplayer.h"
#include "rt_videowidget.h"
#include "rt_water3d.h"
#include "rt_world_projection.h"
// New network classes
#include "rt_animstate.h"
#include "rt_animtimeline.h"
#include "rt_async_socket.h"
#include "rt_connpool.h"
#include "rt_http_client.h"
#include "rt_http_router.h"
#include "rt_http_server.h"
#include "rt_https_server.h"
#include "rt_multipart.h"
#include "rt_netutils.h"
#include "rt_regex.h"
#include "rt_reltime.h"
#include "rt_restclient.h"
#include "rt_result.h"
#include "rt_retry.h"
#include "rt_ring.h"
#include "rt_savedata.h"
#include "rt_sb_bridge.h"
#include "rt_scanner.h"
#include "rt_scene.h"
#include "rt_scene3d.h"
#include "rt_scene_editor.h"
#include "rt_scheduler.h"
#include "rt_screenfx.h"
#include "rt_seq.h"
#include "rt_seq_functional.h"
#include "rt_serialize.h"
#include "rt_set.h"
#include "rt_shutdown.h"
#include "rt_skeleton3d.h"
#include "rt_smoothvalue.h"
#include "rt_smtp.h"
#include "rt_sortedset.h"
#include "rt_soundbank.h"
#include "rt_sparsearray.h"
#include "rt_spline.h"
#include "rt_sprite.h"
#include "rt_spriteanim.h"
#include "rt_spritebatch.h"
#include "rt_spritesheet.h"
#include "rt_sse.h"
#include "rt_stack.h"
#include "rt_statemachine.h"
#include "rt_stopwatch.h"
#include "rt_stream.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_synth.h"
#include "rt_table.h"
#include "rt_tempfile.h"
#include "rt_template.h"
#include "rt_testing.h"
#include "rt_textureasset3d.h"
#include "rt_textwrap.h"
#include "rt_threadpool.h"
#include "rt_threads.h"
#include "rt_tilemap.h"
#include "rt_timer.h"
#include "rt_timezone.h"
#include "rt_tls.h"
#include "rt_toml.h"
#include "rt_trap.h"
#include "rt_treemap.h"
#include "rt_trie.h"
#include "rt_tween.h"
#include "rt_typewriter.h"
#include "rt_unionfind.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include "rt_version.h"
#include "rt_watcher.h"
#include "rt_weakmap.h"
#include "rt_websocket.h"
#include "rt_ws_server.h"
#include "rt_wss_server.h"
#include "rt_xml.h"
#include "rt_yaml.h"
#include "zanna/runtime/rt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NDEBUG
#include "il/runtime/signatures/Registry.hpp"
#include <cassert>
#include <unordered_set>

namespace il::runtime::signatures {
/// @brief Register debug expectations for file-I/O runtime helpers.
void register_fileio_signatures();
/// @brief Register debug expectations for string runtime helpers.
void register_string_signatures();
/// @brief Register debug expectations for mathematical runtime helpers.
void register_math_signatures();
/// @brief Register debug expectations for array runtime helpers.
void register_array_signatures();
/// @brief Register debug expectations for object-model runtime helpers.
void register_oop_signatures();
} // namespace il::runtime::signatures
#endif

namespace il::runtime {
namespace {

using Kind = il::core::Type::Kind;

constexpr std::size_t kRtSigCount = data::kRtSigCount;

/// @brief Merge name-based ownership facts into a parsed runtime signature.
/// @param signature Signature updated in place without clearing existing facts.
/// @param name Canonical or native helper name used for classification.
void applyOwnershipEffects(RuntimeSignature &signature, std::string_view name) {
    const auto ownership = classifyRuntimeOwnership(name);
    signature.consumedArgMask |= ownership.consumedArgMask;
    signature.retainedArgMask |= ownership.retainedArgMask;
    signature.ownedOutArgMask |= ownership.ownedOutArgMask;
    signature.returnsOwned = signature.returnsOwned || ownership.returnsOwned;
    signature.mayAllocate = signature.mayAllocate || ownership.mayAllocate;
    signature.returnsKnownObject =
        signature.returnsKnownObject || ownership.returnsKnownObject;
}

/// @brief Retrieve the parsed runtime signature for a generated enumerator.
///
/// @details Lazily initialises an array of signatures by parsing the table of
///          specification strings emitted at build time.  Subsequent lookups
///          return references into the cached array without additional parsing.
///
/// @param sig Enumerated runtime signature identifier.
/// @return Reference to the parsed runtime signature.
const RuntimeSignature &signatureFor(RtSig sig) {
    /// Parse and annotate the generated signature table on first use.
    static const auto table = [] {
        std::array<RuntimeSignature, kRtSigCount> entries;
        for (std::size_t i = 0; i < kRtSigCount; ++i) {
            entries[i] = parseSignatureSpec(data::kRtSigSpecs[i]);
            const auto effects = classifyHelperEffects(data::kRtSigSymbolNames[i]);
            entries[i].nothrow = effects.nothrow;
            entries[i].readonly = effects.readonly;
            entries[i].pure = effects.pure;
            applyOwnershipEffects(entries[i], data::kRtSigSymbolNames[i]);
        }
        return entries;
    }();
    return table[static_cast<std::size_t>(sig)];
}

/// @brief Check whether a runtime signature enumerator is in range.
///
/// @param sig Enumerated runtime signature identifier.
/// @return True when @p sig maps to a generated signature entry.
bool isValid(RtSig sig) {
    return static_cast<std::size_t>(sig) < kRtSigCount;
}

// Handler templates (DirectHandler, ConsumingStringHandler) and adapter functions
// (invokeRtArr*, trapFromRuntimeString, etc.) are defined in RuntimeSignatures_Handlers.hpp/cpp.

/// @brief Construct a runtime lowering descriptor with optional feature gating.
///
/// @details Packs the lowering metadata into a @ref RuntimeLowering structure,
///          optionally recording the feature enum and whether ordering matters
///          when comparing runtime calls.
/// @param kind Declaration-selection strategy.
/// @param feature Optional feature key when @p kind is Feature.
/// @param ordered Whether declaration request order must be preserved.
/// @return Aggregate lowering metadata.
constexpr RuntimeLowering makeLowering(RuntimeLoweringKind kind,
                                       RuntimeFeature feature = RuntimeFeature::Count,
                                       bool ordered = false) {
    return RuntimeLowering{kind, feature, ordered};
}

// Adapter functions (invokeRtArr*, invokeRtCint*, etc.) are defined in
// RuntimeSignatures_Handlers.cpp and declared in RuntimeSignatures_Handlers.hpp.

constexpr RuntimeLowering kAlwaysLowering = makeLowering(RuntimeLoweringKind::Always);
constexpr RuntimeLowering kBoundsCheckedLowering = makeLowering(RuntimeLoweringKind::BoundsChecked);
constexpr RuntimeLowering kManualLowering = makeLowering(RuntimeLoweringKind::Manual);

/// @brief Helper that records a runtime feature requirement for lowering.
///
/// @details Produces a @ref RuntimeLowering descriptor indicating that the
///          runtime function should only be linked when the given feature is
///          requested by the front end.
/// @param feature Feature that requests the helper.
/// @param ordered Whether request order must be preserved.
/// @return Feature-gated lowering metadata.
constexpr RuntimeLowering featureLowering(RuntimeFeature feature, bool ordered = false) {
    return makeLowering(RuntimeLoweringKind::Feature, feature, ordered);
}

/// @brief VM-only handler that reports non-native execution.
/// @details Used for Zanna.System.Environment.IsNative so VM runs return false while
///          native binaries link against the real rt_env_is_native helper.
/// @param args Ignored argument array; the helper has no visible parameters.
/// @param result Required result slot receiving integer false.
void vm_env_is_native(void ** /*args*/, void *result) {
    detail::storeRequiredResult<int64_t>(result, 0);
}

constexpr std::array<RuntimeHiddenParam, 1> kPowHidden{
    RuntimeHiddenParam{RuntimeHiddenParamKind::PowStatusPointer}};

/// @brief Compile-time source row for one runtime registry descriptor.
struct DescriptorRow {
    /// Exported runtime name used for registry lookup.
    std::string_view name;
    /// Optional identifier into the generated compact signature table.
    std::optional<RtSig> signatureId;
    /// Public textual signature used when no generated identifier is available.
    std::string_view spec;
    /// VM adapter that invokes the underlying C implementation.
    RuntimeHandler handler{nullptr};
    /// Policy controlling when lowering declares the helper.
    RuntimeLowering lowering;
    /// Borrowed pointer to hidden bridge-parameter metadata.
    const RuntimeHiddenParam *hidden{nullptr};
    /// Number of entries reachable through @ref hidden.
    std::size_t hiddenCount{0};
    /// Runtime trap translation applied after invocation.
    RuntimeTrapClass trapClass{RuntimeTrapClass::None};
    /// Whether frontends may expose this row as a standalone runtime function.
    bool publicSurface{true};
    /// Backing linker-visible C symbol, if distinct metadata is available.
    std::string_view cSymbol{};
};

// Use deduced array size to avoid mismatches that would create an empty default row.
constexpr auto kDescriptorRows = std::to_array<DescriptorRow>({
    DescriptorRow{"rt_abort",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_abort, void, const char *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
#include "il/runtime/RuntimeSignatures.inc"
// Legacy C-symbol aliases for BASIC frontends (DUAL mode only).
// These provide lookup-by-C-name for functions whose canonical names are in
// the generated RuntimeSignatures.inc above.
#if ZANNA_RUNTIME_NS_DUAL
    DescriptorRow{"rt_print_str",
                  RtSig::PrintS,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::PrintS)],
                  &DirectHandler<&rt_print_str, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_print_i64",
                  RtSig::PrintI,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::PrintI)],
                  &DirectHandler<&rt_print_i64, void, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_print_f64",
                  RtSig::PrintF,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::PrintF)],
                  &DirectHandler<&rt_print_f64, void, double>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_len",
                  RtSig::Len,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::Len)],
                  &DirectHandler<&rt_str_len, int64_t, rt_string>::invoke,
                  kAlwaysLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_byte_at",
                  std::nullopt,
                  "i64(string,i64)",
                  &DirectHandler<&rt_str_byte_at, int64_t, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::ByteAt),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_substr",
                  RtSig::Substr,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::Substr)],
                  &DirectHandler<&rt_str_substr, rt_string, rt_string, int64_t, int64_t>::invoke,
                  kAlwaysLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_diag_assert",
                  RtSig::DiagAssert,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::DiagAssert)],
                  &DirectHandler<&rt_diag_assert, void, int8_t, rt_string>::invoke,
                  kBoundsCheckedLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_trap_string",
                  RtSig::Trap,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::Trap)],
                  &trapFromRuntimeString,
                  kBoundsCheckedLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_concat",
                  RtSig::Concat,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::Concat)],
                  &ConsumingStringHandler<&rt_str_concat, rt_string, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Concat),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_input_line",
                  RtSig::InputLine,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::InputLine)],
                  &DirectHandler<&rt_input_line, rt_string>::invoke,
                  featureLowering(RuntimeFeature::InputLine),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_to_int",
                  RtSig::ToInt,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::ToInt)],
                  &DirectHandler<&rt_to_int, int64_t, rt_string>::invoke,
                  featureLowering(RuntimeFeature::ToInt),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_to_double",
                  RtSig::ToDouble,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::ToDouble)],
                  &DirectHandler<&rt_to_double, double, rt_string>::invoke,
                  featureLowering(RuntimeFeature::ToDouble),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_parse_int64",
                  RtSig::ParseInt64,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::ParseInt64)],
                  &DirectHandler<&rt_parse_int64, int32_t, const char *, int64_t *>::invoke,
                  featureLowering(RuntimeFeature::ParseInt64),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_parse_double",
                  RtSig::ParseDouble,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::ParseDouble)],
                  &DirectHandler<&rt_parse_double, int32_t, const char *, double *>::invoke,
                  featureLowering(RuntimeFeature::ParseDouble),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_int_to_str",
                  RtSig::IntToStr,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::IntToStr)],
                  &DirectHandler<&rt_int_to_str, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::IntToStr),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_f64_to_str",
                  RtSig::F64ToStr,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::F64ToStr)],
                  &DirectHandler<&rt_f64_to_str, rt_string, double>::invoke,
                  featureLowering(RuntimeFeature::F64ToStr),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_i16_alloc",
                  RtSig::StrFromI16,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::StrFromI16)],
                  &DirectHandler<&rt_str_i16_alloc, rt_string, int16_t>::invoke,
                  featureLowering(RuntimeFeature::StrFromI16),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_i32_alloc",
                  RtSig::StrFromI32,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::StrFromI32)],
                  &DirectHandler<&rt_str_i32_alloc, rt_string, int32_t>::invoke,
                  featureLowering(RuntimeFeature::StrFromI32),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_f_alloc",
                  RtSig::StrFromSingle,
                  data::kRtSigSpecs[static_cast<std::size_t>(RtSig::StrFromSingle)],
                  &invokeRtStrFAlloc,
                  featureLowering(RuntimeFeature::StrFromSingle),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
#endif
    // C-symbol rows for functions in runtime.def that lack manual entries.
    DescriptorRow{
        "rt_str_split_fields",
        RtSig::SplitFields,
        data::kRtSigSpecs[static_cast<std::size_t>(RtSig::SplitFields)],
        &DirectHandler<&rt_str_split_fields, int64_t, rt_string, rt_string *, int64_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{"rt_str_eq",
                  std::nullopt,
                  "bool(string, string)",
                  &DirectHandler<&rt_str_eq, int8_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::StrEq),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_println_i32",
                  std::nullopt,
                  "void(i32)",
                  &DirectHandler<&rt_println_i32, void, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_println_str",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_println_str, void, const char *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_csv_quote_alloc",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_csv_quote_alloc, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::CsvQuote),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_cls",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_cls, void>::invoke,
                  featureLowering(RuntimeFeature::TermCls),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Legacy terminal helpers for BASIC frontends (raw rt_term_* names).
    DescriptorRow{"rt_term_color_i32",
                  std::nullopt,
                  "void(i32,i32)",
                  &DirectHandler<&rt_term_color_i32, void, int32_t, int32_t>::invoke,
                  featureLowering(RuntimeFeature::TermColor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_locate_i32",
                  std::nullopt,
                  "void(i32,i32)",
                  &DirectHandler<&rt_term_locate_i32, void, int32_t, int32_t>::invoke,
                  featureLowering(RuntimeFeature::TermLocate),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_cursor_visible_i32",
                  std::nullopt,
                  "void(i32)",
                  &DirectHandler<&rt_term_cursor_visible_i32, void, int32_t>::invoke,
                  featureLowering(RuntimeFeature::TermCursor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_alt_screen_i32",
                  std::nullopt,
                  "void(i32)",
                  &DirectHandler<&rt_term_alt_screen_i32, void, int32_t>::invoke,
                  featureLowering(RuntimeFeature::TermAltScreen),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_bell",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_bell, void>::invoke,
                  featureLowering(RuntimeFeature::TermBell),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Provide dotted name for INKEY$ access through the Zanna.Terminal namespace
    // Blocking key input - waits for a key press
    // Blocking key input with timeout (milliseconds)
    // Output buffering for improved terminal rendering performance
    DescriptorRow{"rt_term_begin_batch",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_begin_batch, void>::invoke,
                  featureLowering(RuntimeFeature::TermBeginBatch),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_end_batch",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_end_batch, void>::invoke,
                  featureLowering(RuntimeFeature::TermEndBatch),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_flush",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_flush, void>::invoke,
                  featureLowering(RuntimeFeature::TermFlush),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // --- Zanna.Terminal I/O functions (Say = newline, Print = no newline) ---
    DescriptorRow{"rt_term_say",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_term_say, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_say_i64",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_term_say_i64, void, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_say_f64",
                  std::nullopt,
                  "void(f64)",
                  &DirectHandler<&rt_term_say_f64, void, double>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_say_bool",
                  std::nullopt,
                  "void(i1)",
                  &DirectHandler<&rt_term_say_bool, void, int8_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_print",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_term_print, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_print_i64",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_term_print_i64, void, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_print_f64",
                  std::nullopt,
                  "void(f64)",
                  &DirectHandler<&rt_term_print_f64, void, double>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_ask",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_term_ask, rt_string, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_read_line",
                  std::nullopt,
                  "string()",
                  &DirectHandler<&rt_term_read_line, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // i64 wrappers (for frontends that use 64-bit integers)
    DescriptorRow{"rt_term_locate",
                  std::nullopt,
                  "void(i64,i64)",
                  &DirectHandler<&rt_term_locate, void, int64_t, int64_t>::invoke,
                  featureLowering(RuntimeFeature::TermLocate),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_color",
                  std::nullopt,
                  "void(i64,i64)",
                  &DirectHandler<&rt_term_color, void, int64_t, int64_t>::invoke,
                  featureLowering(RuntimeFeature::TermColor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_textcolor",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_term_textcolor, void, int64_t>::invoke,
                  featureLowering(RuntimeFeature::TermColor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_textbg",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_term_textbg, void, int64_t>::invoke,
                  featureLowering(RuntimeFeature::TermColor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_hide_cursor",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_hide_cursor, void>::invoke,
                  featureLowering(RuntimeFeature::TermCursor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_term_show_cursor",
                  std::nullopt,
                  "void()",
                  &DirectHandler<&rt_term_show_cursor, void>::invoke,
                  featureLowering(RuntimeFeature::TermCursor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_keypressed",
                  std::nullopt,
                  "i32()",
                  &DirectHandler<&rt_keypressed, int32_t>::invoke,
                  featureLowering(RuntimeFeature::KeyPressed),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_keypressed_i64",
                  std::nullopt,
                  "i64()",
                  &DirectHandler<&rt_keypressed_i64, int64_t>::invoke,
                  featureLowering(RuntimeFeature::KeyPressed),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sleep_ms_i64",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_sleep_ms_i64, void, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sleep_ms",
                  std::nullopt,
                  "void(i32)",
                  &DirectHandler<&rt_sleep_ms, void, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Legacy time helpers (raw rt_* names).
    DescriptorRow{"rt_timer_ms",
                  std::nullopt,
                  "i64()",
                  &DirectHandler<&rt_timer_ms, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // --- Argument store helpers ---
    DescriptorRow{"rt_args_count",
                  std::nullopt,
                  "i64()",
                  &DirectHandler<&rt_args_count, int64_t>::invoke,
                  featureLowering(RuntimeFeature::ArgsCount),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_args_get",
                  std::nullopt,
                  "string(i64)",
                  &DirectHandler<&rt_args_get, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::ArgsGet),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cmdline",
                  std::nullopt,
                  "string()",
                  &DirectHandler<&rt_cmdline, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Cmdline),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_env_is_native",
                  std::nullopt,
                  "i1()",
                  &vm_env_is_native,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_getkey_str",
                  std::nullopt,
                  "string()",
                  &DirectHandler<&rt_getkey_str, rt_string>::invoke,
                  featureLowering(RuntimeFeature::GetKey),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_getkey_timeout_i32",
                  std::nullopt,
                  "string(i32)",
                  &DirectHandler<&rt_getkey_timeout_i32, rt_string, int32_t>::invoke,
                  featureLowering(RuntimeFeature::GetKeyTimeout),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_inkey_str",
                  std::nullopt,
                  "string()",
                  &DirectHandler<&rt_inkey_str, rt_string>::invoke,
                  featureLowering(RuntimeFeature::InKey),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cint_from_double",
                  std::nullopt,
                  "i64(f64,ptr)",
                  &invokeRtCintFromDouble,
                  featureLowering(RuntimeFeature::CintFromDouble),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_clng_from_double",
                  std::nullopt,
                  "i64(f64,ptr)",
                  &invokeRtClngFromDouble,
                  featureLowering(RuntimeFeature::ClngFromDouble),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_csng_from_double",
                  std::nullopt,
                  "f64(f64,ptr)",
                  &invokeRtCsngFromDouble,
                  featureLowering(RuntimeFeature::CsngFromDouble),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cdbl_from_any",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_cdbl_from_any, double, double>::invoke,
                  featureLowering(RuntimeFeature::CdblFromAny),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_int_floor",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_int_floor, double, double>::invoke,
                  featureLowering(RuntimeFeature::IntFloor),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_fix_trunc",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_fix_trunc, double, double>::invoke,
                  featureLowering(RuntimeFeature::FixTrunc),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_round_even",
                  std::nullopt,
                  "f64(f64,i32)",
                  &invokeRtRoundEven,
                  featureLowering(RuntimeFeature::RoundEven),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_alloc",
                  std::nullopt,
                  "ptr(i64)",
                  &DirectHandler<&rt_alloc, void *, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Alloc),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_new",
                  std::nullopt,
                  "ptr(i64)",
                  &invokeRtArrI32New,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_retain",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_i32_retain, void, int32_t *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_release",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_i32_release, void, int32_t *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_len",
                  std::nullopt,
                  "i64(ptr)",
                  &invokeRtArrI32Len,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_get",
                  std::nullopt,
                  "i64(ptr,i64)",
                  &invokeRtArrI32Get,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_get_fast",
                  std::nullopt,
                  "i64(ptr,i64)",
                  &invokeRtArrI32GetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_set",
                  std::nullopt,
                  "void(ptr,i64,i64)",
                  &invokeRtArrI32Set,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_set_fast",
                  std::nullopt,
                  "void(ptr,i64,i64)",
                  &invokeRtArrI32SetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i32_resize",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &invokeRtArrI32Resize,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // I64 (LONG) array helpers
    DescriptorRow{"rt_arr_i64_new",
                  std::nullopt,
                  "ptr(i64)",
                  &invokeRtArrI64New,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_retain",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_i64_retain, void, int64_t *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_release",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_i64_release, void, int64_t *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_len",
                  std::nullopt,
                  "i64(ptr)",
                  &invokeRtArrI64Len,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_get",
                  std::nullopt,
                  "i64(ptr,i64)",
                  &invokeRtArrI64Get,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_get_fast",
                  std::nullopt,
                  "i64(ptr,i64)",
                  &invokeRtArrI64GetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_set",
                  std::nullopt,
                  "void(ptr,i64,i64)",
                  &invokeRtArrI64Set,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_set_fast",
                  std::nullopt,
                  "void(ptr,i64,i64)",
                  &invokeRtArrI64SetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_i64_resize",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &invokeRtArrI64Resize,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // F64 (SINGLE/DOUBLE) array helpers
    DescriptorRow{"rt_arr_f64_new",
                  std::nullopt,
                  "ptr(i64)",
                  &invokeRtArrF64New,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_retain",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_f64_retain, void, double *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_release",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_f64_release, void, double *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_len",
                  std::nullopt,
                  "i64(ptr)",
                  &invokeRtArrF64Len,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_get",
                  std::nullopt,
                  "f64(ptr,i64)",
                  &invokeRtArrF64Get,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_get_fast",
                  std::nullopt,
                  "f64(ptr,i64)",
                  &invokeRtArrF64GetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_set",
                  std::nullopt,
                  "void(ptr,i64,f64)",
                  &invokeRtArrF64Set,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_set_fast",
                  std::nullopt,
                  "void(ptr,i64,f64)",
                  &invokeRtArrF64SetFast,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_f64_resize",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &invokeRtArrF64Resize,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_oob_panic",
                  std::nullopt,
                  "void(i64,i64)",
                  &invokeRtArrOobPanic,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_str_alloc",
                  std::nullopt,
                  "ptr(i64)",
                  &invokeRtArrStrAlloc,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_str_release",
                  std::nullopt,
                  "void(ptr,i64)",
                  &invokeRtArrStrRelease,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_str_get",
                  std::nullopt,
                  "string(ptr,i64)",
                  &invokeRtArrStrGet,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_str_put",
                  std::nullopt,
                  "void(ptr,i64,str)",
                  &invokeRtArrStrPut,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_str_len",
                  std::nullopt,
                  "i64(ptr)",
                  &invokeRtArrStrLen,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Object array helpers (void* elements).
    DescriptorRow{"rt_arr_obj_new",
                  std::nullopt,
                  "ptr(i64)",
                  &invokeRtArrObjNew,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_obj_len",
                  std::nullopt,
                  "i64(ptr)",
                  &invokeRtArrObjLen,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_obj_get",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &invokeRtArrObjGet,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_obj_put",
                  std::nullopt,
                  "void(ptr,i64,ptr)",
                  &invokeRtArrObjPut,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_obj_resize",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &invokeRtArrObjResize,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_arr_obj_release",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_arr_obj_release, void, void **>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_left",
                  std::nullopt,
                  "string(string,i64)",
                  &DirectHandler<&rt_str_left, rt_string, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Left),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_right",
                  std::nullopt,
                  "string(string,i64)",
                  &DirectHandler<&rt_str_right, rt_string, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Right),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_mid",
                  std::nullopt,
                  "string(string,i64)",
                  &DirectHandler<&rt_str_mid, rt_string, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Mid2),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_mid_len",
                  std::nullopt,
                  "string(string,i64,i64)",
                  &DirectHandler<&rt_str_mid_len, rt_string, rt_string, int64_t, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Mid3),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_index_of",
                  std::nullopt,
                  "i64(string,string)",
                  &DirectHandler<&rt_str_index_of, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Instr2),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_instr3",
                  std::nullopt,
                  "i64(i64,string,string)",
                  &DirectHandler<&rt_str_instr3, int64_t, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Instr3),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_ltrim",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_str_ltrim, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Ltrim),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_rtrim",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_str_rtrim, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Rtrim),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_trim",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_str_trim, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Trim),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_ucase",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_str_ucase, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Ucase),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_lcase",
                  std::nullopt,
                  "string(string)",
                  &DirectHandler<&rt_str_lcase, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Lcase),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_chr",
                  std::nullopt,
                  "string(i64)",
                  &DirectHandler<&rt_str_chr, rt_string, int64_t>::invoke,
                  featureLowering(RuntimeFeature::Chr),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_asc",
                  std::nullopt,
                  "i64(string)",
                  &DirectHandler<&rt_str_asc, int64_t, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Asc),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // String comparison functions - return i64 (0 or non-zero)
    DescriptorRow{"rt_str_lt",
                  std::nullopt,
                  "i64(string,string)",
                  &DirectHandler<&rt_str_lt, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::StrLt),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_le",
                  std::nullopt,
                  "i64(string,string)",
                  &DirectHandler<&rt_str_le, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::StrLe),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_gt",
                  std::nullopt,
                  "i64(string,string)",
                  &DirectHandler<&rt_str_gt, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::StrGt),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_ge",
                  std::nullopt,
                  "i64(string,string)",
                  &DirectHandler<&rt_str_ge, int64_t, rt_string, rt_string>::invoke,
                  featureLowering(RuntimeFeature::StrGe),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_val",
                  std::nullopt,
                  "f64(string)",
                  &DirectHandler<&rt_val, double, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Val),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_val_to_double",
                  std::nullopt,
                  "f64(ptr,ptr)",
                  &DirectHandler<&rt_val_to_double, double, const char *, bool *>::invoke,
                  featureLowering(RuntimeFeature::Val),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_string_cstr",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_string_cstr, const char *, rt_string>::invoke,
                  featureLowering(RuntimeFeature::Val),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sqrt",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_sqrt, double, double>::invoke,
                  featureLowering(RuntimeFeature::Sqrt, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_abs_i64",
                  std::nullopt,
                  "i64(i64)",
                  &DirectHandler<&rt_abs_i64, long long, long long>::invoke,
                  featureLowering(RuntimeFeature::AbsI64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_abs_f64",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_abs_f64, double, double>::invoke,
                  featureLowering(RuntimeFeature::AbsF64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_floor",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_floor, double, double>::invoke,
                  featureLowering(RuntimeFeature::Floor, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_ceil",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_ceil, double, double>::invoke,
                  featureLowering(RuntimeFeature::Ceil, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sin",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_sin, double, double>::invoke,
                  featureLowering(RuntimeFeature::Sin, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cos",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_cos, double, double>::invoke,
                  featureLowering(RuntimeFeature::Cos, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_tan",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_tan, double, double>::invoke,
                  featureLowering(RuntimeFeature::Tan, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_atan",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_atan, double, double>::invoke,
                  featureLowering(RuntimeFeature::Atan, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_exp",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_exp, double, double>::invoke,
                  featureLowering(RuntimeFeature::Exp, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_log",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_log, double, double>::invoke,
                  featureLowering(RuntimeFeature::Log, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sgn_i64",
                  std::nullopt,
                  "i64(i64)",
                  &DirectHandler<&rt_sgn_i64, long long, long long>::invoke,
                  featureLowering(RuntimeFeature::SgnI64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_sgn_f64",
                  std::nullopt,
                  "f64(f64)",
                  &DirectHandler<&rt_sgn_f64, double, double>::invoke,
                  featureLowering(RuntimeFeature::SgnF64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_pow_f64_chkdom",
                  std::nullopt,
                  "f64(f64,f64)",
                  &invokeRtPowF64Chkdom,
                  featureLowering(RuntimeFeature::Pow, true),
                  kPowHidden.data(),
                  kPowHidden.size(),
                  RuntimeTrapClass::PowDomainOverflow},
    DescriptorRow{"rt_pow_f64",
                  std::nullopt,
                  "f64(f64,f64)",
                  &vmInvokeRtPow,
                  featureLowering(RuntimeFeature::Pow, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::PowDomainOverflow},
    DescriptorRow{"rt_math_pow",
                  std::nullopt,
                  "f64(f64,f64)",
                  &vmInvokeRtPow,
                  featureLowering(RuntimeFeature::Pow, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_randomize_i64",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_randomize_i64, void, long long>::invoke,
                  featureLowering(RuntimeFeature::RandomizeI64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Canonical name for RANDOMIZE
    DescriptorRow{"Zanna.Math.Randomize",
                  std::nullopt,
                  "void(i64)",
                  &DirectHandler<&rt_randomize_i64, void, long long>::invoke,
                  featureLowering(RuntimeFeature::RandomizeI64, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_rnd",
                  std::nullopt,
                  "f64()",
                  &DirectHandler<&rt_rnd, double>::invoke,
                  featureLowering(RuntimeFeature::Rnd, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Canonical name for RND
    DescriptorRow{"Zanna.Math.Rnd",
                  std::nullopt,
                  "f64()",
                  &DirectHandler<&rt_rnd, double>::invoke,
                  featureLowering(RuntimeFeature::Rnd, true),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{
        "rt_open_err_vstr",
        std::nullopt,
        "i32(string,i32,i32)",
        &DirectHandler<&rt_open_err_vstr, int32_t, ZannaString *, int32_t, int32_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{"rt_close_err",
                  std::nullopt,
                  "i32(i32)",
                  &DirectHandler<&rt_close_err, int32_t, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_write_ch_err",
                  std::nullopt,
                  "i32(i32,string)",
                  &DirectHandler<&rt_write_ch_err, int32_t, int32_t, ZannaString *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_println_ch_err",
                  std::nullopt,
                  "i32(i32,string)",
                  &DirectHandler<&rt_println_ch_err, int32_t, int32_t, ZannaString *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_line_input_ch_err",
                  std::nullopt,
                  "i32(i32,ptr)",
                  &DirectHandler<&rt_line_input_ch_err, int32_t, int32_t, ZannaString **>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_eof_ch",
                  std::nullopt,
                  "i32(i32)",
                  &DirectHandler<&rt_eof_ch, int32_t, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_lof_ch",
                  std::nullopt,
                  "i32(i32)",
                  &DirectHandler<&rt_lof_ch_i32, int32_t, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_loc_ch",
                  std::nullopt,
                  "i32(i32)",
                  &DirectHandler<&rt_loc_ch_i32, int32_t, int32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_seek_ch_err",
                  std::nullopt,
                  "i32(i32,i64)",
                  &DirectHandler<&rt_seek_ch_err, int32_t, int32_t, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_empty",
                  std::nullopt,
                  "string()",
                  &DirectHandler<&rt_str_empty, rt_string>::invoke,
                  kAlwaysLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_const_cstr",
                  std::nullopt,
                  "string(ptr)",
                  &DirectHandler<&rt_const_cstr, rt_string, const char *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_from_lit",
                  std::nullopt,
                  "string(ptr,i64)",
                  &DirectHandler<&rt_str_from_lit, rt_string, const char *, size_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // Module-level variable address helpers: return ptr from string key
    DescriptorRow{"rt_modvar_addr_i64",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_modvar_addr_i64, void *, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_modvar_addr_f64",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_modvar_addr_f64, void *, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_modvar_addr_i1",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_modvar_addr_i1, void *, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_modvar_addr_ptr",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_modvar_addr_ptr, void *, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_modvar_addr_str",
                  std::nullopt,
                  "ptr(string)",
                  &DirectHandler<&rt_modvar_addr_str, void *, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_modvar_addr_block",
                  std::nullopt,
                  "ptr(string,i64)",
                  &DirectHandler<&rt_modvar_addr_block, void *, rt_string, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
// Canonical dotted names for string retain/release, with legacy aliases gated by
// ZANNA_RUNTIME_NS_DUAL
#if ZANNA_RUNTIME_NS_DUAL
    DescriptorRow{"Zanna.String.RetainMaybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_retain_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_retain_maybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_retain_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"Zanna.String.ReleaseMaybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_release_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_str_release_maybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_release_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
#else
    DescriptorRow{"Zanna.String.RetainMaybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_retain_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"Zanna.String.ReleaseMaybe",
                  std::nullopt,
                  "void(string)",
                  &DirectHandler<&rt_str_release_maybe, void, rt_string>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
#endif
    DescriptorRow{"rt_test_bridge_mutate_str",
                  std::nullopt,
                  "void(string)",
                  &testMutateStringNoStack,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_new_i64",
                  std::nullopt,
                  "ptr(i64,i64)",
                  &DirectHandler<&rt_obj_new_i64, void *, int64_t, int64_t>::invoke,
                  featureLowering(RuntimeFeature::ObjNew),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_heap_release_deferred",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_heap_release_deferred, size_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // OOP virtual/interface dispatch helpers
    DescriptorRow{"rt_get_vfunc",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &DirectHandler<&rt_get_vfunc, void *, const rt_object *, uint32_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_itable_lookup",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &DirectHandler<&rt_itable_lookup, void **, void *, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // OOP class/interface registration and vtable lookup
    DescriptorRow{"rt_get_class_vtable",
                  std::nullopt,
                  "ptr(i64)",
                  &DirectHandler<&rt_get_class_vtable, void **, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_register_class_with_base_rs",
                  std::nullopt,
                  "void(i64,ptr,str,i64,i64)",
                  &DirectHandler<&rt_register_class_with_base_rs,
                                 void,
                                 int,
                                 void **,
                                 rt_string,
                                 int64_t,
                                 int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{
        "rt_register_class_direct",
        std::nullopt,
        "void(i64,ptr,ptr,i64)",
        &DirectHandler<&rt_register_class_direct, void, int, void **, const char *, int>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{
        "rt_register_interface_direct",
        std::nullopt,
        "void(i64,ptr,i64)",
        &DirectHandler<&rt_register_interface_direct, void, int, const char *, int>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{
        "rt_register_interface_direct_rs",
        std::nullopt,
        "void(i64,str,i64)",
        &DirectHandler<&rt_register_interface_direct_rs, void, int64_t, rt_string, int64_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{
        "rt_register_interface_impl",
        std::nullopt,
        "void(i64,i64,ptr)",
        &DirectHandler<&rt_register_interface_impl, void, long long, long long, void **>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{"rt_bind_interface",
                  std::nullopt,
                  "void(i64,i64,ptr)",
                  &DirectHandler<&rt_bind_interface, void, int, int, void **>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_typeid_of",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_typeid_of, int, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_type_is_a",
                  std::nullopt,
                  "i64(i64,i64)",
                  &DirectHandler<&rt_type_is_a, int, int, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_type_implements",
                  std::nullopt,
                  "i64(i64,i64)",
                  &DirectHandler<&rt_type_implements, int, int, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cast_as",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &DirectHandler<&rt_cast_as, void *, void *, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cast_as_iface",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &DirectHandler<&rt_cast_as_iface, void *, void *, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_cast_runtime_class",
                  std::nullopt,
                  "ptr(ptr,i64)",
                  &DirectHandler<&rt_cast_runtime_class, void *, void *, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_get_interface_impl",
                  std::nullopt,
                  "ptr(i64,i64)",
                  &DirectHandler<&rt_get_interface_impl, void **, int, int>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_retain_maybe",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_obj_retain_maybe, void, void *>::invoke,
                  featureLowering(RuntimeFeature::ObjRetainMaybe),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_retain_known",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_obj_retain_known, void, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_release_check0",
                  std::nullopt,
                  "i1(ptr)",
                  &DirectHandler<&rt_obj_release_check0, int32_t, void *>::invoke,
                  featureLowering(RuntimeFeature::ObjReleaseChk0),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_release_known_check0",
                  std::nullopt,
                  "i1(ptr)",
                  &DirectHandler<&rt_obj_release_known_check0, int32_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_free",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_obj_free, void, void *>::invoke,
                  featureLowering(RuntimeFeature::ObjFree),
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_obj_class_id",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_obj_class_id, int64_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // --- Weak Reference Support ---
    DescriptorRow{"rt_weak_store",
                  std::nullopt,
                  "void(ptr,ptr)",
                  &DirectHandler<&rt_weak_store, void, void **, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_weak_load",
                  std::nullopt,
                  "ptr(ptr)",
                  &DirectHandler<&rt_weak_load, void *, void **>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_heap_mark_disposed",
                  std::nullopt,
                  "i1(ptr)",
                  &DirectHandler<&rt_heap_mark_disposed, int32_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // --- Graphics runtime helpers (Canvas) ---
    DescriptorRow{"rt_canvas_new",
                  std::nullopt,
                  "ptr(string,i64,i64)",
                  &DirectHandler<&rt_canvas_new, void *, rt_string, int64_t, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_destroy",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_canvas_destroy, void, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_flip",
                  std::nullopt,
                  "void(ptr)",
                  &DirectHandler<&rt_canvas_flip, void, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_clear",
                  std::nullopt,
                  "void(ptr,i64)",
                  &DirectHandler<&rt_canvas_clear, void, void *, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_width",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_canvas_width, int64_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_height",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_canvas_height, int64_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_should_close",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_canvas_should_close, int64_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{
        "rt_canvas_line",
        std::nullopt,
        "void(ptr,i64,i64,i64,i64,i64)",
        &DirectHandler<&rt_canvas_line, void, void *, int64_t, int64_t, int64_t, int64_t, int64_t>::
            invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{
        "rt_canvas_box",
        std::nullopt,
        "void(ptr,i64,i64,i64,i64,i64)",
        &DirectHandler<&rt_canvas_box, void, void *, int64_t, int64_t, int64_t, int64_t, int64_t>::
            invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_frame",
                  std::nullopt,
                  "void(ptr,i64,i64,i64,i64,i64)",
                  &DirectHandler<&rt_canvas_frame,
                                 void,
                                 void *,
                                 int64_t,
                                 int64_t,
                                 int64_t,
                                 int64_t,
                                 int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{
        "rt_canvas_disc",
        std::nullopt,
        "void(ptr,i64,i64,i64,i64)",
        &DirectHandler<&rt_canvas_disc, void, void *, int64_t, int64_t, int64_t, int64_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{
        "rt_canvas_ring",
        std::nullopt,
        "void(ptr,i64,i64,i64,i64)",
        &DirectHandler<&rt_canvas_ring, void, void *, int64_t, int64_t, int64_t, int64_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_plot",
                  std::nullopt,
                  "void(ptr,i64,i64,i64)",
                  &DirectHandler<&rt_canvas_plot, void, void *, int64_t, int64_t, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_poll",
                  std::nullopt,
                  "i64(ptr)",
                  &DirectHandler<&rt_canvas_poll, int64_t, void *>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{"rt_canvas_key_held",
                  std::nullopt,
                  "i64(ptr,i64)",
                  &DirectHandler<&rt_canvas_key_held, int64_t, void *, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    // --- Graphics runtime helpers (Color) ---
    DescriptorRow{"rt_color_rgb",
                  std::nullopt,
                  "i64(i64,i64,i64)",
                  &DirectHandler<&rt_color_rgb, int64_t, int64_t, int64_t, int64_t>::invoke,
                  kManualLowering,
                  nullptr,
                  0,
                  RuntimeTrapClass::None},
    DescriptorRow{
        "rt_color_rgba",
        std::nullopt,
        "i64(i64,i64,i64,i64)",
        &DirectHandler<&rt_color_rgba, int64_t, int64_t, int64_t, int64_t, int64_t>::invoke,
        kManualLowering,
        nullptr,
        0,
        RuntimeTrapClass::None},
});

/// @brief Name-sorted index entry pointing back to a descriptor source row.
struct Descriptor {
    /// Runtime name used as the sort and binary-search key.
    std::string_view name{};
    /// Index into @ref kDescriptorRows.
    std::size_t rowIndex{};
};

/// @brief Merge two adjacent sorted descriptor ranges.
/// @tparam N Capacity of both working arrays.
/// @param arr Array updated with the merged range.
/// @param temp Scratch array used during merging.
/// @param left Inclusive beginning of the first range.
/// @param mid Exclusive end of the first and inclusive start of the second range.
/// @param right Exclusive end of the second range.
template <std::size_t N>
constexpr void mergeRanges(std::array<Descriptor, N> &arr,
                           std::array<Descriptor, N> &temp,
                           std::size_t left,
                           std::size_t mid,
                           std::size_t right) {
    std::size_t i = left, j = mid, k = left;
    while (i < mid && j < right) {
        if (arr[i].name <= arr[j].name)
            temp[k++] = arr[i++];
        else
            temp[k++] = arr[j++];
    }
    while (i < mid)
        temp[k++] = arr[i++];
    while (j < right)
        temp[k++] = arr[j++];
    for (std::size_t x = left; x < right; ++x)
        arr[x] = temp[x];
}

/// @brief Sort one descriptor-index range by symbol name at compile time.
/// @tparam N Capacity of both working arrays.
/// @param arr Array whose half-open range is sorted.
/// @param temp Scratch array forwarded to merge operations.
/// @param left Inclusive range beginning.
/// @param right Exclusive range end.
template <std::size_t N>
constexpr void mergeSort(std::array<Descriptor, N> &arr,
                         std::array<Descriptor, N> &temp,
                         std::size_t left,
                         std::size_t right) {
    if (right - left <= 1)
        return;
    std::size_t mid = left + (right - left) / 2;
    mergeSort(arr, temp, left, mid);
    mergeSort(arr, temp, mid, right);
    mergeRanges(arr, temp, left, mid, right);
}

/// @brief Build a name-sorted index over all descriptor source rows.
/// @return Compile-time array pairing every symbol name with its original row.
constexpr auto makeDescriptorIndex() {
    std::array<Descriptor, kDescriptorRows.size()> index{};
    for (std::size_t i = 0; i < index.size(); ++i)
        index[i] = Descriptor{kDescriptorRows[i].name, i};

    // Use O(n log n) merge sort instead of O(n²) selection sort
    std::array<Descriptor, kDescriptorRows.size()> temp{};
    mergeSort(index, temp, 0, index.size());

    return index;
}

constexpr std::array<Descriptor, kDescriptorRows.size()> kDescriptors = makeDescriptorIndex();

/// @brief Extract just the descriptor names for binary search comparisons.
/// @details Projects @ref kDescriptors into an array of string views so
///          @ref indexOf can use `std::lower_bound` without touching the row
///          metadata.
/// @return Name keys in descriptor-index order.
constexpr auto makeDescriptorNames() {
    std::array<std::string_view, kDescriptors.size()> names{};
    for (std::size_t i = 0; i < names.size(); ++i)
        names[i] = kDescriptors[i].name;
    return names;
}

constexpr std::array<std::string_view, kDescriptors.size()> kNames = makeDescriptorNames();

/// @brief Build a lookup table from runtime features to descriptor rows.
/// @details Initialises an array keyed by @ref RuntimeFeature enumerators and
///          records the first descriptor row that requires each feature.  Entries
///          left at -1 indicate the feature has no dedicated runtime helper.
/// @return Array mapping each feature to its descriptor-row index or -1.
constexpr auto makeFeatureIndex() {
    std::array<int, static_cast<std::size_t>(RuntimeFeature::Count)> featureIndex{};
    for (auto &entry : featureIndex)
        entry = -1;

    for (std::size_t i = 0; i < kDescriptorRows.size(); ++i) {
        const auto &row = kDescriptorRows[i];
        if (row.lowering.kind == RuntimeLoweringKind::Feature) {
            auto &slot = featureIndex[static_cast<std::size_t>(row.lowering.feature)];
            if (slot < 0)
                slot = static_cast<int>(i);
        }
    }

    return featureIndex;
}

const std::array<int, static_cast<std::size_t>(RuntimeFeature::Count)> kFeatureIndex =
    makeFeatureIndex();

/// @brief Locate the sorted descriptor index entry for a symbol name.
/// @details Performs a binary search over @ref kNames and returns the matching
///          index or -1 when the symbol is absent, enabling callers to fetch
///          either the descriptor row or parsed signature without allocating.
/// @param name Exact runtime symbol name.
/// @return Sorted descriptor index, or -1 when not registered.
static constexpr int indexOf(std::string_view name) noexcept {
    const auto it = std::lower_bound(kNames.begin(), kNames.end(), name);
    if (it == kNames.end() || *it != name)
        return -1;
    return static_cast<int>(std::distance(kNames.begin(), it));
}

/// @brief Construct a @ref RuntimeSignature from a descriptor table row.
///
/// @details Uses a pre-generated signature when available or parses the spec
///          string otherwise.  Hidden parameters and trap metadata are copied
///          from the descriptor row.
/// @param row Source metadata to translate.
/// @return Fully annotated runtime signature.
RuntimeSignature buildSignature(const DescriptorRow &row) {
    RuntimeSignature signature =
        row.signatureId ? signatureFor(*row.signatureId) : parseSignatureSpec(row.spec);
    signature.hiddenParams.assign(row.hidden, row.hidden + row.hiddenCount);
    signature.trapClass = row.trapClass;
    const auto effects = classifyHelperEffects(row.name);
    signature.nothrow = signature.nothrow || effects.nothrow;
    signature.readonly = signature.readonly || effects.readonly;
    signature.pure = signature.pure || effects.pure;
    applyOwnershipEffects(signature, row.name);
    return signature;
}

/// @brief Convert a descriptor table row into a @ref RuntimeDescriptor.
///
/// @details Populates the descriptor with the name, parsed signature, handler
///          thunk, lowering metadata, and trap classification.
/// @param row Compile-time source row.
/// @return Runtime descriptor owning the parsed signature metadata.
RuntimeDescriptor buildDescriptor(const DescriptorRow &row) {
    RuntimeDescriptor descriptor;
    descriptor.name = row.name;
    descriptor.cSymbol = row.cSymbol;
    descriptor.signatureText = row.spec;
    descriptor.signature = buildSignature(row);
    descriptor.handler = row.handler;
    descriptor.lowering = row.lowering;
    descriptor.trapClass = row.trapClass;
    descriptor.publicSurface = row.publicSurface;
    return descriptor;
}

/// @brief Convert an IL type kind to a stable diagnostic spelling.
/// @param kind Type kind to describe.
/// @return Pointer to a static lowercase name, or `"unknown"`.
const char *runtimeTypeKindName(il::core::Type::Kind kind) {
    switch (kind) {
        case Kind::Void:
            return "void";
        case Kind::I1:
            return "i1";
        case Kind::I16:
            return "i16";
        case Kind::I32:
            return "i32";
        case Kind::I64:
            return "i64";
        case Kind::F64:
            return "f64";
        case Kind::Ptr:
            return "ptr";
        case Kind::Str:
            return "str";
        case Kind::Error:
            return "error";
        case Kind::ResumeTok:
            return "resumetok";
    }
    return "unknown";
}

#ifndef NDEBUG
/// @brief Convert a signature parameter kind into a printable name.
/// @details Used exclusively in debug assertions to emit human-friendly
///          diagnostics when runtime descriptors drift from the expected
///          registry entries.
/// @param kind Debug-whitelist parameter kind.
/// @return Pointer to a static lowercase diagnostic spelling.
const char *sigParamKindName(signatures::SigParam::Kind kind) {
    using signatures::SigParam;
    switch (kind) {
        case SigParam::Kind::I1:
            return "i1";
        case SigParam::Kind::I32:
            return "i32";
        case SigParam::Kind::I64:
            return "i64";
        case SigParam::Kind::F32:
            return "f32";
        case SigParam::Kind::F64:
            return "f64";
        case SigParam::Kind::Ptr:
            return "ptr";
        case SigParam::Kind::Str:
            return "str";
    }
    return "unknown";
}

/// @brief Compare expected and actual debug signature kinds.
/// @details String and pointer kinds are accepted interchangeably because both
///          use the pointer-shaped bridge ABI.
/// @param expected Whitelist kind.
/// @param actual Kind derived from the runtime descriptor.
/// @return True when identical or a supported string/pointer pair.
bool sigParamKindsCompatible(signatures::SigParam::Kind expected,
                             signatures::SigParam::Kind actual) {
    using signatures::SigParam;
    if (expected == actual)
        return true;
    return (expected == SigParam::Kind::Str && actual == SigParam::Kind::Ptr) ||
           (expected == SigParam::Kind::Ptr && actual == SigParam::Kind::Str);
}

/// @brief Translate IL type kinds into signature parameter kinds.
/// @details Normalises several IL integer types down to the ABI shapes used in
///          runtime signatures, asserting in debug builds when encountering
///          unsupported kinds so descriptor drift is caught early.
/// @param kind IL type kind to translate.
/// @return Corresponding debug-whitelist ABI kind.
signatures::SigParam::Kind mapToSigParamKind(il::core::Type::Kind kind) {
    using signatures::SigParam;
    switch (kind) {
        case Kind::I1:
            return SigParam::Kind::I1;
        case Kind::I16:
        case Kind::I32:
            return SigParam::Kind::I32;
        case Kind::I64:
            return SigParam::Kind::I64;
        case Kind::F64:
            return SigParam::Kind::F64;
        case Kind::Ptr:
        case Kind::Str:
        case Kind::ResumeTok:
            return SigParam::Kind::Ptr;
        case Kind::Void:
            assert(false && "void type cannot appear in parameter list");
            reportInvariantViolation("void type cannot appear in parameter list");
        case Kind::Error:
            assert(false && "error type cannot appear in runtime signature");
            reportInvariantViolation("error type cannot appear in runtime signature");
    }
    assert(false && "unhandled runtime type kind");
    reportInvariantViolation("unhandled runtime type kind");
}

/// @brief Build the list of parameter kinds expected by a runtime signature.
/// @details Iterates the IL type descriptors recorded in @p signature and maps
///          them into the signature::SigParam domain using @ref mapToSigParamKind
///          so debug validation can compare against whitelisted expectations.
/// @param signature Runtime signature whose visible parameters are converted.
/// @return Parameter kinds in ABI declaration order.
std::vector<signatures::SigParam::Kind> makeParamKinds(const RuntimeSignature &signature) {
    std::vector<signatures::SigParam::Kind> kinds;
    kinds.reserve(signature.paramTypes.size());
    for (const auto &param : signature.paramTypes)
        kinds.push_back(mapToSigParamKind(param.kind));
    return kinds;
}

/// @brief Describe the return type of a runtime signature in signature kind form.
/// @details Produces either an empty vector (for void results) or a single entry
///          that mirrors the ABI-visible type, enabling uniform comparison logic
///          for both parameters and results.
/// @param signature Runtime signature whose return type is converted.
/// @return Empty vector for void or one ABI kind for a value result.
std::vector<signatures::SigParam::Kind> makeReturnKinds(const RuntimeSignature &signature) {
    std::vector<signatures::SigParam::Kind> kinds;
    if (signature.retType.kind != il::core::Type::Kind::Void)
        kinds.push_back(mapToSigParamKind(signature.retType.kind));
    return kinds;
}

/// @brief Ensure the debug-only signature registry is populated.
/// @details Registers every runtime signature group the first time validation
///          runs so later checks can compare descriptors against the whitelist
///          emitted by the dedicated signature modules.
void ensureSignatureWhitelist() {
    /// Register all debug signature groups exactly once.
    static const bool registered = [] {
        signatures::register_fileio_signatures();
        signatures::register_string_signatures();
        signatures::register_math_signatures();
        signatures::register_array_signatures();
        signatures::register_oop_signatures();
        return true;
    }();
    (void)registered;
}

/// @brief Compare generated descriptors against the debug whitelist.
/// @details Builds a map of actual descriptors, verifies that every expected
///          signature is present exactly once, and checks that parameter/return
///          kinds match.  Any mismatch triggers assertions accompanied by a
///          descriptive message to simplify debugging.
/// @param descriptors Generated runtime descriptors to compare.
void validateRuntimeDescriptors(const std::vector<RuntimeDescriptor> &descriptors) {
    ensureSignatureWhitelist();
    const auto &expected = signatures::all_signatures();

    std::unordered_map<std::string_view, const RuntimeDescriptor *> actual;
    actual.reserve(descriptors.size());
    for (const auto &descriptor : descriptors) {
        auto [it, inserted] = actual.emplace(descriptor.name, &descriptor);
        if (!inserted) {
            std::fprintf(stderr,
                         "Duplicate runtime descriptor registered for symbol '%s'.\n",
                         descriptor.name.data());
            assert(false && "duplicate runtime descriptor registration");
            continue;
        }
    }

    std::unordered_set<std::string_view> seen;
    seen.reserve(expected.size());
    for (const auto &signature : expected) {
        if (!seen.insert(signature.name).second) {
            std::fprintf(stderr,
                         "Duplicate expected runtime signature entry for '%s'.\n",
                         signature.name.c_str());
            assert(false && "duplicate expected runtime signature");
            continue;
        }

        auto it = actual.find(signature.name);
        if (it == actual.end()) {
            std::fprintf(stderr,
                         "Expected runtime signature '%s' missing from registry.\n",
                         signature.name.c_str());
            assert(false && "missing runtime signature");
            continue;
        }

        const auto &descriptor = *it->second;
        const auto params = makeParamKinds(descriptor.signature);
        const auto returns = makeReturnKinds(descriptor.signature);

        if (params.size() != signature.params.size()) {
            std::fprintf(
                stderr,
                "Runtime signature '%s' parameter count mismatch (expected %zu, got %zu).\n",
                signature.name.c_str(),
                signature.params.size(),
                params.size());
            assert(false && "runtime signature parameter count mismatch");
        } else {
            for (std::size_t index = 0; index < params.size(); ++index) {
                const auto expectedKind = signature.params[index].kind;
                const auto actualKind = params[index];
                if (!sigParamKindsCompatible(expectedKind, actualKind)) {
                    std::fprintf(stderr,
                                 "Runtime signature '%s' parameter %zu type mismatch (expected %s, "
                                 "got %s).\n",
                                 signature.name.c_str(),
                                 index,
                                 sigParamKindName(expectedKind),
                                 sigParamKindName(actualKind));
                    assert(false && "runtime signature parameter type mismatch");
                    break;
                }
            }
        }

        if (returns.size() != signature.rets.size()) {
            std::fprintf(stderr,
                         "Runtime signature '%s' return count mismatch (expected %zu, got %zu).\n",
                         signature.name.c_str(),
                         signature.rets.size(),
                         returns.size());
            assert(false && "runtime signature return count mismatch");
        } else {
            for (std::size_t index = 0; index < returns.size(); ++index) {
                const auto expectedKind = signature.rets[index].kind;
                const auto actualKind = returns[index];
                if (!sigParamKindsCompatible(expectedKind, actualKind)) {
                    std::fprintf(
                        stderr,
                        "Runtime signature '%s' return %zu type mismatch (expected %s, got %s).\n",
                        signature.name.c_str(),
                        index,
                        sigParamKindName(expectedKind),
                        sigParamKindName(actualKind));
                    assert(false && "runtime signature return type mismatch");
                    break;
                }
            }
        }

        const RuntimeSignature &actualSignature = descriptor.signature;
        const bool metadataMatches =
            actualSignature.nothrow == signature.nothrow &&
            actualSignature.readonly == signature.readonly &&
            actualSignature.pure == signature.pure &&
            actualSignature.consumedArgMask == signature.consumedArgMask &&
            actualSignature.retainedArgMask == signature.retainedArgMask &&
            actualSignature.ownedOutArgMask == signature.ownedOutArgMask &&
            actualSignature.returnsOwned == signature.returnsOwned &&
            actualSignature.mayAllocate == signature.mayAllocate &&
            actualSignature.returnsKnownObject == signature.returnsKnownObject;
        if (!metadataMatches) {
            std::fprintf(stderr,
                         "Runtime signature '%s' effect/ownership metadata mismatch.\n",
                         signature.name.c_str());
            assert(false && "runtime signature metadata mismatch");
        }
    }
}
#endif

} // namespace

/// @brief Retrieve the immutable list of runtime descriptors.
///
/// @details Lazily constructs the registry from @ref kDescriptorRows on first
///          use and caches it for subsequent lookups.
/// @return Stable process-lifetime descriptor vector in source-row order.
const std::vector<RuntimeDescriptor> &runtimeRegistry() {
    /// Materialize descriptors and install VM-specific handler overrides.
    static const std::vector<RuntimeDescriptor> registry = [] {
        std::vector<RuntimeDescriptor> entries;
        entries.reserve(kDescriptorRows.size());
        for (const auto &row : kDescriptorRows)
            entries.push_back(buildDescriptor(row));

        // Patch VM-specific handler overrides.  Generated descriptors use
        // DirectHandler<> wrappers derived from C function signatures.  Some
        // entries need hand-written adapters instead because the C ABI type
        // doesn't match the IL calling convention used by the VM:
        //
        //  - Zanna.System.Environment.IsNative: C function always returns 1
        //    (native).  The VM must return 0.
        //
        //  - Zanna.Math.Pow: C function takes a hidden bool* parameter the VM
        //    doesn't pass.  The adapter manages it internally.
        //
        //  - Zanna.Core.Diagnostics.Trap: C function takes const char* but the
        //    IL type is str (rt_string).  trapFromRuntimeString correctly
        //    extracts the C string from the rt_string struct.
        for (auto &entry : entries) {
            if (entry.name == "Zanna.System.Environment.IsNative") {
                entry.handler = &vm_env_is_native;
            } else if (entry.name == "Zanna.Math.Pow") {
                entry.handler = &vmInvokeRtPow;
                entry.trapClass = RuntimeTrapClass::PowDomainOverflow;
            } else if (entry.name == "Zanna.Core.Diagnostics.Trap") {
                entry.handler = &trapFromRuntimeString;
            }
        }

#ifndef NDEBUG
        validateRuntimeDescriptors(entries);
#endif
        return entries;
    }();
    return registry;
}

/// @brief Find a runtime descriptor by its exported name.
///
/// @details Performs a binary search over the compile-time sorted descriptor
///          index, avoiding dynamic map construction on lookup.
/// @param name Exact exported runtime symbol.
/// @return Borrowed process-lifetime descriptor pointer, or null.
const RuntimeDescriptor *findRuntimeDescriptor(std::string_view name) {
    const int idx = indexOf(name);
    if (idx < 0)
        return nullptr;
    const auto rowIndex = kDescriptors[static_cast<std::size_t>(idx)].rowIndex;
    const auto &registry = runtimeRegistry();
    return &registry[rowIndex];
}

/// @brief Locate the descriptor that provides a particular runtime feature.
///
/// @details Uses a precomputed table mapping features to descriptor indices for
///          constant-time lookup without allocating supporting data structures.
/// @param feature Lowering feature to resolve.
/// @return Borrowed process-lifetime descriptor pointer, or null when the
///         feature has no registered provider.
const RuntimeDescriptor *findRuntimeDescriptor(RuntimeFeature feature) {
    const auto index = kFeatureIndex[static_cast<std::size_t>(feature)];
    if (index < 0)
        return nullptr;
    return &runtimeRegistry()[static_cast<std::size_t>(index)];
}

/// @brief Provide a map from runtime names to parsed signatures.
///
/// @details Materialises an unordered_map on first access by iterating over the
///          registry, enabling quick signature lookups by string name.
/// @return Stable process-lifetime mapping from names to signature copies.
const std::unordered_map<std::string_view, RuntimeSignature> &runtimeSignatures() {
    /// Populate the compatibility lookup map from the canonical registry.
    static const std::unordered_map<std::string_view, RuntimeSignature> table = [] {
        std::unordered_map<std::string_view, RuntimeSignature> map;
        map.reserve(runtimeRegistry().size());
        for (const auto &entry : runtimeRegistry())
            map.emplace(entry.name, entry.signature);
        return map;
    }();
    return table;
}

/// @brief Look up the generated signature enumerator for a runtime symbol name.
///
/// @param name Runtime symbol to resolve.
/// @return Enumerator when found or std::nullopt otherwise.
std::optional<RtSig> findRuntimeSignatureId(std::string_view name) {
    const int idx = indexOf(name);
    if (idx < 0)
        return std::nullopt;
    const auto &row = kDescriptorRows[kDescriptors[static_cast<std::size_t>(idx)].rowIndex];
    if (!row.signatureId)
        return std::nullopt;
    return row.signatureId;
}

/// @brief Retrieve a signature descriptor by enumerator.
///
/// @param sig Enumerated signature identifier.
/// @return Pointer to the signature when valid, otherwise nullptr.
const RuntimeSignature *findRuntimeSignature(RtSig sig) {
    if (!isValid(sig))
        return nullptr;
    return &signatureFor(sig);
}

/// @brief Retrieve a signature descriptor by runtime symbol name.
///
/// @details First attempts to resolve the generated enumerator; when absent it
///          falls back to descriptors registered in @ref runtimeRegistry.
///
/// @param name Runtime symbol to search for.
/// @return Pointer to the signature when found, otherwise nullptr.
const RuntimeSignature *findRuntimeSignature(std::string_view name) {
    if (auto id = findRuntimeSignatureId(name))
        return findRuntimeSignature(*id);
    if (const auto *desc = findRuntimeDescriptor(name))
        return &desc->signature;
    return nullptr;
}

/// @brief Check whether a callee uses C-style variadic arguments.
///
/// @details Consults the runtime registry first; falls back to a hardcoded list
///          of known C library functions that are not registered but require
///          vararg calling convention (e.g., rt_snprintf, rt_sb_printf).
///
/// @param name Symbol name of the callee to query.
/// @return True when the callee is known to be variadic.
bool isVarArgCallee(std::string_view name) {
    // First check the runtime registry for registered signatures with isVarArg set.
    if (const auto *sig = findRuntimeSignature(name)) {
        if (sig->isVarArg)
            return true;
    }

    // Fallback: known C library vararg functions used by the runtime but not
    // registered in the signature registry. This list is maintained here to
    // centralise vararg metadata rather than scattering it across backends.
    static constexpr std::string_view kKnownVarArgCallees[] = {
        "rt_snprintf",
        "rt_sb_printf",
    };

    for (const auto &known : kKnownVarArgCallees) {
        if (name == known)
            return true;
    }

    return false;
}

/// @brief Lightweight runtime descriptor self-check for both debug and release builds.
///
/// @details Performs essential sanity checks that are cheap enough to run in release:
///          1. No duplicate descriptor names in the registry
///          2. Each descriptor's parsed signature has a consistent parameter count
///
///          In debug builds, additionally runs the full whitelist validation.
///          This function is idempotent and thread-safe via static initialization.
///
/// @return True if all checks pass. In release builds, logs errors and returns false
///         on failure. In debug builds, also asserts.
bool selfCheckRuntimeDescriptors() {
    // Run once per process via static init guard
    /// Perform the cached validation pass used by all callers.
    static const bool result = []() -> bool {
        const auto &descriptors = runtimeRegistry();
        bool valid = true;

        // Check 1: No duplicate descriptor names
        // Use a simple O(n^2) check to avoid heap allocation for hash map
        // This runs once at startup so the cost is acceptable
        for (std::size_t i = 0; i < descriptors.size(); ++i) {
            for (std::size_t j = i + 1; j < descriptors.size(); ++j) {
                if (descriptors[i].name == descriptors[j].name) {
                    std::fprintf(stderr,
                                 "[FATAL] Runtime descriptor duplicate: '%.*s' at indices %zu and "
                                 "%zu\n",
                                 static_cast<int>(descriptors[i].name.size()),
                                 descriptors[i].name.data(),
                                 i,
                                 j);
#ifndef NDEBUG
                    assert(false && "duplicate runtime descriptor name");
#endif
                    valid = false;
                }
            }
        }

        // Check 2: Parameter count consistency
        // For each descriptor, verify the signature is parseable, has a handler,
        // and does not contain IL-only error/void parameter shapes.
        constexpr std::size_t kMaxReasonableParams = 16;
        for (std::size_t i = 0; i < descriptors.size(); ++i) {
            const auto &desc = descriptors[i];
            if (!desc.handler) {
                std::fprintf(stderr,
                             "[FATAL] Runtime descriptor '%.*s' has no handler\n",
                             static_cast<int>(desc.name.size()),
                             desc.name.data());
#ifndef NDEBUG
                assert(false && "runtime descriptor missing handler");
#endif
                valid = false;
            }
            if (!desc.signature.valid) {
                std::fprintf(stderr,
                             "[FATAL] Runtime descriptor '%.*s' has an invalid signature\n",
                             static_cast<int>(desc.name.size()),
                             desc.name.data());
#ifndef NDEBUG
                assert(false && "runtime descriptor has invalid signature");
#endif
                valid = false;
            }
            if (desc.signature.retType.kind == il::core::Type::Kind::Error) {
                std::fprintf(stderr,
                             "[FATAL] Runtime descriptor '%.*s' returns unsupported error type\n",
                             static_cast<int>(desc.name.size()),
                             desc.name.data());
#ifndef NDEBUG
                assert(false && "runtime descriptor returns error type");
#endif
                valid = false;
            }
            const std::size_t paramCount = desc.signature.paramTypes.size();
            if (paramCount > kMaxReasonableParams) {
                std::fprintf(stderr,
                             "[FATAL] Runtime descriptor '%.*s' has %zu parameters (max %zu)\n",
                             static_cast<int>(desc.name.size()),
                             desc.name.data(),
                             paramCount,
                             kMaxReasonableParams);
#ifndef NDEBUG
                assert(false && "runtime descriptor has too many parameters");
#endif
                valid = false;
            }
            for (std::size_t paramIndex = 0; paramIndex < paramCount; ++paramIndex) {
                const auto kind = desc.signature.paramTypes[paramIndex].kind;
                if (kind != il::core::Type::Kind::Void && kind != il::core::Type::Kind::Error)
                    continue;
                std::fprintf(stderr,
                             "[FATAL] Runtime descriptor '%.*s' parameter %zu has unsupported %s "
                             "type\n",
                             static_cast<int>(desc.name.size()),
                             desc.name.data(),
                             paramIndex,
                             runtimeTypeKindName(kind));
#ifndef NDEBUG
                assert(false && "runtime descriptor has unsupported parameter type");
#endif
                valid = false;
            }
        }

        // In debug builds, the full whitelist validation already ran during
        // runtimeRegistry() initialization. We just report the combined result.
        // The heavy validation (ensureSignatureWhitelist, full type matching)
        // only runs in debug builds via validateRuntimeDescriptors().

        return valid;
    }();

    return result;
}

// =============================================================================
// Invariant Violation Mode Configuration
// =============================================================================

namespace {
/// @brief Current mode for handling invariant violations.
static std::atomic<InvariantViolationMode> g_violationMode{InvariantViolationMode::Abort};

/// @brief Registered trap handler for invariant violations.
static std::atomic<InvariantTrapHandler> g_trapHandler{nullptr};
} // namespace

/// @brief Select how future invariant violations are reported.
/// @param mode New process-wide reporting mode.
void setInvariantViolationMode(InvariantViolationMode mode) {
    g_violationMode.store(mode, std::memory_order_relaxed);
}

/// @brief Read the process-wide invariant violation mode.
/// @return Currently configured reporting mode.
InvariantViolationMode getInvariantViolationMode() {
    return g_violationMode.load(std::memory_order_relaxed);
}

/// @brief Install or remove the VM invariant-trap callback.
/// @param handler Callback pointer, or null to unregister.
void setInvariantTrapHandler(InvariantTrapHandler handler) {
    g_trapHandler.store(handler, std::memory_order_relaxed);
}

/// @brief Read the currently installed invariant-trap callback.
/// @return Callback pointer, or null when none is installed.
InvariantTrapHandler getInvariantTrapHandler() {
    return g_trapHandler.load(std::memory_order_relaxed);
}

/// @brief Report an unrecoverable runtime signature invariant violation.
/// @details Trap mode first offers the message to the registered VM callback;
///          returning from that callback still falls back to logging and abort.
/// @param message Null-terminated diagnostic message.
/// @return This function never returns.
[[noreturn]] void reportInvariantViolation(const char *message) {
    // In Trap mode with a registered handler, attempt to route through the VM.
    if (g_violationMode.load(std::memory_order_relaxed) == InvariantViolationMode::Trap &&
        g_trapHandler.load(std::memory_order_relaxed) != nullptr) {
        // Handler returns true if the trap was processed (e.g., caught by an exception
        // handler in the VM). In this case, the handler should not return at all.
        // If it returns false, we fall through to abort.
        auto handler = g_trapHandler.load(std::memory_order_relaxed);
        if (handler(message)) {
            // Handler claimed success but returned - this is a logic error.
            // The trap mechanism should not allow normal return.
            std::fprintf(stderr,
                         "[FATAL] Invariant trap handler returned after claiming success.\n");
        }
        // Fall through to abort if handler returned false or unexpectedly.
    }

    // Default/fallback behavior: log and abort.
    std::fprintf(stderr, "[FATAL] Runtime signature invariant violation: %s\n", message);
    std::abort();
}

} // namespace il::runtime
