cmake_minimum_required(VERSION 3.16)
project(pixel_racing C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(OPENAL REQUIRED openal)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)
pkg_check_modules(ZLIB REQUIRED zlib)
find_package(OpenGL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GAME_APP "${CMAKE_SOURCE_DIR}/../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app" CACHE PATH "Path to the extracted 3d_pixel_racing.app")
set(GEN ${CMAKE_BINARY_DIR}/gen)
set(NCODE 16)
set(GEN_SRCS ${GEN}/dispatch.c)
foreach(i RANGE 0 15)
  if(i LESS 10)
    list(APPEND GEN_SRCS ${GEN}/code_0${i}.c)
  else()
    list(APPEND GEN_SRCS ${GEN}/code_${i}.c)
  endif()
endforeach()

# extract the armv6 slice and recompile it to C
add_custom_command(
  OUTPUT ${GEN_SRCS} ${GEN}/funcs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/extract_slice.py "${GAME_APP}/3d_pixel_racing" ${GEN}/armv6.macho
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/recomp.py ${GEN}/armv6.macho ${GEN}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/recomp.py ${CMAKE_SOURCE_DIR}/tools/macho.py ${CMAKE_SOURCE_DIR}/tools/extract_slice.py
  COMMENT "Recompiling ARM code to C")

add_executable(pixel_racing
  runtime/mem.c runtime/objc.c runtime/hle.c runtime/gl.c runtime/al.c runtime/image.c runtime/hooks.c runtime/settings.c runtime/input.c runtime/overlay.c runtime/interp.c runtime/hostgl.c runtime/text.c runtime/main.c
  ${GEN_SRCS})
target_include_directories(pixel_racing PRIVATE runtime ${GEN} ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS} ${OPENAL_INCLUDE_DIRS})
target_link_libraries(pixel_racing PRIVATE ${SDL2_LIBRARIES} ${SDL2TTF_LIBRARIES} ${OPENAL_LIBRARIES} ${ZLIB_LIBRARIES} OpenGL::GL m)
target_compile_options(pixel_racing PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -fwrapv
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
