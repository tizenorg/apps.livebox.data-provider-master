PROJECT(liveinfo C)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/util_liveinfo/include)

INCLUDE(FindPkgConfig)
pkg_check_modules(info_pkgs REQUIRED
	dlog
	db-util
	com-core
	ecore
	glib-2.0
	gio-2.0
	livebox-service
	xdamage
	xfixes
	x11
	xext
)

FOREACH(flag ${info_pkgs_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
ENDFOREACH(flag)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -g -Wall -Werror")

ADD_DEFINITIONS("-DPREFIX=\"${PREFIX}\"")
ADD_DEFINITIONS("-DSOCKET_FILE=\"/opt/usr/share/live_magazine/.live.socket\"")

ADD_EXECUTABLE(${PROJECT_NAME}
	src/liveinfo.c
	src/node.c
)

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${info_pkgs_LDFLAGS})

INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /usr/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# End of a file
