#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

COPYRIGHT = """\
/*
 * Copyright 2026 Mesa-Switch contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import os
import sys

from mako.template import Template

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
VK_UTIL_DIR = os.path.normpath(os.path.join(THIS_DIR, '..', '..', 'vulkan', 'util'))
if VK_UTIL_DIR not in sys.path:
    sys.path.insert(0, VK_UTIL_DIR)

# Mesa-local imports must be declared in meson variable
# '{file_without_suffix}_depend_files'.
from vk_entrypoints import get_entrypoints_from_xml

TEMPLATE_C = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#include <assert.h>
#include <string.h>

#include "util/macros.h"
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_object.h"
#include "vk_physical_device.h"

/* Switch links NVK directly into the final NRO, so expose plain vk* symbols
 * that forward into the same dispatch/runtime machinery the ICD path uses.
 */
extern VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkInstance *pInstance);

extern VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                         uint32_t *pPropertyCount,
                                         VkExtensionProperties *pProperties);

extern VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceVersion(uint32_t *pApiVersion);

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_GetInstanceProcAddr(VkInstance instance, const char *pName);

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_common_GetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR VkResult VKAPI_CALL
nvk_loaderless_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                VkLayerProperties *pProperties)
{
   if (pPropertyCount == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   *pPropertyCount = 0;
   return VK_SUCCESS;
}

% for e in entrypoints:
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
PUBLIC VKAPI_ATTR ${e.return_type} VKAPI_CALL
vk${e.name}(${e.decl_params() if e.decl_params() else 'void'});
  % if e.guard is not None:
#endif /* ${e.guard} */
  % endif

% endfor

static PFN_vkVoidFunction
nvk_loaderless_lookup_public_proc(const char *pName)
{
   if (pName == NULL)
      return NULL;

% for e in entrypoints:
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
   if (strcmp(pName, "vk${e.name}") == 0)
      return (PFN_vkVoidFunction)vk${e.name};
  % if e.guard is not None:
#endif /* ${e.guard} */
  % endif

% endfor
   return NULL;
}

% for e in base_entrypoints:
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
PUBLIC VKAPI_ATTR ${e.return_type} VKAPI_CALL
vk${e.name}(${e.decl_params() if e.decl_params() else 'void'})
{
  % if e.name == 'GetInstanceProcAddr':
   PFN_vkVoidFunction public_func = nvk_loaderless_lookup_public_proc(pName);

   if (pName == NULL)
      return NULL;

   if (public_func == NULL)
      return NULL;

   if (strcmp(pName, "vkGetInstanceProcAddr") == 0 ||
       strcmp(pName, "vkGetDeviceProcAddr") == 0 ||
       strcmp(pName, "vkCreateInstance") == 0 ||
       strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0 ||
       strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0 ||
       strcmp(pName, "vkEnumerateInstanceVersion") == 0)
      return public_func;

   if (instance == VK_NULL_HANDLE)
      return NULL;

   if (nvk_GetInstanceProcAddr(instance, pName) == NULL)
      return NULL;

   return public_func;
  % elif e.name == 'GetDeviceProcAddr':
   PFN_vkVoidFunction public_func = nvk_loaderless_lookup_public_proc(pName);

   if (pName == NULL)
      return NULL;
   if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
      return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
   if (device == VK_NULL_HANDLE)
      return NULL;
   if (public_func == NULL)
      return NULL;

   if (vk_common_GetDeviceProcAddr(device, pName) == NULL)
      return NULL;

   return public_func;
  % elif not e.params or e.params[0].type not in dispatchable_types:
    % if e.name == 'CreateInstance':
   return nvk_CreateInstance(${e.call_params()});
    % elif e.name == 'EnumerateInstanceExtensionProperties':
   return nvk_EnumerateInstanceExtensionProperties(${e.call_params()});
    % elif e.name == 'EnumerateInstanceVersion':
   return nvk_EnumerateInstanceVersion(${e.call_params()});
    % elif e.name == 'EnumerateInstanceLayerProperties':
   return nvk_loaderless_EnumerateInstanceLayerProperties(${e.call_params()});
    % else:
   assert(!"Unhandled global Vulkan wrapper");
      % if e.return_type != 'void':
   return (${e.return_type})0;
      % endif
    % endif
  % elif e.params[0].type == 'VkInstance':
   VK_FROM_HANDLE(vk_instance, vk_instance, ${e.params[0].name});
    % if e.name == 'DestroyInstance':
   if (vk_instance == NULL)
      return;
    % else:
   assert(vk_instance != NULL);
    % endif
    % if e.return_type == 'void':
   vk_instance->dispatch_table.${e.name}(${e.call_params()});
    % else:
   return vk_instance->dispatch_table.${e.name}(${e.call_params()});
    % endif
  % elif e.params[0].type == 'VkPhysicalDevice':
   VK_FROM_HANDLE(vk_physical_device, vk_physical_device, ${e.params[0].name});
   assert(vk_physical_device != NULL);
    % if e.return_type == 'void':
   vk_physical_device->dispatch_table.${e.name}(${e.call_params()});
    % else:
   return vk_physical_device->dispatch_table.${e.name}(${e.call_params()});
    % endif
  % elif e.params[0].type == 'VkDevice':
   VK_FROM_HANDLE(vk_device, vk_device, ${e.params[0].name});
    % if e.name == 'DestroyDevice':
   if (vk_device == NULL)
      return;
    % else:
   assert(vk_device != NULL);
    % endif
    % if e.return_type == 'void':
   vk_device->dispatch_table.${e.name}(${e.call_params()});
    % else:
   return vk_device->dispatch_table.${e.name}(${e.call_params()});
    % endif
  % elif e.params[0].type in ('VkCommandBuffer', 'VkQueue'):
   struct vk_object_base *vk_object = (struct vk_object_base *)${e.params[0].name};
   assert(vk_object != NULL);
    % if e.return_type == 'void':
   vk_object->device->dispatch_table.${e.name}(${e.call_params()});
    % else:
   return vk_object->device->dispatch_table.${e.name}(${e.call_params()});
    % endif
  % else:
   assert(!"Unhandled Vulkan wrapper dispatch case");
    % if e.return_type != 'void':
   return (${e.return_type})0;
    % endif
  % endif
}
  % if e.guard is not None:
#endif /* ${e.guard} */
  % endif

% endfor
% for e in alias_entrypoints:
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
PUBLIC VKAPI_ATTR ${e.return_type} VKAPI_CALL
vk${e.name}(${e.decl_params() if e.decl_params() else 'void'})
{
  % if e.return_type == 'void':
   vk${e.alias.name}(${e.call_params()});
  % else:
   return vk${e.alias.name}(${e.call_params()});
  % endif
}
  % if e.guard is not None:
#endif /* ${e.guard} */
  % endif

% endfor
""")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', required=True, help='Output C file.')
    parser.add_argument('--beta', required=True, help='Enable beta extensions.')
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    entrypoints = list(get_entrypoints_from_xml(args.xml_files, args.beta))
    base_entrypoints = [e for e in entrypoints if e.alias is None]
    alias_entrypoints = [e for e in entrypoints if e.alias is not None]
    dispatchable_types = ('VkInstance', 'VkPhysicalDevice', 'VkDevice',
                          'VkCommandBuffer', 'VkQueue')

    try:
        with open(args.out_c, 'w', encoding='utf-8') as f:
            f.write(TEMPLATE_C.render(
                alias_entrypoints=alias_entrypoints,
                base_entrypoints=base_entrypoints,
                dispatchable_types=dispatchable_types,
                entrypoints=entrypoints,
                filename=os.path.basename(__file__),
            ))
    except Exception:
        if __debug__:
            import sys as _sys
            from mako import exceptions
            _sys.stderr.write(exceptions.text_error_template().render() + '\n')
            _sys.exit(1)
        raise


if __name__ == '__main__':
    main()
