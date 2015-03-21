
INCLUDE_DIRECTORIES( ${CMAKE_CURRENT_SOURCE_DIR} )
ADD_LIBRARY( mpq STATIC common.c huffman.c extract.c explode.c mpq.c wave.c )
