add_library(zanna_codegen_common STATIC
        CallArgLayout.cpp
        Diagnostics.cpp
        LinkerSupport.cpp
        NativeEHLowering.cpp
        objfile/ObjectFileWriter.cpp
        objfile/ElfWriter.cpp
        objfile/MachOWriter.cpp
        objfile/CoffWriter.cpp
        objfile/StringTable.cpp
        objfile/SymbolTable.cpp
        objfile/DebugLineTable.cpp
        linker/ArchiveReader.cpp
        linker/ObjFileReader.cpp
        linker/ElfReader.cpp
        linker/MachOReader.cpp
        linker/CoffReader.cpp
        linker/SymbolResolver.cpp
        linker/DeadStripPass.cpp
        linker/StringDedup.cpp
        linker/ICF.cpp
        linker/BranchTrampoline.cpp
        linker/SectionMerger.cpp
        linker/RelocApplier.cpp
        linker/ElfExeWriter.cpp
        linker/ElfSymbolVersions.cpp
        linker/MachOCodeSign.cpp
        linker/MachOBindRebase.cpp
        linker/MachOExeWriter.cpp
        linker/PeExeWriter.cpp
        linker/DynStubGen.cpp
        linker/LinuxImportPlanner.cpp
        linker/MacImportPlanner.cpp
        linker/NativeLinker.cpp)
target_sources(zanna_codegen_common PRIVATE
        linker/WindowsImportPlanner.cpp)
target_include_directories(zanna_codegen_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(zanna_codegen_common PUBLIC zanna_common zanna_common_opts)
set_target_properties(zanna_codegen_common PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
