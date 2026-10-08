set(JavaScriptCoreTools_LIBRARY_TYPE STATIC)

set(JavaScriptCoreTools_SOURCES
    CorpseByteParser.cpp
    CorpseClient.cpp
    CorpseError.cpp
    CorpseExportsTrie.cpp
    CorpseHeapWalk.cpp
    CorpseImage.cpp
    CorpseMemory.cpp
    CorpseProcess.cpp
    CorpseRegion.cpp
    CorpseSnapshot.cpp
    CorpseSnapshotDebugInfo.cpp
    CorpseSymbol.cpp
    CorpseTargetType.cpp
    CorpseTargetValue.cpp
    CorpseThread.cpp
)

set(JavaScriptCoreTools_PRIVATE_INCLUDE_DIRECTORIES
    $<TARGET_PROPERTY:JavaScriptCore,INCLUDE_DIRECTORIES>
)

set(JavaScriptCoreTools_FRAMEWORKS
    JavaScriptCore
    WTF
    bmalloc
)

if (TARGET LLDB::LLDB)
    list(APPEND JavaScriptCoreTools_LIBRARIES LLDB::LLDB)
endif ()

WEBKIT_LIBRARY_DECLARE(JavaScriptCoreTools)

WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()

WEBKIT_LIBRARY(JavaScriptCoreTools)

# The corpse sources use JavaScriptCore's generated headers.
add_dependencies(JavaScriptCoreTools JavaScriptCore)

# A heap walk reads every value of a heap through WTF's containers, which at
# -O0 makes a walk of a small heap take 1.4 times as long. JavaScriptCore itself
# stays as the build type says.
#
# Optimizing, clang outside Darwin makes a destructor that does no more than its
# base's an alias of it, so a vtable's destructor slot names the base class, and
# the walk refuses the dynamic type of the tools' own polymorphic objects, such as
# their WTF::Functions, which an in-process snapshot holds.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(JavaScriptCoreTools PRIVATE -O2)
    if (COMPILER_IS_CLANG AND NOT APPLE)
        target_compile_options(JavaScriptCoreTools PRIVATE -Xclang -mno-constructor-aliases)
    endif ()
endif ()
