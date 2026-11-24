# tools/jb-mesh: OBJ or glTF to .jbm (docs/tools.md, "`jb-mesh`"). Strict
# C99 against <jitterbug.h> and the format's layout (src/api/formats.h); cgltf
# (third_party/cgltf) is compiled in cgltf_impl.c without the project's
# warning flags, but with -ffp-contract=off, as everything else here.
add_library(jb_mesh_cgltf STATIC cgltf_impl.c)
target_include_directories(jb_mesh_cgltf SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/cgltf)
target_include_directories(jb_mesh_cgltf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(jb_mesh_cgltf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(jb_mesh_cgltf PRIVATE -ffp-contract=off)
endif()

add_executable(jb-mesh jb-mesh.c obj.c gltf.c scene.c)
target_link_libraries(jb-mesh PRIVATE jb_mesh_cgltf jb_tools jb_tools_png)
set_target_properties(jb-mesh PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(jb-mesh PRIVATE -pedantic-errors)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(jb-mesh PRIVATE m)
endif()
# cgltf, and stb_image (jb_tools_png).
jb_install_tool(jb-mesh cgltf stb)
