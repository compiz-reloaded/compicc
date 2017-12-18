CMAKE_MINIMUM_REQUIRED (VERSION 2.6.2)

PROJECT (compicc)

SET (COMPICC_VERSION_MAJOR 0)
SET (COMPICC_VERSION_MINOR 8)
SET (COMPICC_VERSION_MICRO 11)


SET (PACKAGE_NAME "compicc")
SET (PACKAGE_DESCRIPTION "CompICC is a Display Color Server for Compiz-0.8.x . The Compiz ICC color server, or short compicc, lets you color manage your whole desktop at once and in hardware. Play movies, watch images on wide or narrow gamut displays. Each connected monitor is color corrected for its own.")

SET( OY_CMMSUBPATH     "color/modules" )
SET( OY_METASUBPATH    "${PACKAGE_NAME}" )

#additional cmake modules for loading libraries
SET (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

IF (NOT CMAKE_BUILD_TYPE)
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
ENDIF (NOT CMAKE_BUILD_TYPE)


# some basic and important variables
SET( COMPICC_VERSION "${COMPICC_VERSION_MAJOR}.${COMPICC_VERSION_MINOR}.${OYRANOS_VERSION_MICRO}" )
SET( COMPICC_VERSION_MAJORMINOR "${COMPICC_VERSION_MAJOR}.${COMPICC_VERSION_MINOR}" )

IF( ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  SET(PLATFORM "OSX")
ELSEIF( ${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  SET(PLATFORM "Linux")
ELSE( ${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  SET(PLATFORM "${CMAKE_SYSTEM_NAME}")
ENDIF( ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
MESSAGE( "-- Found PLATFORM: ${PLATFORM}" )

IF( CMAKE_BUILD_TYPE MATCHES "[Dd][Ee][Bb][Uu][Gg]" )
  MESSAGE( "-- Info build \"${CMAKE_BUILD_TYPE}\"" )
  SET( DEBUG 1 )
ENDIF()

FIND_PROGRAM(CMAKE_AR_TMP NAMES ar)

FIND_PACKAGE(Oyranos REQUIRED)
IF( OYRANOS_FOUND )
  INCLUDE_DIRECTORIES( ${OYRANOS_INCLUDE_DIRS} )
  LINK_DIRECTORIES( ${OYRANOS_LIBRARY_DIRS} )
ENDIF( OYRANOS_FOUND )

FIND_PACKAGE(Xcm REQUIRED)
IF( XCM_FOUND )
  INCLUDE_DIRECTORIES( ${XCM_INCLUDE_DIRS} )
  LINK_DIRECTORIES( ${XCM_LIBRARY_DIRS} )
ENDIF( XCM_FOUND )

FIND_PACKAGE(Compiz)
IF( COMPIZ_FOUND )
  INCLUDE_DIRECTORIES( ${COMPIZ_INCLUDE_DIRS} )
  LINK_DIRECTORIES( ${COMPIZ_LIBDIR} )
ELSE( COMPIZ_FOUND )
  MESSAGE("test pkg-config compiz --libs --cflags")
  FIND_PACKAGE(PkgConfig)
  PKG_CHECK_MODULES( COMPIZ compiz )
  MESSAGE("test pkg-config compiz --libs --cflags")
  
  INCLUDE_DIRECTORIES( ${COMPIZ_INCLUDE_DIRS} )
  LINK_DIRECTORIES( ${COMPIZ_LIBDIR} )
ENDIF( COMPIZ_FOUND )


if (C_STD)
        message (STATUS "use C_STD as given by user: ${C_STD}")
else()
        set (C_STD "-std=c99 -D__USE_POSIX2=1 -fPIC -DPIC")
endif()
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${C_STD}")

SET_SOURCE_FILES_PROPERTIES( ${PACKAGE_NAME}.c
                             PROPERTIES LANGUAGE_DIALECT C99 ) 
ADD_LIBRARY( ${PACKAGE_NAME}
             SHARED ${PACKAGE_NAME}.c )
TARGET_LINK_LIBRARIES ( ${PACKAGE_NAME}  ${OYRANOS_LIBRARIES} ${COMPIZ_LIBRARIES} ${EXTRA_LIBS} )
INSTALL (TARGETS ${PACKAGE_NAME} DESTINATION lib${LIB_SUFFIX}/compiz)
INSTALL (FILES ${PACKAGE_NAME}.xml DESTINATION share/compiz)
INSTALL (FILES plugin-${PACKAGE_NAME}.svg DESTINATION share/ccsm/icons/hicolor/scalable/apps)


CONFIGURE_FILE(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompICCUninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/CompICCUninstall.cmake"
    IMMEDIATE @ONLY)

ADD_CUSTOM_TARGET( uninstall
	"${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/CompICCUninstall.cmake")

ADD_CUSTOM_TARGET( dist
		COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR}/ && git archive --prefix=compicc-${COMPICC_VERSION}/ --format=tar HEAD > ${CMAKE_CURRENT_BINARY_DIR}/compicc-${COMPICC_VERSION}.tar 
                COMMAND echo "git:       git clone git://guthub.com/compicc-cms/compicc.git" > ${CMAKE_CURRENT_BINARY_DIR}/compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND echo "git sha1:  " | tr -d "\\n" >> ${CMAKE_CURRENT_BINARY_DIR}/compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR}/ && git rev-parse HEAD >> ${CMAKE_CURRENT_BINARY_DIR}/compicc-${COMPICC_VERSION}.tar.bz2.hashsum 
                COMMAND rm -rf compicc-${COMPICC_VERSION} && tar xf compicc-${COMPICC_VERSION}.tar 
                COMMAND cp -ar ${CMAKE_SOURCE_DIR}/elektra-0.7.1/ compicc-${COMPICC_VERSION} && tar cf - compicc-${COMPICC_VERSION} | bzip2 > compicc-${COMPICC_VERSION}.tar.bz2
                COMMAND echo  "package:   http://sourceforge.net/projects/compicc/files/CompICC/CompICC%20${COMPICC_VERSION_MAJORMINOR}/compicc-${COMPICC_VERSION}.tar.bz2" >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND echo  "size:      " | tr -d "\\n" >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND du -b compicc-${COMPICC_VERSION}.tar.bz2 | sed s/[[:space:]][[:print:]]*// >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum 
                COMMAND echo  "md5sum:    " | tr -d "\\n" >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND cd ${CMAKE_CURRENT_BINARY_DIR}/ && md5sum compicc-${COMPICC_VERSION}.tar.bz2 >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND echo  "sha1sum:   " | tr -d "\\n" >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND sha1sum compicc-${COMPICC_VERSION}.tar.bz2 >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND echo  "sha256sum: " | tr -d "\\n" >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
                COMMAND sha256sum compicc-${COMPICC_VERSION}.tar.bz2 >> compicc-${COMPICC_VERSION}.tar.bz2.hashsum
		COMMENT Make a compicc source package by using git
		VERBATIM
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
		)
EXECUTE_PROCESS(COMMAND rpmbuild --showrc
                COMMAND awk '/^build arch/ {print $$4}'
                OUTPUT_VARIABLE RPMARCH
                ERROR_VARIABLE RPMBUILD_ERROR
               )
ADD_CUSTOM_TARGET( rpm
		COMMAND test -d ${PACKAGE_NAME}-${COMPICC_VERSION}/ && rm -rf ${PACKAGE_NAME}-${COMPICC_VERSION}/
		COMMAND tar xjf ${PACKAGE_NAME}-${COMPICC_VERSION}.tar.bz2
                COMMAND mkdir -p rpmdir/BUILD rpmdir/SPECS rpmdir/SOURCES rpmdir/SRPMS rpmdir/RPMS/${RPMARCH}
                COMMAND cp -f ${PACKAGE_NAME}-${COMPICC_VERSION}.tar.bz2 rpmdir/SOURCES
                COMMAND rpmbuild --clean -ba extras/${PACKAGE_NAME}.spec --define "_topdir ${CMAKE_CURRENT_BINARY_DIR}/rpmdir"
                DEPENDS dist
		COMMENT Make a compicc RPM package
		VERBATIM
		)

ADD_CUSTOM_TARGET( debsrc
		COMMAND test -d ${PACKAGE_NAME}-${COMPICC_VERSION}/ && rm -rf ${PACKAGE_NAME}-${COMPICC_VERSION}/
		COMMAND tar xjf ${PACKAGE_NAME}-${COMPICC_VERSION}.tar.bz2
                COMMAND mv ${PACKAGE_NAME}-${COMPICC_VERSION}.tar.bz2 ${PACKAGE_NAME}_${COMPICC_VERSION}.orig.tar.bz2
                COMMAND cp -R ${CMAKE_SOURCE_DIR}/extras/debian ${PACKAGE_NAME}-${COMPICC_VERSION}/
                COMMAND dpkg-source -b ${PACKAGE_NAME}-${COMPICC_VERSION}/
                DEPENDS dist
		COMMENT Make a compicc DEB source package
		VERBATIM
		)
ADD_CUSTOM_TARGET( debian
		COMMAND cd ${PACKAGE_NAME}-${COMPICC_VERSION}/ && debuild
                DEPENDS debsrc
		COMMENT Make a compicc DEB package
		VERBATIM
		)

