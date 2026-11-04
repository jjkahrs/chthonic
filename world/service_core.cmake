# The code every Chthonic C++ service shares, included by world/CMakeLists.txt and chat/CMakeLists.txt:
#   chthonic_protocol      frame format + messages (protocol/); no Asio/DB, reusable by a game client
#   chthonic_service_core  config/log helpers, chat text rules and mutes, TLS sessions, JWT/JWKS verification, MySQL and
#                      the migration runner (server/core, server/net, server/auth, server/db/mysql + migrate)
# The including project finds the packages first (asio, OpenSSL, unofficial-libmysql, jwt-cpp, httplib, nlohmann_json,
# quill); CHTHONIC_SERVICE_COMPILE_OPTIONS / _DEFINITIONS are set here for its own targets too.
set(CHTHONIC_SERVICE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(MSVC)
  set(CHTHONIC_SERVICE_COMPILE_OPTIONS /W4 /WX /permissive- /utf-8 /bigobj)
  set(CHTHONIC_SERVICE_DEFINITIONS _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  set(CHTHONIC_SERVICE_COMPILE_OPTIONS -Wall -Wextra -Werror)
  set(CHTHONIC_SERVICE_DEFINITIONS)
endif()
list(APPEND CHTHONIC_SERVICE_DEFINITIONS ASIO_NO_DEPRECATED CPPHTTPLIB_OPENSSL_SUPPORT JWT_DISABLE_PICOJSON)

add_library(chthonic_protocol STATIC "${CHTHONIC_SERVICE_DIR}/protocol/codec.cpp")
target_include_directories(chthonic_protocol PUBLIC "${CHTHONIC_SERVICE_DIR}/protocol")
target_compile_options(chthonic_protocol PRIVATE ${CHTHONIC_SERVICE_COMPILE_OPTIONS})

add_library(chthonic_service_core STATIC
  "${CHTHONIC_SERVICE_DIR}/server/core/chat_rules.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/core/env.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/core/log.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/core/utc.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/db/migrate.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/db/mysql.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/auth/jwks_cache.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/auth/jwt_verify.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/auth/token_service.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/net/session.cpp"
  "${CHTHONIC_SERVICE_DIR}/server/net/tls_context.cpp"
)
# Included as <core/log.h>, <net/session.h>, ...
target_include_directories(chthonic_service_core PUBLIC "${CHTHONIC_SERVICE_DIR}/server")
target_compile_options(chthonic_service_core PRIVATE ${CHTHONIC_SERVICE_COMPILE_OPTIONS})
target_compile_definitions(chthonic_service_core PUBLIC ${CHTHONIC_SERVICE_DEFINITIONS})
target_link_libraries(chthonic_service_core PUBLIC chthonic_protocol asio::asio OpenSSL::SSL OpenSSL::Crypto
  unofficial::libmysql::libmysql jwt-cpp::jwt-cpp httplib::httplib nlohmann_json::nlohmann_json PRIVATE quill::quill)
