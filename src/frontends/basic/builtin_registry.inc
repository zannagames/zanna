//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/builtin_registry.inc
// Purpose: Supplies the generated X-macro rows that define BASIC builtin
//          descriptors, semantic scan rules, and IL lowering rules.
// Key invariants:
//   - Each row's NAME matches a BuiltinCallExpr::Builtin enumerator.
//   - Descriptor arity, scan traversal, and lowering variants describe the
//     same source-language call shape.
//   - Runtime features requested during scanning agree with those selected
//     during lowering.
// Ownership/Lifetime:
//   - This include fragment owns no storage and is expanded by each includer.
//   - Descriptor strings and runtime helper names become part of the
//     includer's static registry data.
// Links: src/frontends/basic/BuiltinRegistry.cpp,
//        src/frontends/basic/BuiltinRegistry.hpp,
//        docs/adr/0001-builtin-signatures-from-registry.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the generated X-macro table for BASIC builtins.
/// @details The file intentionally has no include guard because
///          BuiltinRegistry.cpp includes it repeatedly with different
///          definitions of `BUILTIN`. Each expansion projects the same rows
///          into descriptor, lowering-rule, or scan-rule tables.

// Each row expands via the BUILTIN macro defined by the includer.
// Parameters:
//   NAME           - Enum suffix from BuiltinCallExpr::Builtin.
//   DESCRIPTOR     - BuiltinDescriptor expression.
//   LOWER_RULE     - BuiltinLoweringRule expression.
//   SCAN_RULE      - BuiltinScanRule expression.
// clang-format off
BUILTIN(
    Len,
    (BuiltinDescriptor{"LEN", B::Len, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_len",
                                   .arguments = {Argument{.index = 0}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {}}))
BUILTIN(
    Mid,
    (BuiltinDescriptor{"MID$", B::Mid, {2, 3}, TypeMask::Str, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
        .variants = {
            Variant{.condition = Condition::IfArgPresent,
                    .conditionArg = 2,
                    .callLocArg = 2,
                    .kind = VariantKind::CallRuntime,
                    .runtime = "rt_str_mid_len",
                    .arguments = {Argument{.index = 0},
                                  Argument{.index = 1,
                                           .transforms = {Transform{.kind = TransformKind::EnsureI64}}},
                                  Argument{.index = 2,
                                           .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                    .features = {Feature{.action = FeatureAction::Request,
                                         .feature = il::runtime::RuntimeFeature::Mid3}}},
            Variant{.condition = Condition::IfArgMissing,
                    .conditionArg = 2,
                    .kind = VariantKind::CallRuntime,
                    .runtime = "rt_str_mid",
                    .arguments = {Argument{.index = 0},
                                  Argument{.index = 1,
                                           .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                    .features = {Feature{.action = FeatureAction::Request,
                                         .feature = il::runtime::RuntimeFeature::Mid2}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::All,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::IfArgPresent,
                                              il::runtime::RuntimeFeature::Mid3,
                                              2,
                                              Lowerer::ExprType::I64},
                     BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::IfArgMissing,
                                              il::runtime::RuntimeFeature::Mid2,
                                              2,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Left,
    (BuiltinDescriptor{"LEFT$", B::Left, {2, 2}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_left",
                                   .arguments = {Argument{.index = 0},
                                                 Argument{.index = 1,
                                                          .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Left}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0, 1},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Left,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Right,
    (BuiltinDescriptor{"RIGHT$", B::Right, {2, 2}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_right",
                                   .arguments = {Argument{.index = 0},
                                                 Argument{.index = 1,
                                                          .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Right}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0, 1},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Right,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Str,
    (BuiltinDescriptor{"STR$", B::Str, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .callLocArg = 0,
                                   .kind = VariantKind::Custom,
                                   .arguments = {Argument{.index = 0}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {}}))
BUILTIN(
    Val,
    (BuiltinDescriptor{"VAL", B::Val, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::F64},
              .variants = {Variant{.condition = Condition::Always,
                                   .callLocArg = 0,
                                   .kind = VariantKind::Custom,
                                   .runtime = "rt_val_to_double",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Val}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::F64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Val,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Cint,
    (BuiltinDescriptor{"CINT", B::Cint, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .callLocArg = 0,
                             .kind = VariantKind::Custom,
                             .runtime = "rt_cint_from_double",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureF64}}}},
                             .features = {Feature{.action = FeatureAction::Request,
                                                  .feature = il::runtime::RuntimeFeature::CintFromDouble}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::CintFromDouble,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Clng,
    (BuiltinDescriptor{"CLNG", B::Clng, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .callLocArg = 0,
                             .kind = VariantKind::Custom,
                             .runtime = "rt_clng_from_double",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureF64}}}},
                             .features = {Feature{.action = FeatureAction::Request,
                                                  .feature = il::runtime::RuntimeFeature::ClngFromDouble}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::ClngFromDouble,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Csng,
    (BuiltinDescriptor{"CSNG", B::Csng, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::F64},
        .variants = {Variant{.condition = Condition::Always,
                             .callLocArg = 0,
                             .kind = VariantKind::Custom,
                             .runtime = "rt_csng_from_double",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureF64}}}},
                             .features = {Feature{.action = FeatureAction::Request,
                                                  .feature = il::runtime::RuntimeFeature::CsngFromDouble}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::F64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::CsngFromDouble,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Cdbl,
    (BuiltinDescriptor{"CDBL", B::Cdbl, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::F64},
        .variants = {Variant{.condition = Condition::Always,
                             .callLocArg = 0,
                             .kind = VariantKind::CallRuntime,
                             .runtime = "rt_cdbl_from_any",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureF64}}}},
                             .features = {Feature{.action = FeatureAction::Request,
                                                  .feature = il::runtime::RuntimeFeature::CdblFromAny}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::F64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::CdblFromAny,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Argc,
    (BuiltinDescriptor{"ARGC", B::Argc, {0, 0}, TypeMask::I64, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_args_count",
                                   .arguments = {},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::ArgsCount}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::ArgsCount,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    ArgGet,
    (BuiltinDescriptor{"ARG$", B::ArgGet, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_args_get",
                                   .arguments = {Argument{.index = 0,
                                                          .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::ArgsGet}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::ArgsGet,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Command,
    (BuiltinDescriptor{"COMMAND$", B::Command, {0, 0}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_cmdline",
                                   .arguments = {},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Cmdline}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Cmdline,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Int,
    (BuiltinDescriptor{"INT", B::Int, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Fix,
    (BuiltinDescriptor{"FIX", B::Fix, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Round,
    (BuiltinDescriptor{"ROUND", B::Round, {1, 2}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Sqr,
    (BuiltinDescriptor{"SQR", B::Sqr, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Abs,
    (BuiltinDescriptor{"ABS", B::Abs, {1, 1}, TypeMask::I64 | TypeMask::F64, &SemanticAnalyzer::analyzeAbs}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Floor,
    (BuiltinDescriptor{"FLOOR", B::Floor, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Ceil,
    (BuiltinDescriptor{"CEIL", B::Ceil, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Sin,
    (BuiltinDescriptor{"SIN", B::Sin, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Cos,
    (BuiltinDescriptor{"COS", B::Cos, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Tan,
    (BuiltinDescriptor{"TAN", B::Tan, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Atn,
    (BuiltinDescriptor{"ATN", B::Atn, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Exp,
    (BuiltinDescriptor{"EXP", B::Exp, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Log,
    (BuiltinDescriptor{"LOG", B::Log, {1, 1}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Sgn,
    (BuiltinDescriptor{"SGN", B::Sgn, {1, 1}, TypeMask::I64 | TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Pow,
    (BuiltinDescriptor{"POW", B::Pow, {2, 2}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))

BUILTIN(
    Rnd,
    (BuiltinDescriptor{"RND", B::Rnd, {0, 0}, TypeMask::F64, nullptr}),
    (LowerRule{}),
    (BuiltinScanRule{}))
BUILTIN(
    Instr,
    (BuiltinDescriptor{"INSTR", B::Instr, {2, 3}, TypeMask::I64, &SemanticAnalyzer::analyzeInstr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {
            Variant{.condition = Condition::IfArgPresent,
                    .conditionArg = 2,
                    .callLocArg = 2,
                    .kind = VariantKind::CallRuntime,
                    .runtime = "rt_str_instr3",
                    .arguments = {Argument{.index = 0,
                                           .transforms = {Transform{.kind = TransformKind::EnsureI64},
                                                          Transform{.kind = TransformKind::AddConst,
                                                                    .immediate = -1}}},
                                  Argument{.index = 1},
                                  Argument{.index = 2}},
                    .features = {Feature{.action = FeatureAction::Request,
                                         .feature = il::runtime::RuntimeFeature::Instr3}}},
            Variant{.condition = Condition::IfArgMissing,
                    .conditionArg = 2,
                    .callLocArg = 1,
                    .kind = VariantKind::CallRuntime,
                    .runtime = "rt_str_index_of",
                    .arguments = {Argument{.index = 0}, Argument{.index = 1}},
                    .features = {Feature{.action = FeatureAction::Request,
                                         .feature = il::runtime::RuntimeFeature::Instr2}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::All,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::IfArgPresent,
                                              il::runtime::RuntimeFeature::Instr3,
                                              2,
                                              Lowerer::ExprType::I64},
                     BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::IfArgMissing,
                                              il::runtime::RuntimeFeature::Instr2,
                                              2,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Ltrim,
    (BuiltinDescriptor{"LTRIM$", B::Ltrim, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_ltrim",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Ltrim}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Ltrim,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Rtrim,
    (BuiltinDescriptor{"RTRIM$", B::Rtrim, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_rtrim",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Rtrim}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Rtrim,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Trim,
    (BuiltinDescriptor{"TRIM$", B::Trim, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_trim",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Trim}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Trim,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Ucase,
    (BuiltinDescriptor{"UCASE$", B::Ucase, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_ucase",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Ucase}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Ucase,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Lcase,
    (BuiltinDescriptor{"LCASE$", B::Lcase, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_lcase",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Lcase}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Lcase,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Chr,
    (BuiltinDescriptor{"CHR$", B::Chr, {1, 1}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_chr",
                                   .arguments = {Argument{.index = 0,
                                                          .transforms = {Transform{.kind = TransformKind::EnsureI64}}}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Chr}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Chr,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    Asc,
    (BuiltinDescriptor{"ASC", B::Asc, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_str_asc",
                                   .arguments = {Argument{.index = 0}},
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::Asc}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Asc,
                                              0,
                                              Lowerer::ExprType::I64}}}))
BUILTIN(
    InKey,
    (BuiltinDescriptor{"INKEY$", B::InKey, {0, 0}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_inkey_str",
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::InKey}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::InKey}}}))
BUILTIN(
    GetKey,
    (BuiltinDescriptor{"GETKEY$", B::GetKey, {0, 0}, TypeMask::Str, nullptr}),
    (LowerRule{.result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::Str},
              .variants = {Variant{.condition = Condition::Always,
                                   .kind = VariantKind::CallRuntime,
                                   .runtime = "rt_getkey_str",
                                   .features = {Feature{.action = FeatureAction::Request,
                                                        .feature = il::runtime::RuntimeFeature::GetKey}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::Str,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::GetKey}}}))
BUILTIN(
    Eof,
    (BuiltinDescriptor{"EOF", B::Eof, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .kind = VariantKind::CallRuntime,
                             .runtime = "rt_eof_ch",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureI32}}}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {}}))
BUILTIN(
    Lof,
    (BuiltinDescriptor{"LOF", B::Lof, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .kind = VariantKind::CallRuntime,
                             .runtime = "rt_lof_ch",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureI32}}}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {}}))
BUILTIN(
    Loc,
    (BuiltinDescriptor{"LOC", B::Loc, {1, 1}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .kind = VariantKind::CallRuntime,
                             .runtime = "rt_loc_ch",
                             .arguments = {Argument{.index = 0,
                                                    .transforms = {Transform{.kind = TransformKind::EnsureI32}}}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {0},
                    {}}))
BUILTIN(
    Timer,
    (BuiltinDescriptor{"TIMER", B::Timer, {0, 0}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .kind = VariantKind::CallRuntime,
                             .runtime = "rt_timer_ms",
                             .arguments = {},
                             .features = {Feature{.action = FeatureAction::Request,
                                                  .feature = il::runtime::RuntimeFeature::Timer}}}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {BuiltinScanRule::Feature{BuiltinScanRule::Feature::Action::Request,
                                              BuiltinScanRule::Feature::Condition::Always,
                                              il::runtime::RuntimeFeature::Timer}}}))
BUILTIN(
    Err,
    (BuiltinDescriptor{"ERR", B::Err, {0, 0}, TypeMask::I64, nullptr}),
    (LowerRule{
        .result = {.kind = ResultSpec::Kind::Fixed, .type = Lowerer::ExprType::I64},
        .variants = {Variant{.condition = Condition::Always,
                             .kind = VariantKind::Custom}}}),
    (BuiltinScanRule{BuiltinScanRule::ResultSpec{BuiltinScanRule::ResultSpec::Kind::Fixed,
                                                Lowerer::ExprType::I64,
                                                0},
                    BuiltinScanRule::ArgTraversal::Explicit,
                    {},
                    {}}))
// clang-format on
