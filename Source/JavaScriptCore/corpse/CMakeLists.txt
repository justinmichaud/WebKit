set(JavaScriptCoreTools_LIBRARY_TYPE STATIC)

set(JavaScriptCoreTools_SOURCES
    CorpseByteParser.cpp
    CorpseClient.cpp
    CorpseError.cpp
    CorpseExportsTrie.cpp
    CorpseProcess.cpp
    CorpseRegion.cpp
    CorpseSnapshot.cpp
    CorpseSymbol.cpp
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

# LLDB and the RTTI its identification needs travel together, and HAVE(MYA_TYPEINFO)
# reads the same include path to decide. -frtti comes after the global -fno-rtti,
# and the compiler honours the later of the two.
if (ENABLE_MYA_TYPEINFO)
    list(APPEND JavaScriptCoreTools_SYSTEM_INCLUDE_DIRECTORIES ${LLDB_INCLUDE_DIRS})
    list(APPEND JavaScriptCoreTools_LIBRARIES ${LLDB_LIBRARIES})
endif ()

WEBKIT_LIBRARY_DECLARE(JavaScriptCoreTools)

WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()

WEBKIT_LIBRARY(JavaScriptCoreTools)

# The corpse sources use JavaScriptCore's generated headers.
add_dependencies(JavaScriptCoreTools JavaScriptCore)

if (ENABLE_MYA_TYPEINFO)
    WEBKIT_ADD_TARGET_CXX_FLAGS(JavaScriptCoreTools -frtti)
endif ()
