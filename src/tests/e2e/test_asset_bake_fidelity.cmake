#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/e2e/test_asset_bake_fidelity.cmake
# Purpose: Verify the machine-readable and human-readable `zanna asset bake`
#   fidelity diagnostics against full VSCN v5 SceneAsset round trips.
#
# Key invariants:
#   - JSON mode emits the stable v1 report schema on stdout.
#   - Default mode warns on stderr for every detected dropped resource class.
#
# Ownership/Lifetime: CTest owns the generated VSCN files in the build tree.
#
# Links: src/tools/zanna/cmd_asset.cpp, docs/tools/cli.md
#
#===----------------------------------------------------------------------===#

if (NOT DEFINED ZANNA_EXE OR NOT DEFINED INPUT OR NOT DEFINED SOURCE_INPUT OR
    NOT DEFINED DECODED_INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR
            "ZANNA_EXE, INPUT, SOURCE_INPUT, DECODED_INPUT, and OUTPUT are required")
endif ()

execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}" --json
        RESULT_VARIABLE bake_result
        OUTPUT_VARIABLE bake_stdout
        ERROR_VARIABLE bake_stderr)
if (NOT bake_result EQUAL 0)
    message(FATAL_ERROR "JSON bake failed (${bake_result}): ${bake_stderr}")
endif ()

# LOD constraints are independent of the base-mesh simplification pass.
foreach(invalid_levels IN ITEMS "2junk" "1.5" "9" "-1" "")
    execute_process(
            COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}.invalid-lod"
                    --lods "${invalid_levels}" --lod-lock-seams
            RESULT_VARIABLE invalid_result OUTPUT_VARIABLE invalid_stdout
            ERROR_VARIABLE invalid_stderr)
    if (NOT invalid_result EQUAL 1 OR NOT invalid_stderr MATCHES "--lods expects 0..8")
        message(FATAL_ERROR "invalid LOD count accepted: '${invalid_levels}': ${invalid_stderr}")
    endif ()
endforeach()
foreach(invalid_error IN ITEMS "0" "1" "nan" "inf" "0.001junk" "")
    execute_process(
            COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}.invalid-lod"
                    --lods 2 --lod-max-error "${invalid_error}"
            RESULT_VARIABLE invalid_result OUTPUT_VARIABLE invalid_stdout
            ERROR_VARIABLE invalid_stderr)
    if (NOT invalid_result EQUAL 1 OR NOT invalid_stderr MATCHES "--lod-max-error expects")
        message(FATAL_ERROR "invalid LOD error accepted: '${invalid_error}': ${invalid_stderr}")
    endif ()
endforeach()
execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}.invalid-lod"
                --lod-lock-seams
        RESULT_VARIABLE invalid_result OUTPUT_VARIABLE invalid_stdout
        ERROR_VARIABLE invalid_stderr)
if (NOT invalid_result EQUAL 1 OR NOT invalid_stderr MATCHES "require --lods > 0")
    message(FATAL_ERROR "LOD constraints without levels were accepted: ${invalid_stderr}")
endif ()
execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}.constrained-lod.vscn"
                --lods 2 --lod-lock-seams --lod-max-error 0.001 --json
        RESULT_VARIABLE constrained_result OUTPUT_VARIABLE constrained_stdout
        ERROR_VARIABLE constrained_stderr)
if (NOT constrained_result EQUAL 0 OR constrained_stderr)
    message(FATAL_ERROR "constrained LOD bake failed: ${constrained_stderr}")
endif ()
string(JSON constrained_status GET "${constrained_stdout}" status)
if (NOT constrained_status STREQUAL "ok")
    message(FATAL_ERROR "constrained LOD bake report is not successful: ${constrained_stdout}")
endif ()

string(JSON schema ERROR_VARIABLE json_error GET "${bake_stdout}" schema)
if (json_error OR NOT schema STREQUAL "zanna.asset-bake-report/v1")
    message(FATAL_ERROR "invalid bake report schema: ${json_error}; ${bake_stdout}")
endif ()
string(JSON status GET "${bake_stdout}" status)
string(JSON lossy GET "${bake_stdout}" lossy)
string(JSON source_scenes GET "${bake_stdout}" source scenes)
string(JSON baked_scenes GET "${bake_stdout}" baked scenes)
string(JSON source_cameras GET "${bake_stdout}" source cameras)
string(JSON source_node_animations GET "${bake_stdout}" source nodeAnimations)
string(JSON source_morph_targets GET "${bake_stdout}" source morphTargets)
string(JSON baked_morph_targets GET "${bake_stdout}" baked morphTargets)
string(JSON source_morph_shapes GET "${bake_stdout}" source morphShapes)
string(JSON baked_morph_shapes GET "${bake_stdout}" baked morphShapes)
string(JSON source_variants GET "${bake_stdout}" source variants)
string(JSON loss_count LENGTH "${bake_stdout}" losses)
if (NOT status STREQUAL "ok" OR lossy OR
    NOT source_scenes EQUAL 2 OR NOT baked_scenes EQUAL 2 OR
    NOT source_cameras EQUAL 1 OR NOT source_node_animations EQUAL 1 OR
    source_morph_targets LESS_EQUAL 0 OR
    NOT source_morph_targets EQUAL baked_morph_targets OR
    source_morph_shapes LESS_EQUAL 0 OR
    NOT source_morph_shapes EQUAL baked_morph_shapes OR
    NOT source_variants EQUAL 2 OR NOT loss_count EQUAL 0)
    message(FATAL_ERROR "unexpected bake fidelity report: ${bake_stdout}")
endif ()
if (bake_stderr)
    message(FATAL_ERROR "JSON bake wrote unexpected stderr: ${bake_stderr}")
endif ()

execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${INPUT}" "${OUTPUT}.human.vscn"
        RESULT_VARIABLE human_result
        OUTPUT_VARIABLE human_stdout
        ERROR_VARIABLE human_stderr)
if (NOT human_result EQUAL 0 OR NOT human_stdout MATCHES "baked ")
    message(FATAL_ERROR "human bake failed (${human_result}): ${human_stdout}; ${human_stderr}")
endif ()
if (human_stderr)
    message(FATAL_ERROR "lossless human bake wrote unexpected warnings: ${human_stderr}")
endif ()

execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${SOURCE_INPUT}" "${OUTPUT}.source.vscn" --json
        RESULT_VARIABLE source_result
        OUTPUT_VARIABLE source_stdout
        ERROR_VARIABLE source_stderr)
if (NOT source_result EQUAL 0 OR source_stderr)
    message(FATAL_ERROR
            "source-container fidelity bake failed (${source_result}): ${source_stderr}")
endif ()
string(JSON source_lossy GET "${source_stdout}" lossy)
string(JSON preserved_source GET "${source_stdout}"
        textureFidelity summary preserved-source)
string(JSON source_changed GET "${source_stdout}"
        textureFidelity summary changed-after-import)
string(JSON source_texture_losses GET "${source_stdout}" textureFidelity summary losses)
string(JSON source_entry_state GET "${source_stdout}" textureFidelity entries 0 state)
string(JSON source_entry_container GET "${source_stdout}" textureFidelity entries 0 sourceContainer)
if (source_lossy OR NOT preserved_source EQUAL 1 OR NOT source_changed EQUAL 0 OR
    NOT source_texture_losses EQUAL 0 OR NOT source_entry_state STREQUAL "preserved-source" OR
    NOT source_entry_container STREQUAL "ktx2")
    message(FATAL_ERROR "unexpected exact-source texture fidelity report: ${source_stdout}")
endif ()

execute_process(
        COMMAND "${ZANNA_EXE}" asset bake "${DECODED_INPUT}" "${OUTPUT}.decoded.vscn" --json
        RESULT_VARIABLE decoded_result
        OUTPUT_VARIABLE decoded_stdout
        ERROR_VARIABLE decoded_stderr)
if (NOT decoded_result EQUAL 0 OR decoded_stderr)
    message(FATAL_ERROR
            "decoded-texture fidelity bake failed (${decoded_result}): ${decoded_stderr}")
endif ()
string(JSON decoded_lossy GET "${decoded_stdout}" lossy)
string(JSON preserved_decoded GET "${decoded_stdout}"
        textureFidelity summary preserved-decoded)
string(JSON decoded_changed GET "${decoded_stdout}"
        textureFidelity summary changed-after-import)
string(JSON decoded_texture_losses GET "${decoded_stdout}" textureFidelity summary losses)
string(JSON decoded_entry_state GET "${decoded_stdout}" textureFidelity entries 0 state)
if (decoded_lossy OR NOT preserved_decoded EQUAL 1 OR NOT decoded_changed EQUAL 0 OR
    NOT decoded_texture_losses EQUAL 0 OR
    NOT decoded_entry_state STREQUAL "preserved-decoded")
    message(FATAL_ERROR "unexpected decoded texture fidelity report: ${decoded_stdout}")
endif ()
