# Copyright © 2022 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT
nvk_files = files(
  'nvk_acceleration_structure.c',
  'nvk_buffer.c',
  'nvk_buffer.h',
  'nvk_buffer_view.c',
  'nvk_buffer_view.h',
  'nvk_cmd_buffer.c',
  'nvk_cmd_buffer.h',
  'nvk_cmd_clear.c',
  'nvk_cmd_copy.c',
  'nvk_cmd_dispatch.c',
  'nvk_cmd_draw.c',
  'nvk_cmd_indirect.c',
  'nvk_cmd_meta.c',
  'nvk_cmd_pool.c',
  'nvk_cmd_pool.h',
  'nvk_debug.h',
  'nvk_descriptor_set.h',
  'nvk_descriptor_set.c',
  'nvk_descriptor_set_layout.c',
  'nvk_descriptor_set_layout.h',
  'nvk_descriptor_table.c',
  'nvk_descriptor_table.h',
  'nvk_descriptor_types.h',
  'nvk_device.c',
  'nvk_device.h',
  'nvk_device_memory.c',
  'nvk_device_memory.h',
  'nvk_edb_bview_cache.c',
  'nvk_edb_bview_cache.h',
  'nvk_event.c',
  'nvk_event.h',
  'nvk_format.c',
  'nvk_format.h',
  'nvk_heap.c',
  'nvk_heap.h',
  'nvk_host_copy.c',
  'nvk_image.c',
  'nvk_image.h',
  'nvk_image_view.c',
  'nvk_image_view.h',
  'nvk_indirect_execution_set.c',
  'nvk_indirect_execution_set.h',
  'nvk_instance.c',
  'nvk_instance.h',
  'nvk_mem_arena.c',
  'nvk_mem_arena.h',
  'nvk_mem_stream.c',
  'nvk_mem_stream.h',
  'nvk_mme.c',
  'nvk_mme.h',
  'nvk_nir_lower_descriptors.c',
  'nvk_physical_device.c',
  'nvk_physical_device.h',
  'nvk_private.h',
  'nvk_query_pool.c',
  'nvk_query_pool.h',
  'nvk_queue.c',
  'nvk_queue.h',
  'nvk_sampler.c',
  'nvk_sampler.h',
  'nvk_shader.c',
  'nvk_shader.h',
  'nvk_upload_queue.c',
  'nvk_upload_queue.h',
  'nvk_wsi.c',
  'nvk_wsi.h',
  'layers/nvk_app_workarounds.c',
  'layers/nvk_app_workarounds.h',
  'nvkmd/nvkmd.c',
  'nvkmd/nvkmd.h',
)

if with_platform_switch
  nvk_loaderless_vk = custom_target(
    'nvk_loaderless_vk',
    input : [files('nvk_loaderless_vk_gen.py'), vk_api_xml],
    output : 'nvk_loaderless_vk.c',
    command : [
      prog_python, '@INPUT0@', '--xml', '@INPUT1@',
      '--out-c', '@OUTPUT@', '--beta', with_vulkan_beta.to_string(),
    ],
    depend_files : vk_entrypoints_gen_depend_files,
  )

  nvk_files += [nvk_loaderless_vk]
  nvk_files += files(
    'nvk_switch_wsi.h',
    'rust_switch_stubs.c',
    'nvkmd/switch/nvkmd_switch.h',
    'nvkmd/switch/nvkmd_switch_dev.c',
    'nvkmd/switch/nvkmd_switch_pdev.c',
  )
else
  nvk_files += files(
    'nvkmd/nouveau/nvkmd_nouveau.h',
    'nvkmd/nouveau/nvkmd_nouveau_ctx.c',
    'nvkmd/nouveau/nvkmd_nouveau_dev.c',
    'nvkmd/nouveau/nvkmd_nouveau_mem.c',
    'nvkmd/nouveau/nvkmd_nouveau_pdev.c',
    'nvkmd/nouveau/nvkmd_nouveau_va.c',
  )
endif

nvkcl_files = files(
  'cl/nvk_query.cl',
  'cl/nvk_copy_indirect.cl',
)

nvk_entrypoints = custom_target(
  'nvk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['nvk_entrypoints.h', 'nvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'nvk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

nvkcl_spv = custom_target(
  input : [nvkcl_files, libnvidia_headers],
  output : 'nvkcl.spv',
  command : [
    prog_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', nvkcl_files, '--',
    '-I' + join_paths(meson.project_source_root(), 'src/compiler/libcl'),
    '-I' + join_paths(meson.current_source_dir(), '.'),
    '-I' + join_paths(meson.project_source_root(), 'src'),
    '-I' + join_paths(meson.project_source_root(), 'src/nouveau/headers'),
    '-I' + join_paths(meson.project_source_root(), 'src/nouveau/headers/nvidia/classes'),
    '-I' + join_paths(meson.project_build_root(), 'src/nouveau/headers'),
    cl_args,
    ],
  depfile : 'nvkcl.spv.d',
)

nvkcl = custom_target(
  input : nvkcl_spv,
  output : ['nvkcl.cpp', 'nvkcl.h'],
  command : [prog_vtn_bindgen2, '@INPUT@', '@OUTPUT@'],
)

nvk_deps = [
  dep_libdrm,
  idep_nak,
  idep_nil,
  idep_nir,
  idep_mesautil,
  idep_nouveau_mme,
  idep_nouveau_ws,
  idep_nvidia_headers,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
]

nvk_flags = []

if with_platform_android
  nvk_files += files('nvk_android.c')
endif

libnvk = static_library(
  'nvk',
  [
    nvk_entrypoints,
    cl_generated,
    nvk_files,
    nvkcl,
    sha1_h,
  ],
  include_directories : [
    inc_include,
    inc_src,
  ],
  dependencies : nvk_deps,
  c_args : [no_override_init_args, nvk_flags],
  gnu_symbol_visibility : 'hidden',
  install : host_machine.system() == 'horizon',
)

if host_machine.system() == 'horizon'
  # Switch has no Vulkan loader and no shared-library runtime: ld pulls in
  # crt0/libnx that don't compose with -shared, and there's nothing to dlopen
  # the result anyway. Expose libnvk as a static archive so homebrew code can
  # link it directly. The custom_targets below still emit an ICD JSON that
  # points at libvulkan_nouveau.<suffix>; on Switch we just keep the static .a
  # at that name for any tooling that wants it.
  libvulkan_nouveau = libnvk

  libvulkan_private_archives = [
    libnvk,
    libvulkan_lite_runtime,
    libvulkan_runtime,
    libvulkan_lite_instance,
    libvulkan_instance,
    libvulkan_util,
    libvulkan_wsi,
    _libnak,
    _libnak_rs,
    libvtn,
    _libxmlconfig,
    _libnil,
    _libnil_format_table,
    _libnouveau_mme,
    libnouveau_ws,
    libnvidia_headers,
    _libnir,
    libcompiler,
    _libcompiler_c,
    _libmesa_util,
    libmesa_util_simd,
    blake3,
    _libmesa_util_c11,
  ]

  libvulkan_private_link_dirs = []
  foreach archive : libvulkan_private_archives
    archive_dir = fs.parent(archive.full_path())
    if not libvulkan_private_link_dirs.contains(archive_dir)
      libvulkan_private_link_dirs += archive_dir
    endif
  endforeach

  libvulkan_private_link_args = []
  foreach archive_dir : libvulkan_private_link_dirs
    libvulkan_private_link_args += '-L' + archive_dir
  endforeach

  # Preserve the original archive-group semantics behind a single
  # client-facing libvulkan.a so Horizon consumers don't need bespoke link
  # order or a manually curated archive list.
  libvulkan = custom_target(
    'nvk_loaderless_libvulkan',
    input : libvulkan_private_archives,
    output : 'libvulkan.a',
    command : [
      prog_python, files('nvk_linker_script_gen.py'),
      '--out', '@OUTPUT@',
      '@INPUT@',
    ],
    depend_files : files('nvk_linker_script_gen.py'),
    build_by_default : true,
    install : true,
    install_dir : get_option('libdir'),
  )

  dep_vulkan_horizon = declare_dependency(
    include_directories : inc_include,
    dependencies : [
      dep_libdrm,
      dep_zlib,
      dep_zstd,
    ],
    link_args : libvulkan_private_link_args + [libvulkan.full_path(), '-lnx', '-lm', '-lstdc++'],
  )

  meson.override_dependency('vulkan', dep_vulkan_horizon)

  pkg.generate(
    name : 'vulkan',
    filebase : 'vulkan',
    description : 'Mesa Vulkan static archive for Horizon',
    version : meson.project_version(),
    libraries : ['-lvulkan'],
    libraries_private : ['-lnx', '-lm', '-lstdc++'],
    requires_private : ['libdrm_nouveau', 'zlib', 'libzstd'],
  )
else
  libvulkan_nouveau = shared_library(
    'vulkan_nouveau',
    link_whole : [libnvk],
    link_args: [ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],
    gnu_symbol_visibility : 'hidden',
    install : true,
  )
endif

icd_file_name = libname_prefix + 'vulkan_nouveau.' + libname_suffix

nouveau_icd = custom_target(
  'nouveau_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'nouveau_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'nouveau_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'nouveau_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

if with_tests and not with_platform_android
  test(
    'nvk_mme',
    executable(
      'test_nvk_mme',
      files('test_nvk_mme.c'),
      dependencies : [nvk_deps],
      link_with : [libnvk],
    ),
    suite : ['nouveau'],
  )
endif
