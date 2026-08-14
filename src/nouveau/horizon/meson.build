dep_nouveau_horizon_nx = cc.find_library('nx', required : true)

libnouveau_horizon = static_library(
  'nouveau_horizon',
  files(
    'nouveau_horizon.h',
    'nouveau_horizon_private.h',
    'nouveau_horizon_gm20b.h',
    'nouveau_horizon_runtime.c',
    'nouveau_horizon_device.c',
    'nouveau_horizon_memory.c',
    'nouveau_horizon_vm.c',
    'nouveau_horizon_channel.c',
    'nouveau_horizon_gm20b.c',
  ),
  include_directories : [
    inc_include, inc_src,
    include_directories('../headers', '../headers/nvidia/classes'),
  ],
  dependencies : [idep_mesautil, dep_nouveau_horizon_nx],
  gnu_symbol_visibility : 'hidden',
  install : host_machine.system() == 'horizon',
)

idep_nouveau_horizon = declare_dependency(
  link_with : libnouveau_horizon,
  include_directories : include_directories('.'),
  dependencies : [idep_mesautil, dep_nouveau_horizon_nx],
)
