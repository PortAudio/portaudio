add_executable(paloopback
  audio_analyzer.c
  biquad_filter.c
  paqa.c
  paqa_tools.c
  test_audio_analyzer.c
  write_wav.c
)
target_include_directories(paloopback PRIVATE ..)
target_link_libraries(paloopback portaudio)
