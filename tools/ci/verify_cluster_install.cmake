# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information

if(NOT IS_DIRECTORY "${INSTALL_ROOT}")
  message(FATAL_ERROR "INSTALL_ROOT must name an installed clustering package")
endif()
if(PACKAGE_PLATFORM STREQUAL "windows")
  set(runtime "${INSTALL_ROOT}")
  set(config "${INSTALL_ROOT}")
  set(suffix ".exe")
elseif(PACKAGE_PLATFORM STREQUAL "linux")
  set(runtime "${INSTALL_ROOT}/bin")
  set(config "${INSTALL_ROOT}/etc")
  set(suffix "")
else()
  message(FATAL_ERROR "PACKAGE_PLATFORM must be windows or linux")
endif()

set(required)
foreach(service authserver worldserver hubserver chatserver battlegroundserver)
  list(APPEND required "${runtime}/${service}${suffix}" "${config}/${service}.conf.dist")
endforeach()
foreach(asset
    web/index.html web/app.js web/status.js web/metrics.js
    control/controlserver.py control/control.toml.dist control/requirements.txt
    backup/backupserver.py backup/backup.toml.dist backup/requirements.txt
    mapserver/mapserver.py mapserver/map_common.py mapserver/fetch_maps.py
    mapserver/mapserver.toml.dist mapserver/requirements.txt
    mapserver/hub_service.py mapserver/cluster_certificates.py
    characterserver/characterserver.py characterserver/database.py
    characterserver/social_store.py characterserver/social_projection.py
    characterserver/character_state.py characterserver/statements.json
    characterserver/characterserver.toml.dist characterserver/requirements.txt
    characterserver/hub_service.py characterserver/cluster_certificates.py
    sql/base/hub_database.sql)
  list(APPEND required "${runtime}/${asset}")
endforeach()
foreach(path IN LISTS required)
  if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "Clustering INSTALL package is missing: ${path}")
  endif()
  file(SIZE "${path}" size)
  if(size EQUAL 0)
    message(FATAL_ERROR "Clustering INSTALL package has an empty file: ${path}")
  endif()
endforeach()
if(NOT IS_DIRECTORY "${runtime}/sql/updates/hub" OR
   NOT IS_DIRECTORY "${runtime}/sql/pending_updates/hub")
  message(FATAL_ERROR "Clustering INSTALL package is missing the hub SQL update directories")
endif()
message(STATUS "Clustering INSTALL package verified: ${INSTALL_ROOT}")
