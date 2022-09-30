#!/usr/bin/env python3
# Copyright © 2019, 2022 Intel Corporation
# SPDX-License-Identifier: MIT

from __future__ import annotations
from collections import OrderedDict
import pathlib
import re
import xml.etree.ElementTree as et
import typing

if typing.TYPE_CHECKING:
    import os

def get_filename(element: et.Element) -> str:
    return element.attrib['filename']

def get_name(element: et.Element) -> str:
    return element.attrib['name']

def get_value(element: et.Element) -> int:
    return int(element.attrib['value'], 0)

def get_start(element: et.Element) -> int:
    return int(element.attrib['start'], 0)


BASE_TYPES = {
    'address',
    'offset',
    'int',
    'uint',
    'bool',
    'float',
}

FIXED_PATTERN = re.compile(r"(s|u)(\d+)\.(\d+)")

def is_base_type(name: str) -> bool:
    return name in BASE_TYPES or FIXED_PATTERN.match(name) is not None

def add_struct_refs(items: typing.OrderedDict[str, bool], node: et.Element) -> None:
    if node.tag == 'field':
        if 'type' in node.attrib and not is_base_type(node.attrib['type']):
            t = node.attrib['type']
            items[t] = True
        return
    if node.tag not in {'struct', 'group'}:
        return
    for c in node:
        add_struct_refs(items, c)


class Struct(object):
    def __init__(self, xml: et.Element):
        self.xml = xml
        self.name = xml.attrib['name']
        self.deps: typing.OrderedDict[str, Struct] = OrderedDict()

    def find_deps(self, struct_dict, enum_dict) -> None:
        deps: typing.OrderedDict[str, bool] = OrderedDict()
        add_struct_refs(deps, self.xml)
        for d in deps.keys():
            if d in struct_dict:
                self.deps[d] = struct_dict[d]
            else:
                assert d in enum_dict

    def add_xml(self, items: typing.OrderedDict[str, et.Element]) -> None:
        for d in self.deps.values():
            d.add_xml(items)
        items[self.name] = self.xml


# ordering of the various tag attributes
genxml_desc = {
    'genxml'      : [ 'name', 'gen', ],
    'enum'        : [ 'name', 'value', 'prefix', ],
    'struct'      : [ 'name', 'length', ],
    'field'       : [ 'name', 'start', 'end', 'type', 'default', 'prefix', ],
    'instruction' : [ 'name', 'bias', 'length', 'engine', ],
    'value'       : [ 'name', 'value', ],
    'group'       : [ 'count', 'start', 'size', ],
    'register'    : [ 'name', 'length', 'num', ],
}

space_delta = 2

def print_node(f: typing.TextIO, offset: int, node: et.Element) -> None:
    if node.tag in [ 'enum', 'struct', 'instruction', 'register' ]:
        f.write('\n')
    spaces = ''.rjust(offset * space_delta)
    f.write('{0}<{1}'.format(spaces, node.tag))
    attribs = genxml_desc[node.tag]
    for a in node.attrib:
        assert a in attribs 
    for a in attribs:
        if a in node.attrib:
            f.write(' {0}="{1}"'.format(a, node.attrib[a]))
    children = list(node)
    if len(children) > 0:
        f.write('>\n')
        for c in children:
            print_node(f, offset + 1, c)
        f.write('{0}</{1}>\n'.format(spaces, node.tag))
    else:
        f.write('/>\n')


def process(filename: os.PathLike[str]) -> None:
    xml = et.parse(filename)
    genxml = xml.getroot()

    enums = sorted(genxml.findall('enum'), key=get_name)
    enum_dict: typing.Dict[str, et.Element] = {}
    for e in enums:
        values = e.findall('./value')
        e[:] = sorted(e, key=get_value)
        enum_dict[e.attrib['name']] = e

    # Structs are a bit annoying because they can refer to each other. We sort
    # them alphabetically and then build a graph of dependencies. Finally we go
    # through the alphabetically sorted list and print out dependencies first.
    structs = sorted(xml.findall('./struct'), key=get_name)
    wrapped_struct_dict: typing.Dict[str, Struct] = {}
    for s in structs:
        s[:] = sorted(s, key=get_start)
        ws = Struct(s)
        wrapped_struct_dict[ws.name] = ws

    for ws in wrapped_struct_dict.values():
        ws.find_deps(wrapped_struct_dict, enum_dict)

    sorted_structs: typing.OrderedDict[str, et.Element] = OrderedDict()
    for s in structs:
        _s = wrapped_struct_dict[s.attrib['name']]
        _s.add_xml(sorted_structs)

    instructions = sorted(xml.findall('./instruction'), key=get_name)
    for i in instructions:
        i[:] = sorted(i, key=get_start)

    registers = sorted(xml.findall('./register'), key=get_name)
    for r in registers:
        r[:] = sorted(r, key=get_start)

    genxml[:] = enums + list(sorted_structs.values()) + instructions + registers

    with open(filename, 'w') as f:
        f.write('<?xml version="1.0" ?>\n')
        print_node(f, 0, genxml)


if __name__ == '__main__':
    folder = pathlib.Path('.')
    for f in folder.glob('*.xml'):
        print('Processing {}... '.format(f), end='', flush=True)
        process(f)
        print('done.')
