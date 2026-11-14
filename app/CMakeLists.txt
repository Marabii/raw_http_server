# The URL shortener: business logic on top of the `rhs` library.
# The target is still called http_server so the Dockerfile and launch.json keep working.

add_executable(http_server
    main.c
    shortner.c
    health.c
)

target_link_libraries(http_server PRIVATE
    rhs
    rhs_build_options
)
