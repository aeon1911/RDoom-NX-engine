cmake_minimum_required( VERSION 3.16 )

add_subdirectory( re2c )
add_subdirectory( lemon )
add_subdirectory( zipdir )
# RDoom: the old separate launcher NRO (tools/nxlauncher) is replaced by the
# launcher built into RDoom.nro (../launcher).

set( CROSS_EXPORTS ${CROSS_EXPORTS} PARENT_SCOPE )
