#!/usr/bin/env python3
# Copyright © 2019, 2022 Intel Corporation
# SPDX-License-Identifier: MIT

from __future__ import annotations
import argparse
import copy
import intel_genxml
import pathlib
import xml.etree.ElementTree as et
import typing


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('files', nargs='*',
                        default=pathlib.Path(__file__).parent.glob('*.xml'),
                        type=pathlib.Path)
    parser.add_argument('--validate', action='store_true')
    parser.add_argument('--quiet', action='store_true')
    args: Args = parser.parse_args()

    for filename in args.files:
        if not args.quiet:
            print('Processing {}... '.format(filename), end='', flush=True)

        xml = et.parse(filename)
        original = copy.deepcopy(xml) if args.validate else xml
        intel_genxml.sort_xml(xml)

        if args.validate:
            for old, new in zip(original.getroot(), xml.getroot()):
                assert intel_genxml.node_validator(old, new), f'{filename} is invalid, run gen_sort_tags.py and commit that'
        else:
            tmp = filename.with_suffix(f'{filename.suffix}.tmp')
            et.indent(xml, space='  ')
            xml.write(tmp, encoding="utf-8", xml_declaration=True)
            tmp.replace(filename)

        if not args.quiet:
            print('done.')


if __name__ == '__main__':
    main()
