CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(data-provider-master C)

INCLUDE(FindPkgConfig)
pkg_check_modules(pkg REQUIRED
	ail
	dlog
	aul
	vconf
	sqlite3
	db-util
	glib-2.0
	gio-2.0
	bundle
	ecore-x
	ecore
	evas
	ecore-evas
	com-core
	heynoti
	x11
	libdri2
	libdrm
	libtbm
	xfixes
	dri2proto
	xext
	xdamage
	pkgmgr
	livebox-service
	notification
	badge
)

SET(PACKAGE "${PROJECT_NAME}")
SET(CMAKE_C_FLAGS "-Wall -fpie -Winline -Werror -fno-builtin-malloc -fno-omit-frame-pointer -g")
SET(CMAKE_EXE_LINKER_FLAGS "-pie")

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)

#ADD_DEFINITIONS("-DPREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
ADD_DEFINITIONS("-DPATH_MAX=256")
ADD_DEFINITIONS("-DPACKAGE=\"${PACKAGE}\"")
ADD_DEFINITIONS("-DLOCALEDIR=\"${LOCALEDIR}\"")

ADD_DEFINITIONS("-DINFO_SOCKET=\"/opt/usr/share/live_magazine/.live.socket\"")
ADD_DEFINITIONS("-DCLIENT_SOCKET=\"/opt/usr/share/live_magazine/.client.socket\"")
ADD_DEFINITIONS("-DSLAVE_SOCKET=\"/opt/usr/share/live_magazine/.slave.socket\"")
ADD_DEFINITIONS("-DSERVICE_SOCKET=\"/opt/usr/share/live_magazine/.service.socket\"")

ADD_DEFINITIONS("-DNDEBUG")
#ADD_DEFINITIONS("-DFLOG")
ADD_DEFINITIONS(${pkg_CFLAGS})
ADD_DEFINITIONS(${pkg_LDFLAGS})

ADD_EXECUTABLE(${PROJECT_NAME}
	src/main.c
	src/util.c
	src/fault_manager.c
	src/parser.c
	src/conf.c
	src/dead_monitor.c
	src/group.c
	src/fb.c
	src/script_handler.c
	src/buffer_handler.c
	src/io.c
	src/xmonitor.c
	src/slave_life.c
	src/slave_rpc.c
	src/client_life.c
	src/client_rpc.c
	src/setting.c
	src/package.c
	src/instance.c
	src/server.c
	src/abi.c
	src/critical_log.c
	src/liveinfo.c
	src/pkgmgr.c
	src/event.c
	src/shortcut_service.c
	src/badge_service.c
	src/notification_service.c
	src/service_common.c
)

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${pkg_LDFLAGS} "-ldl")
INSTALL(FILES ${CMAKE_SOURCE_DIR}/data/abi.ini DESTINATION /usr/share/data-provider-master PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

INSTALL(FILES ${CMAKE_SOURCE_DIR}/data/${PRODUCT}.conf.ini DESTINATION /usr/share/data-provider-master RENAME "conf.ini" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

INSTALL(FILES ${CMAKE_SOURCE_DIR}/data/resolution.ini DESTINATION /usr/share/data-provider-master PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME "${PROJECT_NAME}")
INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /usr/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE SETUID)

# INCLUDE FOR BUILD & INSTALL .PO FILES
ADD_SUBDIRECTORY(res)
ADD_SUBDIRECTORY(data)
ADD_SUBDIRECTORY(pkgmgr_livebox)
ADD_SUBDIRECTORY(util_liveinfo)
