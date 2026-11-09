cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(tweaks C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu11 -nostdlib")

add_executable(tweaks main.c regroup.c store.c)

# Only ever loaded into the Settings app, so plain (strong) stubs are fine.
target_link_libraries(tweaks
  taihen_stub
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceSysmem_stub
  gcc
)

vita_create_self(tweaks.suprx tweaks CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/tweaks.yml)
