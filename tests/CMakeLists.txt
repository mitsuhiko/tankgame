# Tank Game - Test Suite

# Smoke tests
add_executable(test_smoke test_smoke.c)
target_include_directories(test_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME smoke COMMAND test_smoke)

# Memory system tests
add_executable(test_mem test_mem.c ../src/core/pz_mem.c)
target_include_directories(test_mem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_mem PRIVATE PZ_DEBUG)  # Always test with debug tracking
add_test(NAME mem COMMAND test_mem)

# Math library tests
add_executable(test_math test_math.c ../src/core/pz_math.c)
target_include_directories(test_math PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_math PRIVATE m)  # Link math library
add_test(NAME math COMMAND test_math)

# Collision primitive tests
add_executable(test_collision test_collision.c ../src/game/pz_collision.c)
target_include_directories(test_collision PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_collision PRIVATE m)
add_test(NAME collision COMMAND test_collision)

# Data structures tests
add_executable(test_ds test_ds.c ../src/core/pz_ds.c ../src/core/pz_mem.c)
target_include_directories(test_ds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_ds PRIVATE PZ_DEBUG)
add_test(NAME ds COMMAND test_ds)

# String utility tests
add_executable(test_str test_str.c ../src/core/pz_str.c ../src/core/pz_mem.c)
target_include_directories(test_str PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_str PRIVATE PZ_DEBUG)
add_test(NAME str COMMAND test_str)

# Platform layer tests
add_executable(test_platform test_platform.c ../src/core/pz_platform.c ../src/core/pz_mem.c)
target_include_directories(test_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_platform PRIVATE PZ_DEBUG)
add_test(NAME platform COMMAND test_platform)

# Renderer tests (using null backend - no OpenGL needed)
add_executable(test_renderer test_renderer.c
    ../src/engine/render/pz_renderer.c
    ../src/engine/render/pz_render_null.c
    ../src/core/pz_mem.c
    ../src/core/pz_log.c
    ../src/core/pz_math.c
    ../src/core/pz_platform.c
    ../third_party/stb_image_write_impl.c
)
target_include_directories(test_renderer PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(test_renderer PRIVATE PZ_DEBUG)
target_link_libraries(test_renderer PRIVATE m)
add_test(NAME renderer COMMAND test_renderer)

# Map system tests (includes tile registry and texture system)
add_executable(test_map test_map.c
    ../src/game/pz_map.c
    ../src/game/pz_toxic_cloud.c
    ../src/game/pz_tile_registry.c
    ../src/engine/render/pz_texture.c
    ../src/engine/render/pz_renderer.c
    ../src/engine/render/pz_render_null.c
    ../src/core/pz_ds.c
    ../src/core/pz_mem.c
    ../src/core/pz_log.c
    ../src/core/pz_str.c
    ../src/core/pz_math.c
    ../src/core/pz_platform.c
    ../third_party/stb_image_impl.c
    ../third_party/stb_image_write_impl.c
)
target_include_directories(test_map PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(test_map PRIVATE PZ_DEBUG)
target_link_libraries(test_map PRIVATE m)
add_test(NAME map COMMAND test_map)

# Simulation system tests
add_executable(test_sim test_sim.c
    ../src/core/pz_sim.c
    ../src/core/pz_mem.c
)
target_include_directories(test_sim PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_sim PRIVATE PZ_DEBUG)
add_test(NAME sim COMMAND test_sim)

# Toxic cloud tests
add_executable(test_toxic_cloud test_toxic_cloud.c
    ../src/game/pz_toxic_cloud.c
    ../src/core/pz_mem.c
)
target_include_directories(test_toxic_cloud PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_toxic_cloud PRIVATE PZ_DEBUG)
target_link_libraries(test_toxic_cloud PRIVATE m)
add_test(NAME toxic_cloud COMMAND test_toxic_cloud)

# Networking offer URL tests
add_executable(test_net_offer test_net_offer.c
    ../src/net/pz_net.c
    ../src/core/pz_mem.c
    ../src/core/pz_str.c
)
target_include_directories(test_net_offer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_net_offer PRIVATE PZ_DEBUG)
add_test(NAME net_offer COMMAND test_net_offer)
