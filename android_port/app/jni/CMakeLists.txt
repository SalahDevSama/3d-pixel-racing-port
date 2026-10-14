cmake_minimum_required(VERSION 3.18)
project(pixel_racing_android C CXX)

# Shared sources with the Linux port
set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/../../../linux_port)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------- third-party libraries
add_subdirectory(${TP}/SDL ${CMAKE_BINARY_DIR}/SDL)

set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2TTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${TP}/SDL_ttf ${CMAKE_BINARY_DIR}/SDL_ttf)

set(LIBTYPE STATIC CACHE STRING "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OBOE OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/openal-soft ${CMAKE_BINARY_DIR}/openal-soft)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

# headers are included as <SDL2/...>
set(INC ${CMAKE_BINARY_DIR}/inc)
file(GLOB SDL_HEADERS ${TP}/SDL/include/*.h)
file(COPY ${SDL_HEADERS} ${TP}/SDL_ttf/SDL_ttf.h DESTINATION ${INC}/SDL2)

# ---------------------------------------------------------------- recompiled game code
set(GEN ${CMAKE_BINARY_DIR}/gen)
set(GEN_SRCS ${GEN}/dispatch.c)
foreach(i RANGE 0 15)
  if(i LESS 10)
    list(APPEND GEN_SRCS ${GEN}/code_0${i}.c)
  else()
    list(APPEND GEN_SRCS ${GEN}/code_${i}.c)
  endif()
endforeach()
add_custom_command(
  OUTPUT ${GEN_SRCS} ${GEN}/funcs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND Python3::Interpreter -I ${PORT}/tools/extract_slice.py "${GAME_APP}/3d_pixel_racing" ${GEN}/armv6.macho
  COMMAND Python3::Interpreter -I ${PORT}/tools/recomp.py ${GEN}/armv6.macho ${GEN}
  DEPENDS ${PORT}/tools/recomp.py ${PORT}/tools/macho.py ${PORT}/tools/extract_slice.py
  COMMENT "Recompiling ARM code to C")

set(RT ${PORT}/runtime)
add_library(main SHARED
  ${RT}/mem.c ${RT}/objc.c ${RT}/hle.c ${RT}/gl.c ${RT}/al.c ${RT}/image.c ${RT}/hooks.c
  ${RT}/settings.c ${RT}/input.c ${RT}/overlay.c ${RT}/interp.c ${RT}/hostgl.c ${RT}/text.c ${RT}/main.c
  ${GEN_SRCS})
target_include_directories(main PRIVATE ${RT} ${GEN} ${INC} ${TP}/openal-soft/include)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -fwrapv
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
target_link_libraries(main PRIVATE SDL2 SDL2_ttf OpenAL GLESv1_CM EGL z log android m)
