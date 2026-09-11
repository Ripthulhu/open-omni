cmake_minimum_required(VERSION 3.22)
project(omni_mcu1_core VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_library(omni_core STATIC src/volume.c src/uac2_control.c src/boot_ack.c)
target_include_directories(omni_core PUBLIC include)
target_compile_options(omni_core PRIVATE -Wall -Wextra -Werror -Wconversion -ffunction-sections -fdata-sections)
if(NOT CMAKE_CROSSCOMPILING)
  include(cmake/host-tests.cmake)
endif()
if(CMAKE_CROSSCOMPILING)
  enable_language(ASM)
  set(OMNI_VENDOR "" CACHE PATH "Pinned SDK checkout directory")
  set(OMNI_BUILD_ID "omni-a-dev" CACHE STRING "Diagnostic build identification")
  if(NOT EXISTS "${OMNI_VENDOR}/usb/device/usb_device_dci.c")
    message(FATAL_ERROR "Set OMNI_VENDOR to the pinned dependency directory")
  endif()
  set(V ${OMNI_VENDOR})
  set(D ${V}/devices/LPC5500/LPC55S69/drivers)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_usb_dci.py ${V}/usb/device/usb_device_lpcip3511.c
    ${V}/usb/device/usb_device_lpcip3511.h ${V}/usb/device/usb_device_dci.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_usb_class.py ${V}/usb/device/usb_device_ch9.c
    ${V}/usb/device/class/usb_device_class.c ${V}/usb/device/class/usb_device_hid.c)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_usb_dci.py
    ${V}/usb/device/usb_device_lpcip3511.c ${CMAKE_CURRENT_BINARY_DIR}/usb_device_lpcip3511_omni.c
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_usb_class.py
    ${V}/usb/device ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND_ERROR_IS_FATAL ANY)

  file(READ ${V}/usb/config/device/ip3511fs/usb_device_config.h USB_CONFIG)
  string(REPLACE "#define USB_DEVICE_CONFIG_SELF_POWER (1U)" "#define USB_DEVICE_CONFIG_SELF_POWER (0U)" USB_CONFIG "${USB_CONFIG}")
  # LPC55 USB0 FS has five logical endpoints (UM11126 ch.40); use EP4 for the UAC2
  # async feedback IN endpoint. The SDK default caps at four.
  string(REPLACE "#define USB_DEVICE_CONFIG_ENDPOINTS (4U)" "#define USB_DEVICE_CONFIG_ENDPOINTS (6U)" USB_CONFIG "${USB_CONFIG}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/usb_device_config.h "${USB_CONFIG}")
  add_executable(omni_mcu1.elf src/startup.S src/board.c src/diagnostic_usb.c
    src/microphone.c src/settings_menu.c src/eq_menu.c src/settings_menu_native.c src/mixer_control.c src/mixer.c src/mixer_ui.c src/source_mix.c src/home_ui.c src/eq_response.c src/ui.c src/display.c src/display_lpc5528.c src/rotary.c src/usb_guard.c src/controls.c src/control_action.c
    src/mcu2_link.c src/mcu2_runtime.c src/mcu2_controls.c src/mcu2_uart_lpc5528.c src/mcu2_probe.c
    src/control_uart.c src/dsp_link.c src/dsp_probe.c src/interchip.c
    src/dsp_capture.c src/dsp_capture_probe.c src/audio_clock_snapshot.c
    src/audio_clock.c src/audio_clock_lpc5528.c src/audio_mode.c src/usb_audio_ring.c
    src/audio_queue.c src/audio_rate.c src/sof_capture.c
    src/usb_iso_banks.c src/usb_iso_lpc5528.c src/dsp_volume.c src/dsp_gain_trial.c src/dsp_volume_probe.c
    src/native_gain.c src/native_gain_curve.c src/native_gain_adapter.c src/headset_gain.c src/headset_volume.c src/headset_battery.c src/headset_query.c src/dsp_meter.c src/dsp_settings.c src/remote_menu.c
    src/charger.c src/charger_adc.c src/charger_policy.c
    src/audio_probe.c src/audio_descriptors.c src/volume.c src/volume_scale.c src/uac2_control.c
    src/boot_ack.c src/boot_ack_lpc5528.c
    ${V}/core/drivers/iap1/fsl_iap.c
    ${D}/fsl_clock.c ${D}/fsl_reset.c ${D}/fsl_power.c
    ${V}/core/drivers/common/fsl_common.c
    ${V}/core/drivers/common/fsl_common_arm.c
    ${CMAKE_CURRENT_BINARY_DIR}/usb_device_dci_omni.c
    ${CMAKE_CURRENT_BINARY_DIR}/usb_device_ch9_omni.c
    ${CMAKE_CURRENT_BINARY_DIR}/usb_device_lpcip3511_omni.c
    ${CMAKE_CURRENT_BINARY_DIR}/usb_device_class_omni.c
    ${CMAKE_CURRENT_BINARY_DIR}/usb_device_hid_omni.c
    ${V}/legacy-sdk/components/osa/fsl_os_abstraction_bm.c
    ${V}/legacy-sdk/components/lists/fsl_component_generic_list.c)
  target_include_directories(omni_mcu1.elf PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR} include
    ${V}/devices/LPC5500/LPC5528 ${V}/devices/LPC5500/periph ${D}
    ${V}/cmsis/CMSIS/Core/Include ${V}/core/drivers/common ${V}/core/drivers/iap1
    ${V}/usb/include ${V}/usb/device ${V}/usb/device/class
    ${V}/legacy-sdk/components/osa ${V}/legacy-sdk/components/lists)
  target_compile_definitions(omni_mcu1.elf PRIVATE CPU_LPC5528JBD100 USB_DEVICE_CONFIG_HID=1
    USB_DEVICE_CONFIG_AUDIO=1
    SDK_DEBUGCONSOLE=0 NDEBUG OMNI_BUILD_ID="${OMNI_BUILD_ID}")
  target_compile_options(omni_mcu1.elf PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
  # Recompile the integrated display/input drivers with stricter warning gates.
  add_library(omni_ui_bringup OBJECT src/display.c src/display_lpc5528.c src/rotary.c src/controls.c src/control_action.c)
  target_include_directories(omni_ui_bringup PRIVATE
    $<TARGET_PROPERTY:omni_mcu1.elf,INCLUDE_DIRECTORIES>)
  target_compile_definitions(omni_ui_bringup PRIVATE CPU_LPC5528JBD100 SDK_DEBUGCONSOLE=0 NDEBUG)
  target_compile_options(omni_ui_bringup PRIVATE -Wall -Wextra -Werror -Wconversion)
  # Standalone protocol compile gate; runtime startup belongs to the main target.
  add_library(omni_interchip_bringup OBJECT src/interchip.c src/audio_bus.c src/audio_mode.c src/audio_clock.c src/audio_clock_lpc5528.c)
  target_include_directories(omni_interchip_bringup PRIVATE include)
  target_compile_options(omni_interchip_bringup PRIVATE -Wall -Wextra -Werror -Wconversion)
  target_link_options(omni_mcu1.elf PRIVATE -nostartfiles --specs=nosys.specs
    -T${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcu1.ld
    -Wl,--gc-sections,-Map=${CMAKE_CURRENT_BINARY_DIR}/omni_mcu1.map)
  set_property(TARGET omni_mcu1.elf APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcu1.ld)
endif()
