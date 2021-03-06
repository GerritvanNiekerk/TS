# Copyright (C) 2010 Ion Torrent Systems, Inc. All Rights Reserved
cmake_minimum_required (VERSION 2.6)
project (ion-pipeline)
set(PROJECT_DESCRIPTION "Ion Torrent Pipeline")
enable_testing()

include(../buildTools/cmake/CMakeLists.version.txt)
include(../buildTools/cmake/CMakeLists.compiler.txt)

FIND_PACKAGE(PythonInterp)
# Determine python site-packages or dist-packages directory
execute_process (
        COMMAND "${PYTHON_EXECUTABLE}" -c "from distutils.sysconfig import get_python_lib; print get_python_lib()"
        OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process (
        COMMAND "${PYTHON_EXECUTABLE}" -c "from distutils.sysconfig import get_python_lib; print get_python_lib(prefix='/usr/local')"
        OUTPUT_VARIABLE PYTHON_LOCAL_SITE_PACKAGES
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process (
        COMMAND "${PYTHON_EXECUTABLE}" -c "from distutils.sysconfig import get_python_version; print get_python_version()"
        OUTPUT_VARIABLE PYTHON_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
)


# Where to install - override with: -DION_INSTALL_PREFIX
set(ION_INSTALL_PREFIX "/usr" CACHE PATH "Ion Install Prefix")
set(CMAKE_INSTALL_PREFIX "${ION_INSTALL_PREFIX}" CACHE INTERNAL "Prefix prepended to install directories" FORCE)
set(CPACK_PACKAGING_INSTALL_PREFIX ${ION_INSTALL_PREFIX})
set(CPACK_SET_DESTDIR "ON")


add_custom_target(compileall ALL
    COMMAND ${PYTHON_EXECUTABLE} -m compileall -q -f "${PROJECT_SOURCE_DIR}/python/"
)

install(FILES "${PROJECT_SOURCE_DIR}/oia/oia.config" DESTINATION /usr/share/ion/oia)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/oia/oiad.py" DESTINATION /usr/share/ion/oia)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/oia/ecc.py" DESTINATION /usr/share/ion/oia)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/oia/oia" DESTINATION /etc/init.d)

install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion-plugin-status" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_netinfo" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/remote_host.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_sysinfo" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_raidinfo" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_gpuinfo" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_timeout.sh" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/ion_versionCheck.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/BlockTLScript.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/MergeTLScript.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/pluginReporting.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/bin/alignmentQC.py" DESTINATION bin)
install(PROGRAMS  "${PROJECT_SOURCE_DIR}/java/FlowspaceCalibration.jar" DESTINATION local/lib/java)

install(DIRECTORY web DESTINATION share/ion
    USE_SOURCE_PERMISSIONS
    PATTERN ".svn" EXCLUDE
)

install(DIRECTORY python/ion DESTINATION ${PYTHON_SITE_PACKAGES}
    USE_SOURCE_PERMISSIONS
    PATTERN ".svn" EXCLUDE
)

install(FILES "${PROJECT_SOURCE_DIR}/cluster_settings.py" DESTINATION /etc/torrentserver )
# We are copying an existing __init__.py, they are all the same.
install(FILES "${PROJECT_SOURCE_DIR}/python/ion/__init__.py" DESTINATION /etc/torrentserver )




include(InstallRequiredSystemLibraries)
set(CPACK_GENERATOR "DEB")

include(../buildTools/cmake/CMakeLists.cpack.txt)

#% nice to automate this
set(CPACK_DEBIAN_PACKAGE_DEPENDS "python,
    python-matplotlib,
    python-scipy,
    python-django,
    python-guppy,
    python-imaging,
    python-argparse,
    python-dev")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "python-markdown,
    python-pygments")
set(CPACK_DEBIAN_PACKAGE_BREAKS "ion-dbreports (<= 2.1.6)")
set(CPACK_DEBIAN_PACKAGE_REPLACES "ion-dbreports (<= 2.1.6)")
set(CPACK_PACKAGE_DESCRIPTION "This package contains the Torrent Suite pipeline.")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
	${CMAKE_CURRENT_BINARY_DIR}/debian/postinst
	${CMAKE_CURRENT_BINARY_DIR}/debian/prerm
	${CMAKE_CURRENT_BINARY_DIR}/debian/postrm
)
include(CPack)
