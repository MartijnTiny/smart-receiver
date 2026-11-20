# Peripheral drivers live in esp_driver_* components since ESP-IDF 5.3; older
# versions provide them through the umbrella "driver" component.
set(reqs esp_timer)
if(EXISTS "${IDF_PATH}/components/esp_driver_gpio")
    list(APPEND reqs esp_driver_gpio esp_driver_rmt esp_driver_uart)
else()
    list(APPEND reqs driver)
endif()

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES ${reqs})
