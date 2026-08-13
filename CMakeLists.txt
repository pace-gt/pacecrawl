cmake_minimum_required(VERSION 3.24)

project(pacecrawl)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

add_subdirectory(gtl gtl)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenMP REQUIRED)

pkg_check_modules(ACL REQUIRED libacl)

add_library(mdb liblmdb/midl.c liblmdb/mdb.c)
target_include_directories(mdb PUBLIC liblmdb)

add_executable(pacecrawl pacecrawl.cpp pacecrawl_fts/pacecrawl_fts.c)
target_include_directories(pacecrawl PUBLIC liblmdb)
target_compile_features(pacecrawl PUBLIC cxx_std_14)
target_include_directories(pacecrawl PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl PUBLIC "${ACL_LINK_LIBRARIES}" OpenSSL::Crypto mdb OpenMP::OpenMP_CXX)

add_library(lmdb-misc STATIC lmdb-misc.c)
target_link_libraries(lmdb-misc PUBLIC mdb)

add_executable(pacecrawl-diff pacecrawl-diff.cpp)
target_compile_features(pacecrawl-diff PUBLIC cxx_std_20)
target_include_directories(pacecrawl-diff PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-diff PUBLIC "${ACL_LINK_LIBRARIES}" OpenSSL::Crypto OpenMP::OpenMP_CXX lmdb-misc mdb gtl)

add_executable(pacecrawl-applyperms pacecrawl-applyperms.c)
target_compile_features(pacecrawl-applyperms PUBLIC cxx_std_14)
target_include_directories(pacecrawl-applyperms PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-applyperms PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-getdirs pacecrawl-getdirs.c)
target_include_directories(pacecrawl-getdirs PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-getdirs PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-dirsummaries pacecrawl-dirsummaries.cpp)
target_include_directories(pacecrawl-dirsummaries PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-dirsummaries PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-allpaths pacecrawl-allpaths.c)
target_include_directories(pacecrawl-allpaths PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-allpaths PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-timetable pacecrawl-timetable.cpp)
target_include_directories(pacecrawl-timetable PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-timetable PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-spacediff pacecrawl-spacediff.cpp)
target_include_directories(pacecrawl-spacediff PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-spacediff PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-stats pacecrawl-stats.cpp)
target_include_directories(pacecrawl-stats PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-stats PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-stats2 pacecrawl-stats2.cpp)
target_include_directories(pacecrawl-stats2 PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-stats2 PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-findpath pacecrawl-findpath.c)
target_include_directories(pacecrawl-findpath PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-findpath PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

add_executable(pacecrawl-findinode pacecrawl-findinode.c)
target_include_directories(pacecrawl-findinode PUBLIC "${ACL_INCLUDE_DIRS}")
target_link_libraries(pacecrawl-findinode PUBLIC OpenMP::OpenMP_C "${ACL_LINK_LIBRARIES}" lmdb-misc mdb)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/opt/pacecrawl" CACHE PATH "..." FORCE)
endif()

include(GNUInstallDirs)

install(TARGETS
    pacecrawl
    pacecrawl-diff
    pacecrawl-applyperms
    pacecrawl-getdirs
    pacecrawl-dirsummaries
    pacecrawl-timetable
    pacecrawl-spacediff
    pacecrawl-stats
    pacecrawl-stats2
    pacecrawl-findpath
    pacecrawl-findinode
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${CMAKE_SOURCE_DIR}/README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT Documentation)

# --- Packaging Logic ---

set(CPACK_PACKAGE_NAME "pacecrawl")
set(CPACK_PACKAGE_VENDOR "PACE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tools to crawl file trees and verify transfers")
set(CPACK_PACKAGE_VERSION "4.0.0")
set(CPACK_PACKAGE_RELEASE 1)
set(CPACK_PACKAGE_CONTACT "alambert48@gatech.edu")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

set(CPACK_GENERATOR "RPM")
set(CPACK_RPM_PACKAGE_LICENSE "GPLv3")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_AUTOREQ "YES")

# This line must be last
include(CPack)
