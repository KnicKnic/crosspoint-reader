file(GLOB_RECURSE app_sources "*.c" "*.cpp")

set(app_include_dirs ".")
foreach(qrcode_env memory_baseline memory_pm)
  set(qrcode_include "${CMAKE_SOURCE_DIR}/.pio/libdeps/${qrcode_env}/QRCode/src")
  if(EXISTS "${qrcode_include}")
    list(APPEND app_include_dirs "${qrcode_include}")
  endif()
endforeach()

idf_component_register(SRCS ${app_sources} INCLUDE_DIRS ${app_include_dirs})
