add_test(paqa_errs)
add_test(paqa_devs)
if(LINK_PRIVATE_SYMBOLS)
  add_test(paqa_dither)
endif()
add_test(paqa_latency)

subdirs(loopback)
