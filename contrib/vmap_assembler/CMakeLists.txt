# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# uncomment next line to disable debug mode
ADD_DEFINITIONS("-DIOMAP_DEBUG")
ADD_DEFINITIONS("-DNO_CORE_FUNCS")

ADD_DEFINITIONS("-Wall")
ADD_DEFINITIONS("-ggdb")
ADD_DEFINITIONS("-O3")

include_directories(../../src/shared)
include_directories(../../src/game/vmap/)
include_directories(../../dep/include/g3dlite/)
include_directories(../../src/framework/)

add_library(vmap
    ${CMAKE_SOURCE_DIR}/src/game/vmap/BIH.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/VMapManager2.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/MapTree.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/TileAssembler.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/WorldModel.cpp
    ${CMAKE_SOURCE_DIR}/src/game/vmap/ModelInstance.cpp
)

target_link_libraries(vmap g3dlite z)

add_executable(vmap_assembler vmap_assembler.cpp)
target_link_libraries(vmap_assembler vmap)

