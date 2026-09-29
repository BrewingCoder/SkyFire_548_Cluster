# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
set(battlegroundserver_sources Main.cpp BattlegroundServer.cpp BattlegroundServer.h)
if(WIN32)
  list(APPEND battlegroundserver_sources ${sources_windows_Debugging} battlegroundserver.rc battlegroundserver.ico)
endif()
add_executable(battlegroundserver ${battlegroundserver_sources})
# JSON's PMR headers also trigger Container auto-linking on MSVC. The bundled
# JSON implementation uses its own memory resource, so neither binary is needed.
target_compile_definitions(battlegroundserver PRIVATE BOOST_JSON_NO_LIB BOOST_JSON_STATIC_LINK BOOST_CONTAINER_NO_LIB)
add_dependencies(battlegroundserver revision.h)
target_include_directories(battlegroundserver PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/src/server/shared
  ${CMAKE_SOURCE_DIR}/src/server/shared/Configuration
  ${CMAKE_SOURCE_DIR}/src/server/shared/Database
  ${CMAKE_SOURCE_DIR}/src/server/shared/Logging
  ${CMAKE_SOURCE_DIR}/src/server/shared/Threading
  ${Boost_INCLUDE_DIRS} ${MYSQL_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
target_link_libraries(battlegroundserver shared ${MYSQL_LIBRARY} ${OPENSSL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
if(TARGET Boost::headers)
  target_link_libraries(battlegroundserver Boost::headers)
elseif(TARGET Boost::boost)
  target_link_libraries(battlegroundserver Boost::boost)
endif()
if(WIN32)
  skyfire_copy_windows_runtime_dependencies(battlegroundserver)
  install(TARGETS battlegroundserver RUNTIME DESTINATION ".")
  install(FILES battlegroundserver.conf.dist DESTINATION ".")
else()
  target_compile_definitions(battlegroundserver PRIVATE _SKYFIRE_BATTLEGROUND_CONFIG="${CONF_DIR}/battlegroundserver.conf")
  install(TARGETS battlegroundserver RUNTIME DESTINATION bin)
  install(FILES battlegroundserver.conf.dist DESTINATION ${CONF_DIR})
endif()
add_custom_command(TARGET battlegroundserver POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/battlegroundserver.conf.dist" "$<TARGET_FILE_DIR:battlegroundserver>/battlegroundserver.conf.dist"
  VERBATIM)
