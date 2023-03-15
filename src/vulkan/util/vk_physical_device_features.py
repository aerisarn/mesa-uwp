COPYRIGHT=u"""
/* Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
"""

import argparse
from collections import OrderedDict
import os
import sys
import typing
import xml.etree.ElementTree as et

import mako
from mako.template import Template
from vk_extensions import get_all_required, filter_api

class FeatureStruct(typing.NamedTuple):
    c_type: str
    s_type: str
    features: list[str]

TEMPLATE_C = Template(COPYRIGHT + """
/* This file generated from ${filename}, don't edit directly. */

#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_util.h"

static VkResult
check_physical_device_features(struct vk_physical_device *physical_device,
                               const VkPhysicalDeviceFeatures *supported,
                               const VkPhysicalDeviceFeatures *enabled,
                               const char *struct_name)
{
% for flag in pdev_features:
   if (enabled->${flag} && !supported->${flag})
      return vk_errorf(physical_device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "%s.%s not supported", struct_name, "${flag}");
% endfor

   return VK_SUCCESS;
}

VkResult
vk_physical_device_check_device_features(struct vk_physical_device *physical_device,
                                         const VkDeviceCreateInfo *pCreateInfo)
{
   VkPhysicalDevice vk_physical_device =
      vk_physical_device_to_handle(physical_device);

   /* Query the device what kind of features are supported. */
   VkPhysicalDeviceFeatures2 supported_features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
   };

% for f in feature_structs:
   ${f.c_type} supported_${f.c_type} = { .pNext = NULL };
% endfor

   vk_foreach_struct_const(features, pCreateInfo->pNext) {
      VkBaseOutStructure *supported = NULL;
      switch (features->sType) {
% for f in feature_structs:
      case ${f.s_type}:
         supported = (VkBaseOutStructure *) &supported_${f.c_type};
         break;
% endfor
      default:
         break;
      }

      /* Not a feature struct. */
      if (!supported)
         continue;

      /* Check for cycles in the list */
      if (supported->pNext != NULL || supported->sType != 0)
         return VK_ERROR_UNKNOWN;

      supported->sType = features->sType;
      __vk_append_struct(&supported_features2, supported);
   }

   physical_device->dispatch_table.GetPhysicalDeviceFeatures2(
      vk_physical_device, &supported_features2);

   if (pCreateInfo->pEnabledFeatures) {
      VkResult result =
        check_physical_device_features(physical_device,
                                       &supported_features2.features,
                                       pCreateInfo->pEnabledFeatures,
                                       "VkPhysicalDeviceFeatures");
      if (result != VK_SUCCESS)
         return result;
   }

   /* Iterate through additional feature structs */
   vk_foreach_struct_const(features, pCreateInfo->pNext) {
      /* Check each feature boolean for given structure. */
      switch (features->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
         const VkPhysicalDeviceFeatures2 *features2 = (const void *)features;
         VkResult result =
            check_physical_device_features(physical_device,
                                           &supported_features2.features,
                                           &features2->features,
                                           "VkPhysicalDeviceFeatures2.features");
         if (result != VK_SUCCESS)
            return result;
        break;
      }
% for f in feature_structs:
      case ${f.s_type} : {
         const ${f.c_type} *a = &supported_${f.c_type};
         const ${f.c_type} *b = (const void *) features;
% for flag in f.features:
         if (b->${flag} && !a->${flag})
            return vk_errorf(physical_device, VK_ERROR_FEATURE_NOT_PRESENT,
                             "%s.%s not supported", "${f.c_type}", "${flag}");
% endfor
         break;
      }
% endfor
      default:
         break;
      }
   } // for each extension structure
   return VK_SUCCESS;
}

""", output_encoding='utf-8')

def get_pdev_features(doc):
    _type = doc.find(".types/type[@name='VkPhysicalDeviceFeatures']")
    if _type is not None:
        flags = []
        for p in _type.findall('./member'):
            assert p.find('./type').text == 'VkBool32'
            flags.append(p.find('./name').text)
        return flags
    return None

def filter_api(elem, api):
    if 'api' not in elem.attrib:
        return True

    return api in elem.attrib['api'].split(',')

def get_feature_structs(doc, api):
    feature_structs = OrderedDict()

    required = get_all_required(doc, 'type', api)

    # parse all struct types where structextends VkPhysicalDeviceFeatures2
    for _type in doc.findall('./types/type[@category="struct"]'):
        if _type.attrib.get('structextends') != 'VkPhysicalDeviceFeatures2,VkDeviceCreateInfo':
            continue
        if _type.attrib['name'] not in required:
            continue

        # find Vulkan structure type
        for elem in _type:
            if "STRUCTURE_TYPE" in str(elem.attrib):
                s_type = elem.attrib.get('values')

        # collect a list of feature flags
        flags = []

        for p in _type.findall('./member'):
            if not filter_api(p, api):
                continue

            m_name = p.find('./name').text
            if m_name == 'pNext':
                pass
            elif m_name == 'sType':
                s_type = p.attrib.get('values')
            else:
                assert p.find('./type').text == 'VkBool32'
                flags.append(m_name)

        feature_struct = FeatureStruct(c_type=_type.attrib.get('name'), s_type=s_type, features=flags)
        feature_structs[feature_struct.c_type] = feature_struct

    return feature_structs.values()

def get_feature_structs_from_xml(xml_files, api='vulkan'):
    pdev_features = None
    feature_structs = []

    for filename in xml_files:
        doc = et.parse(filename)
        feature_structs += get_feature_structs(doc, api)
        if not pdev_features:
            pdev_features = get_pdev_features(doc)

    return pdev_features, feature_structs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', required=True, help='Output C file.')
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True, action='append', dest='xml_files')
    args = parser.parse_args()

    pdev_features, feature_structs = get_feature_structs_from_xml(args.xml_files)

    environment = {
        'filename': os.path.basename(__file__),
        'pdev_features': pdev_features,
        'feature_structs': feature_structs,
    }

    try:
        with open(args.out_c, 'wb') as f:
            f.write(TEMPLATE_C.render(**environment))
    except Exception:
        # In the event there's an error, this uses some helpers from mako
        # to print a useful stack trace and prints it, then exits with
        # status 1, if python is run with debug; otherwise it just raises
        # the exception
        print(mako.exceptions.text_error_template().render(), file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
