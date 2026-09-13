//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime_classes/TestMethodIndex.cpp
// Purpose: Validate BASIC runtime class method lookup and signature metadata.
// Key invariants:
//   - Indexed targets and receiver flags match the live runtime registry.
//   - Object results preserve their concrete runtime class names.
// Ownership/Lifetime:
//   - Tests borrow process-global registry entries for each assertion.
// Links: src/frontends/basic/RuntimeMethodIndex.cpp, src/il/runtime/runtime.def
//
//===----------------------------------------------------------------------===//
///
/// @file TestMethodIndex.cpp
/// @brief Unit tests for RuntimeMethodIndex method lookup functionality.
///
/// @details This test file verifies that the RuntimeMethodIndex correctly
/// looks up runtime class methods and returns accurate signature information.
/// It tests the integration between the BASIC frontend's method index and
/// the IL-layer RuntimeRegistry.
///
/// ## Test Coverage
///
/// ### String.Substring Tests
///
/// Verifies that String.Substring(start, length) is correctly resolved with:
/// - Target: "Zanna.String.Substring"
/// - Return type: String
/// - Parameter types: [Int, Int]
///
/// ### Object Method Tests
///
/// Verifies standard Object methods from the runtime library:
///
/// | Method         | Arity | Expected Target                 | Return |
/// |----------------|-------|---------------------------------|--------|
/// | Equals(obj)    | 1     | Zanna.Core.Object.Equals        | Bool   |
/// | HashCode()     | 0     | Zanna.Core.Object.HashCode      | Int    |
/// | ToString()     | 0     | Zanna.Core.Object.ToString      | String |
/// | RefEquals      | 2     | Zanna.Core.Object.RefEquals     | Bool   |
///
/// ## RuntimeMethodIndex Architecture
///
/// The RuntimeMethodIndex now delegates to the unified RuntimeRegistry:
///
/// ```
/// runtimeMethodIndex().find(class, method, arity)
///         │
///         ▼
/// RuntimeRegistry::instance().findMethod(class, method, arity)
///         │
///         ▼
/// ParsedMethod (IL types)
///         │
///         ▼
/// toBasicType() conversion
///         │
///         ▼
/// RuntimeMethodInfo (BASIC types)
/// ```
///
/// This ensures signature information is consistent across all frontends.
///
/// @see RuntimeMethodIndex - BASIC frontend method lookup interface
/// @see RuntimeRegistry - Unified signature registry
/// @see runtime.def - Source definitions for runtime methods
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "tests/TestHarness.hpp"

using il::frontends::basic::BasicType;
using il::frontends::basic::runtimeMethodIndex;

/// @brief Test String.Substring lookup returns correct target and types.
///
/// @details Verifies that looking up String.Substring with arity 2 returns:
/// - Correct extern target name for IL code generation
/// - Correct return type (String)
/// - Correct parameter types (Int, Int)
///
TEST(RuntimeMethodIndexBasic, StringSubstringTarget) {
    // Seed the index (delegates to RuntimeRegistry internally)
    runtimeMethodIndex().seed();

    // Look up String.Substring(start: Int, length: Int) -> String
    auto info = runtimeMethodIndex().find("Zanna.String", "Substring", 2);
    ASSERT_TRUE(info.has_value());

    // Verify extern target name
    EXPECT_EQ(info->target, std::string("Zanna.String.Substring"));
    EXPECT_TRUE(info->hasReceiver);

    // Verify return type is String
    EXPECT_EQ(info->ret, BasicType::String);

    // Verify parameter types are [Int, Int]
    ASSERT_EQ(info->args.size(), 2u);
    EXPECT_EQ(info->args[0], BasicType::Int);
    EXPECT_EQ(info->args[1], BasicType::Int);
}

/// @brief Test Object class method lookups.
///
/// @details Verifies that standard Object methods are correctly resolved.
/// Also tests that RefEquals is exposed through the class method surface.
///
TEST(RuntimeMethodIndexBasic, ObjectMethodsTargets) {
    // Seed the index (delegates to RuntimeRegistry internally)
    runtimeMethodIndex().seed();

    // Test Object.Equals(other: Object) -> Boolean
    auto eq = runtimeMethodIndex().find("Zanna.Core.Object", "Equals", 1);
    ASSERT_TRUE(eq.has_value());
    EXPECT_EQ(eq->target, std::string("Zanna.Core.Object.Equals"));
    EXPECT_TRUE(eq->hasReceiver);

    // Test Object.HashCode() -> Int
    auto hc = runtimeMethodIndex().find("Zanna.Core.Object", "HashCode", 0);
    ASSERT_TRUE(hc.has_value());
    EXPECT_EQ(hc->target, std::string("Zanna.Core.Object.HashCode"));
    EXPECT_TRUE(hc->hasReceiver);

    // Test Object.ToString() -> String
    auto ts = runtimeMethodIndex().find("Zanna.Core.Object", "ToString", 0);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(ts->target, std::string("Zanna.Core.Object.ToString"));
    EXPECT_TRUE(ts->hasReceiver);

    auto re = runtimeMethodIndex().find("Zanna.Core.Object", "RefEquals", 2);
    ASSERT_TRUE(re.has_value());
    EXPECT_EQ(re->target, std::string("Zanna.Core.Object.RefEquals"));
    EXPECT_EQ(re->ret, BasicType::Bool);
    EXPECT_FALSE(re->hasReceiver);

    auto isNullInstance = runtimeMethodIndex().find("Zanna.Core.Object", "IsNull", 0);
    ASSERT_TRUE(isNullInstance.has_value());
    EXPECT_EQ(isNullInstance->target, std::string("Zanna.Core.Object.IsNull"));
    EXPECT_TRUE(isNullInstance->hasReceiver);

    auto isNullStatic = runtimeMethodIndex().find("Zanna.Core.Object", "IsNull", 1);
    ASSERT_TRUE(isNullStatic.has_value());
    EXPECT_EQ(isNullStatic->target, std::string("Zanna.Core.Object.IsNull"));
    EXPECT_FALSE(isNullStatic->hasReceiver);
}

TEST(RuntimeMethodIndexBasic, MemoryAndParseSurfaceMethods) {
    runtimeMethodIndex().seed();

    // Refcount mutation now lives only under Zanna.Runtime.Unsafe; the
    // Zanna.Memory mirrors were removed in the pre-alpha sweep.
    auto retain = runtimeMethodIndex().find("Zanna.Runtime.Unsafe", "Retain", 1);
    ASSERT_TRUE(retain.has_value());
    EXPECT_EQ(retain->target, std::string("Zanna.Runtime.Unsafe.Retain"));
    auto oldRetain = runtimeMethodIndex().find("Zanna.Memory", "Retain", 1);
    EXPECT_FALSE(oldRetain.has_value());

    auto releaseStr = runtimeMethodIndex().find("Zanna.Runtime.Unsafe", "ReleaseStr", 1);
    ASSERT_TRUE(releaseStr.has_value());
    EXPECT_EQ(releaseStr->target, std::string("Zanna.Runtime.Unsafe.ReleaseStr"));
    EXPECT_EQ(releaseStr->ret, BasicType::Int);

    auto oldDoubleOpt = runtimeMethodIndex().find("Zanna.Core.Parse", "DoubleOption", 1);
    EXPECT_FALSE(oldDoubleOpt.has_value());
    auto oldIntOpt = runtimeMethodIndex().find("Zanna.Core.Parse", "Int64Option", 1);
    EXPECT_FALSE(oldIntOpt.has_value());

    auto tryNum = runtimeMethodIndex().find("Zanna.Core.Parse", "TryDouble", 1);
    ASSERT_TRUE(tryNum.has_value());
    EXPECT_EQ(tryNum->target, std::string("Zanna.Core.Parse.TryDouble"));
    EXPECT_EQ(tryNum->ret, BasicType::Object);
    EXPECT_EQ(tryNum->returnClassQName, std::string("Zanna.Option"));
    // The legacy TryNum spelling is gone.
    EXPECT_FALSE(runtimeMethodIndex().find("Zanna.Core.Parse", "TryNum", 1).has_value());

    auto tryInt = runtimeMethodIndex().find("Zanna.Core.Parse", "TryInt", 1);
    ASSERT_TRUE(tryInt.has_value());
    EXPECT_EQ(tryInt->target, std::string("Zanna.Core.Parse.TryInt"));
    EXPECT_EQ(tryInt->ret, BasicType::Object);
    EXPECT_EQ(tryInt->returnClassQName, std::string("Zanna.Option"));

    auto weakNew = runtimeMethodIndex().find("Zanna.Memory.WeakRef", "New", 1);
    ASSERT_TRUE(weakNew.has_value());
    EXPECT_EQ(weakNew->target, std::string("Zanna.Memory.WeakRef.New"));
    EXPECT_EQ(weakNew->ret, BasicType::Object);
    EXPECT_EQ(weakNew->returnClassQName, std::string("Zanna.Memory.WeakRef"));

    auto weakGet = runtimeMethodIndex().find("Zanna.Memory.WeakRef", "Get", 1);
    ASSERT_TRUE(weakGet.has_value());
    EXPECT_EQ(weakGet->target, std::string("Zanna.Memory.WeakRef.Get"));
    EXPECT_FALSE(weakGet->hasReceiver);

    auto weakGetInstance = runtimeMethodIndex().find("Zanna.Memory.WeakRef", "Get", 0);
    ASSERT_TRUE(weakGetInstance.has_value());
    EXPECT_EQ(weakGetInstance->target, std::string("Zanna.Memory.WeakRef.Get"));
    EXPECT_TRUE(weakGetInstance->hasReceiver);

    auto weakResetInstance = runtimeMethodIndex().find("Zanna.Memory.WeakRef", "Reset", 1);
    ASSERT_TRUE(weakResetInstance.has_value());
    EXPECT_EQ(weakResetInstance->target, std::string("Zanna.Memory.WeakRef.Reset"));
    EXPECT_TRUE(weakResetInstance->hasReceiver);

    auto msgCallback = runtimeMethodIndex().find("Zanna.Core.MessageBus", "Callback", 1);
    ASSERT_TRUE(msgCallback.has_value());
    EXPECT_EQ(msgCallback->target, std::string("Zanna.Core.MessageBus.Callback"));
    EXPECT_FALSE(msgCallback->hasReceiver);

    auto msgSubscribe = runtimeMethodIndex().find("Zanna.Core.MessageBus", "Subscribe", 2);
    ASSERT_TRUE(msgSubscribe.has_value());
    EXPECT_EQ(msgSubscribe->target, std::string("Zanna.Core.MessageBus.Subscribe"));
    EXPECT_TRUE(msgSubscribe->hasReceiver);

    // Zanna.Core.ValueType was removed; value-type registration lives only
    // under Zanna.Runtime.Unsafe.
    EXPECT_FALSE(runtimeMethodIndex().find("Zanna.Core.ValueType", "AddField", 3).has_value());
}

TEST(RuntimeMethodIndexBasic, CollectionReturningMethodsPreserveConcreteClass) {
    runtimeMethodIndex().seed();

    auto patternFindAll = runtimeMethodIndex().find("Zanna.Text.Pattern", "FindAll", 2);
    ASSERT_TRUE(patternFindAll.has_value());
    EXPECT_EQ(patternFindAll->ret, BasicType::Object);
    EXPECT_EQ(patternFindAll->returnClassQName, std::string("Zanna.Collections.Seq"));

    auto findAll = runtimeMethodIndex().find("Zanna.Text.CompiledPattern", "FindAll", 1);
    ASSERT_TRUE(findAll.has_value());
    EXPECT_EQ(findAll->ret, BasicType::Object);
    EXPECT_EQ(findAll->returnClassQName, std::string("Zanna.Collections.Seq"));

    auto wrapLines = runtimeMethodIndex().find("Zanna.Text.TextWrapper", "WrapLines", 2);
    ASSERT_TRUE(wrapLines.has_value());
    EXPECT_EQ(wrapLines->ret, BasicType::Object);
    EXPECT_EQ(wrapLines->returnClassQName, std::string("Zanna.Collections.Seq"));

    auto templateKeys = runtimeMethodIndex().find("Zanna.Text.Template", "Keys", 1);
    ASSERT_TRUE(templateKeys.has_value());
    EXPECT_EQ(templateKeys->ret, BasicType::Object);
    // Template.Keys returns a deduplicated StringSet (VDOC-044).
    EXPECT_EQ(templateKeys->returnClassQName, std::string("Zanna.Collections.StringSet"));

    auto keys = runtimeMethodIndex().find("Zanna.Collections.DefaultMap", "Keys", 0);
    ASSERT_TRUE(keys.has_value());
    EXPECT_EQ(keys->ret, BasicType::Object);
    EXPECT_EQ(keys->returnClassQName, std::string("Zanna.Collections.Seq"));

    auto toList = runtimeMethodIndex().find("Zanna.Collections.Seq", "ToList", 0);
    ASSERT_TRUE(toList.has_value());
    EXPECT_EQ(toList->ret, BasicType::Object);
    EXPECT_EQ(toList->returnClassQName, std::string("Zanna.Collections.List"));

    auto lazyToSeqN = runtimeMethodIndex().find("Zanna.Functional.LazySeq", "ToSeq", 1);
    ASSERT_TRUE(lazyToSeqN.has_value());
    EXPECT_EQ(lazyToSeqN->ret, BasicType::Object);
    EXPECT_EQ(lazyToSeqN->returnClassQName, std::string("Zanna.Collections.Seq"));
}

TEST(RuntimeMethodIndexBasic, SoundFactoriesPreserveConcreteReturnClass) {
    runtimeMethodIndex().seed();

    auto tone = runtimeMethodIndex().find("Zanna.Audio.Synth", "Tone", 3);
    ASSERT_TRUE(tone.has_value());
    EXPECT_EQ(tone->target, std::string("Zanna.Audio.Synth.Tone"));
    EXPECT_EQ(tone->ret, BasicType::Object);
    EXPECT_EQ(tone->returnClassQName, std::string("Zanna.Audio.Sound"));

    auto sweep = runtimeMethodIndex().find("Zanna.Audio.Synth", "Sweep", 4);
    ASSERT_TRUE(sweep.has_value());
    EXPECT_EQ(sweep->ret, BasicType::Object);
    EXPECT_EQ(sweep->returnClassQName, std::string("Zanna.Audio.Sound"));

    auto noise = runtimeMethodIndex().find("Zanna.Audio.Synth", "Noise", 2);
    ASSERT_TRUE(noise.has_value());
    EXPECT_EQ(noise->ret, BasicType::Object);
    EXPECT_EQ(noise->returnClassQName, std::string("Zanna.Audio.Sound"));

    auto sfx = runtimeMethodIndex().find("Zanna.Audio.Synth", "Sfx", 1);
    ASSERT_TRUE(sfx.has_value());
    EXPECT_EQ(sfx->ret, BasicType::Object);
    EXPECT_EQ(sfx->returnClassQName, std::string("Zanna.Audio.Sound"));

    auto build = runtimeMethodIndex().find("Zanna.Audio.MusicGen", "Build", 0);
    ASSERT_TRUE(build.has_value());
    EXPECT_EQ(build->target, std::string("Zanna.Audio.MusicGen.Build"));
    EXPECT_EQ(build->ret, BasicType::Object);
    EXPECT_EQ(build->returnClassQName, std::string("Zanna.Audio.Sound"));
}

TEST(RuntimeMethodIndexBasic, GraphicsSurfaceBindingsAreCataloged) {
    runtimeMethodIndex().seed();

    auto spriteFontBdf = runtimeMethodIndex().find("Zanna.Graphics.BitmapFont", "LoadBDF", 1);
    ASSERT_TRUE(spriteFontBdf.has_value());
    EXPECT_EQ(spriteFontBdf->ret, BasicType::Object);
    EXPECT_EQ(spriteFontBdf->returnClassQName, std::string("Zanna.Graphics.BitmapFont"));
    EXPECT_FALSE(spriteFontBdf->hasReceiver);

    auto pixelsFromBytes = runtimeMethodIndex().find("Zanna.Graphics.Pixels", "FromBytes", 3);
    ASSERT_TRUE(pixelsFromBytes.has_value());
    EXPECT_EQ(pixelsFromBytes->ret, BasicType::Object);
    EXPECT_EQ(pixelsFromBytes->returnClassQName, std::string("Zanna.Graphics.Pixels"));
    EXPECT_FALSE(pixelsFromBytes->hasReceiver);

    auto spriteFromFile = runtimeMethodIndex().find("Zanna.Graphics.Sprite", "FromFile", 1);
    ASSERT_TRUE(spriteFromFile.has_value());
    EXPECT_EQ(spriteFromFile->returnClassQName, std::string("Zanna.Graphics.Sprite"));
    EXPECT_FALSE(spriteFromFile->hasReceiver);

    auto sheetFromGrid = runtimeMethodIndex().find("Zanna.Graphics.SpriteSheet", "FromGrid", 3);
    ASSERT_TRUE(sheetFromGrid.has_value());
    EXPECT_EQ(sheetFromGrid->returnClassQName, std::string("Zanna.Graphics.SpriteSheet"));
    EXPECT_FALSE(sheetFromGrid->hasReceiver);

    auto tilemapLoad = runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "Load", 1);
    ASSERT_TRUE(tilemapLoad.has_value());
    EXPECT_EQ(tilemapLoad->returnClassQName, std::string("Zanna.Graphics2D.Tilemap"));
    EXPECT_FALSE(tilemapLoad->hasReceiver);

    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "LoadCsv", 3).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "SetTileAnim", 3).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "SetTileAnimFrame", 3).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "UpdateAnims", 1).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics2D.Tilemap", "ResolveAnimTile", 1).has_value());

    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Game.ParticleEmitter", "Destroy", 0).has_value());
    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Game.ParticleEmitter", "Destroy", 0).has_value());
    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Game.Lighting2D", "Destroy", 0).has_value());

    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Graphics.Canvas", "GetWindowX", 0).has_value());
    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Graphics.Canvas", "GetWindowY", 0).has_value());
    EXPECT_TRUE(runtimeMethodIndex().find("Zanna.Graphics.Canvas", "SetPosition", 2).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics.Canvas", "GetMonitorWidth", 0).has_value());
    EXPECT_TRUE(
        runtimeMethodIndex().find("Zanna.Graphics.Canvas", "GetMonitorHeight", 0).has_value());
}

TEST(RuntimeMethodIndexBasic, JsonStreamInstanceMethodsDoNotRequireExplicitReceiver) {
    runtimeMethodIndex().seed();

    auto next = runtimeMethodIndex().find("Zanna.Data.JsonStream", "Next", 0);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->ret, BasicType::Int);

    auto hasNext = runtimeMethodIndex().find("Zanna.Data.JsonStream", "HasNext", 0);
    ASSERT_TRUE(hasNext.has_value());
    EXPECT_EQ(hasNext->ret, BasicType::Bool);

    auto wrongArity = runtimeMethodIndex().find("Zanna.Data.JsonStream", "Next", 1);
    EXPECT_FALSE(wrongArity.has_value());
}

TEST(RuntimeMethodIndexBasic, TypedLookupMatchesArgumentTypes) {
    runtimeMethodIndex().seed();

    auto substring = runtimeMethodIndex().find(
        "Zanna.String", "Substring", std::vector<BasicType>{BasicType::Int, BasicType::Int});
    ASSERT_TRUE(substring.has_value());
    EXPECT_EQ(substring->target, std::string("Zanna.String.Substring"));
    EXPECT_EQ(substring->ret, BasicType::String);

    auto badSubstring = runtimeMethodIndex().find(
        "Zanna.String", "Substring", std::vector<BasicType>{BasicType::String, BasicType::Int});
    EXPECT_FALSE(badSubstring.has_value());
}

TEST(RuntimeMethodIndexBasic, TypedLookupAcceptsObjectCompatibleArguments) {
    runtimeMethodIndex().seed();

    auto mapSet =
        runtimeMethodIndex().find("Zanna.Collections.Map",
                                  "Set",
                                  std::vector<BasicType>{BasicType::String, BasicType::String});
    ASSERT_TRUE(mapSet.has_value());
    EXPECT_EQ(mapSet->target, std::string("Zanna.Collections.Map.Set"));

    auto listPush = runtimeMethodIndex().find(
        "Zanna.Collections.List", "Push", std::vector<BasicType>{BasicType::String});
    ASSERT_TRUE(listPush.has_value());
    EXPECT_EQ(listPush->target, std::string("Zanna.Collections.List.Push"));

    auto badPush = runtimeMethodIndex().find(
        "Zanna.Collections.List", "Push", std::vector<BasicType>{BasicType::Void});
    EXPECT_FALSE(badPush.has_value());
}

TEST(RuntimeMethodIndexBasic, TypedLookupAcceptsIntegerBooleanArguments) {
    runtimeMethodIndex().seed();

    auto looping =
        runtimeMethodIndex().find("Zanna.Graphics3D.AnimController3D",
                                  "SetStateLooping",
                                  std::vector<BasicType>{BasicType::String, BasicType::Int});
    ASSERT_TRUE(looping.has_value());
    EXPECT_EQ(looping->target, std::string("Zanna.Graphics3D.AnimController3D.SetStateLooping"));
}

TEST(RuntimeMethodIndexBasic, GuiConcreteWidgetsResolveBaseWidgetMethods) {
    runtimeMethodIndex().seed();

    auto buttonEnabled = runtimeMethodIndex().find("Zanna.GUI.Button", "SetEnabled", 1);
    ASSERT_TRUE(buttonEnabled.has_value());
    EXPECT_EQ(buttonEnabled->target, std::string("Zanna.GUI.Widget.SetEnabled"));
    EXPECT_TRUE(buttonEnabled->hasReceiver);
    ASSERT_EQ(buttonEnabled->args.size(), 1u);
    EXPECT_EQ(buttonEnabled->args[0], BasicType::Bool);

    auto labelClicked = runtimeMethodIndex().find("Zanna.GUI.Label", "WasClicked", 0);
    ASSERT_TRUE(labelClicked.has_value());
    EXPECT_EQ(labelClicked->target, std::string("Zanna.GUI.Widget.WasClicked"));
    EXPECT_EQ(labelClicked->ret, BasicType::Bool);
    EXPECT_TRUE(labelClicked->hasReceiver);

    auto typedTooltip = runtimeMethodIndex().find(
        "Zanna.GUI.TextInput", "SetTooltip", std::vector<BasicType>{BasicType::String});
    ASSERT_TRUE(typedTooltip.has_value());
    EXPECT_EQ(typedTooltip->target, std::string("Zanna.GUI.Widget.SetTooltip"));
    EXPECT_TRUE(typedTooltip->hasReceiver);

    auto menuItemFallback = runtimeMethodIndex().find("Zanna.GUI.MenuItem", "SetTooltip", 1);
    EXPECT_FALSE(menuItemFallback.has_value());
}

TEST(RuntimeMethodIndexBasic, IoNamedFactoriesDoNotCreateNewAliases) {
    runtimeMethodIndex().seed();

    auto binNew = runtimeMethodIndex().find("Zanna.IO.BinFile", "New", 2);
    EXPECT_FALSE(binNew.has_value());

    auto binOpen = runtimeMethodIndex().find("Zanna.IO.BinFile", "Open", 2);
    ASSERT_TRUE(binOpen.has_value());
    EXPECT_EQ(binOpen->target, std::string("Zanna.IO.BinFile.Open"));
    EXPECT_EQ(binOpen->ret, BasicType::Object);

    auto readerNew = runtimeMethodIndex().find("Zanna.IO.LineReader", "New", 1);
    EXPECT_FALSE(readerNew.has_value());

    auto readerOpen = runtimeMethodIndex().find("Zanna.IO.LineReader", "Open", 1);
    ASSERT_TRUE(readerOpen.has_value());
    EXPECT_EQ(readerOpen->target, std::string("Zanna.IO.LineReader.Open"));
    EXPECT_EQ(readerOpen->ret, BasicType::Object);

    auto writerNew = runtimeMethodIndex().find("Zanna.IO.LineWriter", "New", 1);
    EXPECT_FALSE(writerNew.has_value());

    auto writerOpen = runtimeMethodIndex().find("Zanna.IO.LineWriter", "Open", 1);
    ASSERT_TRUE(writerOpen.has_value());
    EXPECT_EQ(writerOpen->target, std::string("Zanna.IO.LineWriter.Open"));
    EXPECT_EQ(writerOpen->ret, BasicType::Object);

    const auto &registry = il::runtime::RuntimeRegistry::instance();
    EXPECT_TRUE(registry.findFunction("Zanna.IO.BinFile.Open").has_value());
    EXPECT_TRUE(registry.findFunction("Zanna.IO.LineReader.Open").has_value());
    EXPECT_TRUE(registry.findFunction("Zanna.IO.LineWriter.Open").has_value());
}

/// @brief Constructors, free functions and property getters report the class their row declares.
/// @details A constructor is not a catalog method, so the registry indexes it from its
///          runtime.def row; frontends infer `x = Zanna.Text.CompiledPattern.New(...)` from it
///          (ADR 0356). BASIC spells qualified names in upper case, so lookups ignore case.
TEST(RuntimeMethodIndexTest, FunctionLookupCoversConstructorsAndGetters) {
    const auto &registry = il::runtime::RuntimeRegistry::instance();

    auto ctor = registry.findFunction("Zanna.Text.CompiledPattern.New");
    ASSERT_TRUE(ctor.has_value());
    EXPECT_EQ(il::runtime::concreteRuntimeReturnClassQName(*ctor),
              std::string("Zanna.Text.CompiledPattern"));

    auto upper = registry.findFunction("ZANNA.GAME2D.SCENEDOCUMENT.NEW");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(il::runtime::concreteRuntimeReturnClassQName(*upper),
              std::string("Zanna.Game2D.SceneDocument"));

    auto getter = registry.findFunction("Zanna.Graphics3D.Camera3D.get_Position");
    ASSERT_TRUE(getter.has_value());
    EXPECT_TRUE(getter->params.empty());
    EXPECT_EQ(il::runtime::concreteRuntimeReturnClassQName(*getter),
              std::string("Zanna.Math.Vec3"));
}

/// @brief Test entry point.
int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
