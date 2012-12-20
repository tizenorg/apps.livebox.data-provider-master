PROJECT(livebox C)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/pkgmgr_livebox/include)

INCLUDE(FindPkgConfig)
pkg_check_modules(bin_pkgs REQUIRED
	dlog
	sqlite3
	libxml-2.0
	db-util
)

FOREACH(flag ${bin_pkgs_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
ENDFOREACH(flag)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -g -Wall -Werror")

ADD_DEFINITIONS("-DPREFIX=\"${PREFIX}\"")

ADD_LIBRARY(${PROJECT_NAME} SHARED
	src/service_register.c
	src/dlist.c
)

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${bin_pkgs_LDFLAGS})

INSTALL(TARGETS ${PROJECT_NAME} DESTINATION "etc/package-manager/parserlib")

# End of a file
