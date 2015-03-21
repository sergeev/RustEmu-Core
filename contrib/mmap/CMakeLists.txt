# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

ADD_DEFINITIONS(-DMMAP_GENERATOR -DNO_CORE_FUNCS -DDEBUG)
ADD_DEFINITIONS(-DDT_POLYREF64)

# zlib
ADD_DEFINITIONS( -DNO_vsnprintf )

ADD_DEFINITIONS("-ggdb")
ADD_DEFINITIONS("-O3")

include_directories(
   ${CMAKE_SOURCE_DIR}/src
   ${CMAKE_SOURCE_DIR}/src/shared
   ${CMAKE_SOURCE_DIR}/src/game
   ${CMAKE_SOURCE_DIR}/src/game/vmap
   ${CMAKE_SOURCE_DIR}/dep/include/g3dlite
   ${CMAKE_SOURCE_DIR}/src/framework
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Include
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Include
   ${CMAKE_SOURCE_DIR}/dep/src/zlib
   ${CMAKE_SOURCE_DIR}/dep/src/
)

add_library(mmap_vmap
    ${CMAKE_SOURCE_DIR}/src/game/vmap/BIH.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/VMapManager2.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/MapTree.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/TileAssembler.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/WorldModel.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/ModelInstance.cpp
)

target_link_libraries(mmap_vmap g3dlite zlib)

add_library(Detour
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourAlloc.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourCommon.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourNavMesh.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourNavMeshBuilder.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourNavMeshQuery.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Detour/Source/DetourNode.cpp
)

add_library(Recast
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/Recast.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastAlloc.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastArea.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastContour.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastFilter.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastMesh.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastMeshDetail.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastRasterization.cpp
   ${CMAKE_SOURCE_DIR}/dep/recastnavigation/Recast/Source/RecastRegion.cpp
)

set(SOURCES
    src/IntermediateValues.cpp
    src/generator.cpp
    src/MapBuilder.cpp
    src/TerrainBuilder.cpp
    src/VMapExtensions.cpp
)

add_executable( MoveMapGen ${SOURCES} )

IF(WIN32)
  target_link_libraries( MoveMapGen g3dlite vmap Detour Recast zlib )
ELSE()
  target_link_libraries( MoveMapGen g3dlite vmap Detour Recast z )
ENDIF()
