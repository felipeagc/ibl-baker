project('ibl_baker', 'c', 'cpp')

cc = meson.get_compiler('c')

sources = [
  'src/main.c',
  'src/vk_mem_alloc.cpp'
]

deps = [
  dependency('vulkan'),
  cc.find_library('m', required : false)
]

executable(
  'ibl_baker',
  sources,
  include_directories: include_directories('src'),
  dependencies: deps)
