# The server engine: event loop + coroutines, HTTP, and the Postgres driver.

find_package(OpenSSL REQUIRED)

add_library(rhs STATIC
    # core/
    core/base64.c
    core/bytebuf.c

    # runtime/
    runtime/event_loop.c
    runtime/scheduler.c

    # http/
    http/headers_parser.c
    http/request_handler.c
    http/response_handler.c
    http/router.c

    # pg/
    pg/pool.c
    pg/query.c
    pg/startup.c
    pg/wire.c
    pg/auth/auth.c
    pg/auth/auth_helpers.c
    pg/auth/scram-sha-256/mechanism.c

    ${PROJECT_SOURCE_DIR}/utils/utils.c
    ${PROJECT_SOURCE_DIR}/env_parser/env.c
)

# All includes are written relative to the project root,
# e.g. #include "server/core/bytebuf.h", so the root is the include directory.
# PUBLIC so that app/ gets it too.
target_include_directories(rhs PUBLIC ${PROJECT_SOURCE_DIR})

# SCRAM-SHA-256 needs libcrypto (EVP / HMAC / SHA256 / PBKDF2).
target_link_libraries(rhs
    PUBLIC OpenSSL::Crypto
    PRIVATE rhs_build_options
)
