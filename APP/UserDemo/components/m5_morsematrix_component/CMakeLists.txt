idf_component_register(SRCS "m5_morsematrix_wifi.cpp" "m5_morsematrix_led_map.cpp" "m5_morsematrix_display.cpp" "m5_morsematrix_ir.cpp" "m5_morsematrix_button.cpp" "m5_morsematrix_component.cpp"
                           
                            "otool_ir_nec/ir_nec_encoder.c"
                            "otool_ir_nec/ir_nec_tools.cpp"
                            "otool_ir_nec/rs_toolkit/rs_encoder.cpp"
                            "otool_ir_nec/rs_toolkit/rs_decoder.cpp"
                            "otool_ir_nec/rs_toolkit/rs_true_codec.cpp"
                            "font_matrix/m5_morsematrix_fm_ascii_5x8.cpp"
                            "font_matrix/m5_morsematrix_fm_normal_8x8.cpp"
                       INCLUDE_DIRS "." 
                                    "otool_ir_nec"
                                    "otool_ir_nec/include"
                                    "otool_ir_nec/rs_toolkit"
                                    "font_matrix"
                        REQUIRES driver esp_timer esp_wifi nvs_flash cjson)

# RS Encoding Mode Configuration
# Choose ONE of the following modes:
#
# RS_MODE_TRUE_CORRECTION: TRUE Reed-Solomon(255,223) [RECOMMENDED]
#   - Can CORRECT up to 16 byte errors (50% data loss tolerance)
#   - Slightly larger overhead (32 bytes)
#   - Ideal for: Noisy IR environment, long distance, many obstacles
#
# RS_MODE_PARITY_CHECK: Simple parity check (legacy)
#   - Can only DETECT errors, cannot correct
#   - Lower overhead (16 bytes)
#   - Ideal for: Clean environment, short distance
#
# Uncomment ONE line to select mode:
target_compile_definitions(${COMPONENT_LIB} PRIVATE RS_MODE_TRUE_CORRECTION)
# target_compile_definitions(${COMPONENT_LIB} PRIVATE RS_MODE_PARITY_CHECK)
