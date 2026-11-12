# portable core as an IDF component. tools/preview builds the same files on PC with MSVC
# the 5" boards and the 7" (config.h GFX_SCALED) have the same fonts half as big again, and one more
if(RADAR_DISPLAY EQUAL 10 OR RADAR_DISPLAY EQUAL 11 OR RADAR_DISPLAY EQUAL 12 OR RADAR_DISPLAY EQUAL 14 OR RADAR_DISPLAY EQUAL 15 OR RADAR_DISPLAY EQUAL 16)
    set(FONTS "font_glass_small.c" "font_glass_medium.c" "font_glass_large.c" "font_glass_huge.c" "font_glass_tiny.c")
else()
    set(FONTS "font_small.c" "font_medium.c" "font_large.c" "font_huge.c")
endif()
idf_component_register(SRCS "sky.c" "demo.c" "gfx.c" "font.c" "ui.c" "ui_scope.c" "ui_pages.c" "ui_wifi.c" "paper.c"
                            "ui_about.c" "qr_site.c" "map.c" "png_decode.c" "watch.c" "light.c"
                            ${FONTS}
                       INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)
