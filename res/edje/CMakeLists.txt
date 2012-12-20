#ADD_CUSTOM_TARGET(master.edj
#	COMMAND edje_cc -id ${CMAKE_CURRENT_SOURCE_DIR}/image ${EDJE_CFLAGS}
#	${CMAKE_CURRENT_SOURCE_DIR}/master.edc master.edj
#	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/master.edc
#)
#ADD_DEPENDENCIES(${PROJECT_NAME} master.edj)
#INSTALL(FILES master.edj DESTINATION "/opt/apps/org.tizen.${PROJECT_NAME}/res/edje")

# End of a file
