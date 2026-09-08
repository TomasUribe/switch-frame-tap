cmake_minimum_required(VERSION 3.16)
project(switch-stream-recv C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AV   REQUIRED libavcodec libavutil)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(USB  REQUIRED libusb-1.0)

add_executable(switch-stream-recv
  src/main.c
  src/source.c
  src/decode.c
  src/render.c)

target_include_directories(switch-stream-recv PRIVATE
  ${AV_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS} ${USB_INCLUDE_DIRS})
target_link_directories(switch-stream-recv PRIVATE
  ${AV_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS} ${USB_LIBRARY_DIRS})
target_link_libraries(switch-stream-recv PRIVATE
  ${AV_LIBRARIES} ${SDL2_LIBRARIES} ${USB_LIBRARIES})
target_compile_options(switch-stream-recv PRIVATE
  ${AV_CFLAGS_OTHER} ${SDL2_CFLAGS_OTHER} ${USB_CFLAGS_OTHER}
  -Wall -Wextra)
