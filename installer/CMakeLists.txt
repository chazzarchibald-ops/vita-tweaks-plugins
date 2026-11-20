cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(tweaks_installer C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Tweaks/Plugins Installer")
set(VITA_TITLEID "TWKS00001")
set(VITA_VERSION "01.00")

# The tweaks.suprx the VPK carries: the plugin's own build, next door.
set(TWEAKS_SUPRX "${CMAKE_CURRENT_SOURCE_DIR}/../build/tweaks.suprx" CACHE FILEPATH "tweaks.suprx to bundle")
if(NOT EXISTS "${TWEAKS_SUPRX}")
  message(FATAL_ERROR "No tweaks.suprx at ${TWEAKS_SUPRX}: build Tweaks/Plugins first, or pass -DTWEAKS_SUPRX=<path>")
endif()

# The Tweaks/Plugins version this installer carries: what "Check for a new
# version" compares the latest release on GitHub with.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../tweaks.h" TWEAKS_VERSION_LINE REGEX "#define TWEAKS_VERSION ")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" TWEAKS_VERSION "${TWEAKS_VERSION_LINE}")
if(NOT TWEAKS_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
  message(FATAL_ERROR "No TWEAKS_VERSION in ../tweaks.h")
endif()

# For trying the update on a console before a release is the latest one, and
# never for a release: -DUPDATE_TEST_TAG=1.1 -DUPDATE_TEST_HAVE=1.0 makes an
# installer that says it carries Tweaks/Plugins 1.0 and asks GitHub for the
# 1.1 release by its tag, which finds a pre-release too. Its screen says
# "Update test".
set(UPDATE_TEST_TAG "" CACHE STRING "Test build: the release tag to ask GitHub for")
set(UPDATE_TEST_HAVE "" CACHE STRING "Test build: the Tweaks/Plugins version to say it carries")
if(UPDATE_TEST_TAG AND UPDATE_TEST_HAVE)
  set(TWEAKS_VERSION "${UPDATE_TEST_HAVE}")
endif()

add_executable(tweaks_installer main.c gfx.c liquid.c files.c net.c json.c sha256.c sha1.c headbin.c package.c unzip.c
               update.c projects.c lookup.c png.c live.c)
target_compile_definitions(tweaks_installer PRIVATE TWEAKS_VERSION="${TWEAKS_VERSION}")
if(UPDATE_TEST_TAG)
  target_compile_definitions(tweaks_installer PRIVATE UPDATE_TEST_TAG="${UPDATE_TEST_TAG}")
endif()
# -O3: the liquid is drawn on the CPU.
target_compile_options(tweaks_installer PRIVATE -O3 -Wall)
target_link_libraries(tweaks_installer
  SceDisplay_stub
  SceCtrl_stub
  SceSysmodule_stub
  ScePower_stub
  ScePvf_stub
  SceIofilemgr_stub
  SceSysmem_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  ScePromoterUtil_stub
  m
)

# The updater's param.sfo. To update itself the installer installs this same
# eboot.bin under a second title ID, and that app puts the new version in
# (update.c). Its package header is made on the console from the installer's own.
set(UPDATER_SFO "${CMAKE_CURRENT_BINARY_DIR}/updater_param.sfo")
add_custom_command(OUTPUT "${UPDATER_SFO}"
  COMMAND ${VITA_MKSFOEX} -s APP_VER=01.00 -s TITLE_ID=TWKS00002 "Tweaks/Plugins Updater" "${UPDATER_SFO}"
  COMMENT "Generating the updater's param.sfo"
  VERBATIM
)
add_custom_target(updater_sfo ALL DEPENDS "${UPDATER_SFO}")

# UNSAFE: the installer writes to ur0:tai and ur0:data.
vita_create_self(eboot.bin tweaks_installer UNSAFE)
vita_create_vpk(tweaks_installer.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${TWEAKS_SUPRX} tweaks.suprx
  FILE ${UPDATER_SFO} updater/param.sfo
  # the other projects' own bubble pictures, for their emblems
  FILE icons/engram_48.rgb icons/engram_48.rgb
  FILE icons/engram_34.rgb icons/engram_34.rgb
  FILE icons/cyberflow_48.rgb icons/cyberflow_48.rgb
  FILE icons/cyberflow_34.rgb icons/cyberflow_34.rgb
  # the fonts the app's text is in (SIL Open Font License: the licence goes with them)
  FILE font/Poppins-Regular.ttf font/Poppins-Regular.ttf
  FILE font/OFL-Poppins.txt font/OFL-Poppins.txt
  FILE font/Righteous-Regular.ttf font/Righteous-Regular.ttf
  FILE font/OFL-Righteous.txt font/OFL-Righteous.txt
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
