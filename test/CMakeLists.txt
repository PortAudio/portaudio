# Test projects
# Use the macro to add test projects

add_test(pa_minlat)
add_test(patest1)
add_test(patest_buffer)
add_test(patest_callbackstop)
add_test(patest_clip)
if(LINK_PRIVATE_SYMBOLS)
  add_test(patest_converters)
endif()
add_test(patest_dither)
if(PA_USE_DS)
    add_test(patest_dsound_find_best_latency_params)
    add_test(patest_dsound_low_level_latency_params)
    add_test(patest_dsound_surround)
endif()
add_test(patest_hang)
add_test(patest_in_overflow)
if(PA_USE_WASAPI)
    add_test(patest_jack_wasapi)
    add_test(patest_wasapi_ac3)
    add_test(patest_wasapi_eac3)
    add_test(patest_read_write_wire_wasapi)
    add_test(patest_read_record_wasapi)
endif()
add_test(patest_latency)
add_test(patest_leftright)
add_test(patest_longsine)
add_test(patest_many)
add_test(patest_maxsines)
add_test(patest_mono)
add_test(patest_multi_sine)
add_test(patest_out_underflow)
add_test(patest_prime)
add_test(patest_read_record)
add_test(patest_ringmix)
add_test(patest_sine8)
add_test(patest_sine_channelmaps)
add_test(patest_sine_formats)
add_test(patest_sine_srate)
add_test(patest_sine_time)
add_test(patest_start_stop)
add_test(patest_stop)
add_test(patest_stop_playout)
add_test(patest_suggested_vs_streaminfo_latency)
if(LINK_PRIVATE_SYMBOLS)
  add_test(patest_sync)
endif()
add_test(patest_timing)
add_test(patest_toomanysines)
add_test(patest_two_rates)
add_test(patest_underflow)
add_test(patest_unplug)
add_test(patest_wire)
if(PA_USE_WMME)
    add_test(patest_wmme_find_best_latency_params)
    add_test(patest_wmme_low_level_latency_params)
endif()
add_test(patest_write_stop)
if(UNIX)
    add_test(patest_write_stop_hang_illegal)
endif()
add_test(patest_enumerate_default_latency)
