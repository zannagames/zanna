set(ZIA_SOURCES
        Warnings.cpp
        Lexer.cpp
        ImportResolver.cpp
        Parser_Tokens.cpp
        Parser_Expr.cpp
        Parser_Expr_Primary.cpp
        Parser_Expr_Pattern.cpp
        Parser_Stmt.cpp
        Parser_Type.cpp
        Parser_Decl.cpp
        Types.cpp
        RuntimeAdapter.cpp
        sema/SemaTypes.cpp
        Sema.cpp
        Sema_Scope.cpp
        Sema_Diagnostics.cpp
        Sema_Module.cpp
        Sema_Generics.cpp
        Sema_TypeResolution.cpp
        Sema_Runtime.cpp
        Sema_Completion.cpp
        Sema_Decl.cpp
        Sema_Stmt.cpp
        Sema_Expr.cpp
        Sema_Expr_Ops.cpp
        Sema_Expr_Call.cpp
        Sema_Expr_Advanced.cpp
        Lowerer.cpp
        Lowerer_Decl.cpp
        Lowerer_Decl_Types.cpp
        Lowerer_Decl_Functions.cpp
        Lowerer_Stmt.cpp
        Lowerer_Stmt_EH.cpp
        Lowerer_Expr.cpp
        Lowerer_Expr_Complex.cpp
        Lowerer_Expr_Optional.cpp
        Lowerer_Expr_Lambda.cpp
        Lowerer_Expr_Literals.cpp
        Lowerer_Expr_Binary.cpp
        Lowerer_Expr_Call.cpp
        Lowerer_Expr_Method.cpp
        Lowerer_Expr_Collections.cpp
        LowererBinaryOperatorLowerer.cpp
        LowererCallArgumentLowerer.cpp
        LowererCollectionLowerer.cpp
        LowererRuntimeCallBuilder.cpp
        Lowerer_Expr_Match.cpp
        Lowerer_Dispatch.cpp
        Lowerer_Emit.cpp
        LowererTypeLayout.cpp
        ZiaLocationScope.cpp
        Compiler.cpp
        ZiaAstPrinter.cpp
)

add_library(fe_zia ${ZIA_SOURCES})

target_compile_options(fe_zia PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>,$<BOOL:$<STREQUAL:${CMAKE_CXX_COMPILER_FRONTEND_VARIANT},MSVC>>>:/clang:-fconstexpr-steps=100000000>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>,$<NOT:$<BOOL:$<STREQUAL:${CMAKE_CXX_COMPILER_FRONTEND_VARIANT},MSVC>>>>:-fconstexpr-steps=100000000>
        # 100000000 is GCC's default, so the budget has to be raised past it.
        $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=1000000000>
)

target_include_directories(fe_zia PUBLIC
        ${CMAKE_BINARY_DIR}/generated
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(fe_zia PUBLIC
        zanna_support
        zanna_il_core
        il_build
        il_runtime
        il_transform
        fe_common
        PRIVATE
        zanna_common_opts
)
add_dependencies(fe_zia generate_runtime_registry)

add_library(zia_editor_services
        ZiaCompletion.cpp
        rt_zia_completion.cpp
        rt_zia_highlight.cpp
)

target_compile_options(zia_editor_services PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

target_include_directories(zia_editor_services PUBLIC
        ${CMAKE_BINARY_DIR}/generated
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(zia_editor_services PUBLIC
        fe_zia
        zanna_support
        zanna_il_core
        il_build
        il_runtime
        il_transform
        fe_common
        PRIVATE
        zanna_common_opts
)
add_dependencies(zia_editor_services generate_runtime_registry)
