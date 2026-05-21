# Claude Buddy: Claude desktop companion for the GeekMagic Pro hardware.
# ESP-IDF v6.0.1. Boilerplate must stay in this order for cmake to work.
cmake_minimum_required(VERSION 3.22)

# Mirrors main/app_version.h. Both values are shown on /debug; ESP-IDF
# uses PROJECT_VER for the esp_app_desc header that the OTA image carries.
# Update both together when cutting a release.
set(PROJECT_VER "v0.3.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ClaudeBuddy)
