set(JavaScriptCoreTools_LIBRARY_TYPE STATIC)

set(JavaScriptCoreTools_SOURCES
    CorpseBackendDarwin.cpp
    CorpseBackendLinux.cpp
    CorpseByteParser.cpp
    CorpseClient.cpp
    CorpseElfImage.cpp
    CorpseError.cpp
    CorpseExportsTrie.cpp
    CorpseProcess.cpp
    CorpseProcessDarwin.cpp
    CorpseProcessLinux.cpp
    CorpseProcFile.cpp
    CorpsePtrace.cpp
    CorpseRegion.cpp
    CorpseSnapshot.cpp
    CorpseSymbol.cpp
    CorpseSymbolElf.cpp
    CorpseSymbolMachO.cpp
    CorpseTargetObject.cpp
    CorpseTargetType.cpp
    CorpseTypeInfo.cpp
    CorpseTypeName.cpp
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

# Corpse type introspection reads the target's debug info through LLDB's SB API.
# On Darwin that framework is part of the SDK and is linked by the Xcode build;
# anywhere else the build has to go looking for it. Not finding it is not an
# error: TypeSystem::create then reports that there is no type system, and
# everything that does not need the target's types still works.
if (NOT APPLE)
    find_package(LLDB)
    if (LLDB_FOUND)
        list(APPEND JavaScriptCoreTools_SYSTEM_INCLUDE_DIRECTORIES ${LLDB_INCLUDE_DIRS})
        list(APPEND JavaScriptCoreTools_LIBRARIES ${LLDB_LIBRARIES})
        # Read by CorpsePlatform.h. PUBLIC, because a tool linking this library
        # has to agree with it about whether there is a type system.
        list(APPEND JavaScriptCoreTools_DEFINITIONS CORPSE_USE_LLDB=1)
    else ()
        message(STATUS "liblldb was not found: the corpse tools will not be able to read "
            "the target's types. Install the LLDB development package, or set LLDB_ROOT.")
    endif ()
endif ()

WEBKIT_LIBRARY_DECLARE(JavaScriptCoreTools)

WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()

WEBKIT_LIBRARY(JavaScriptCoreTools)

# The corpse sources use JavaScriptCore's generated headers.
add_dependencies(JavaScriptCoreTools JavaScriptCore)
