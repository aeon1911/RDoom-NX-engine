cmake_minimum_required( VERSION 3.1.0 )

if( NINTENDO_SWITCH )
	set(APP_NAME "UZDoom Launcher")
	set(APP_AUTHOR "fgsfds, UZDoom team")
	set(APP_VERSION "${SWITCH_APP_VERSION}")
	set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/gzdoom_launcher.jpg")
	set(APP_RESDIR "${CMAKE_CURRENT_SOURCE_DIR}/res")
	file( GLOB SRC_HDR src/*.h )
	file( GLOB SRC_FILES src/*.c )
	add_executable( gzdoom_launcher ${SRC_HDR} ${SRC_FILES} )
	set_target_properties( gzdoom_launcher PROPERTIES C_STANDARD 11 )
	target_compile_options( gzdoom_launcher PRIVATE -O2 -D_GNU_SOURCE -Wno-format-truncation )
	target_link_libraries( gzdoom_launcher -Wl,--start-group SDL2 EGL glapi drm_nouveau z nx m stdc++ -Wl,--end-group )
	add_custom_command( TARGET gzdoom_launcher POST_BUILD
		COMMAND nacptool --create "${APP_NAME}" "${APP_AUTHOR}" "${APP_VERSION}" "gzdoom_launcher.nacp"
		COMMAND ${CMAKE_STRIP} -o gzdoom_launcher_stripped.elf $<TARGET_FILE:gzdoom_launcher>
		COMMAND elf2nro gzdoom_launcher_stripped.elf "${CMAKE_BINARY_DIR}/gzdoom_launcher.nro" --icon="${APP_ICON}" --nacp=gzdoom_launcher.nacp --romfsdir="${APP_RESDIR}" )
endif()
