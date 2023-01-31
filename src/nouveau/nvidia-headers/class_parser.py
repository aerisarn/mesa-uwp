#! /usr/bin/env python3

# script to parse nvidia CL headers and generate inlines to be used in pushbuffer encoding.
# probably needs python3.9

import argparse
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--out_h', required=True, help='Output C header.')
parser.add_argument('--in_h',
                    help='Input class header file.',
                    required=True)
args = parser.parse_args()

filein = args.in_h
fileout = args.out_h

if (filein.strip == ""):
    print("class_parser.py class.h output.h")
    sys.exit()

nvcl = filein.split("/")[-1]
clheader = nvcl
nvcl = nvcl.removeprefix("cl")
nvcl = nvcl.removesuffix(".h")
nvcl = nvcl.upper()

nvcl = "NV" + nvcl

f = open(filein)

fout = open(fileout, 'w')

METHOD_ARRAY_SIZES = {
    'SET_STREAM_OUT_BUFFER_*'   : 4,
    'SET_PIPELINE_*'            : 6,
    'SET_COLOR_TARGET_*'        : 8,
    'SET_COLOR_COMPRESSION'     : 8,
    'SET_CT_WRITE'              : 8,
    'SET_BLEND'                 : 8,
    'SET_BLEND_PER_TARGET_*'    : 8,
    'SET_VIEWPORT_*'            : 16,
    'SET_SCISSOR_*'             : 16,
    'SET_VERTEX_ATTRIBUTE_*'    : 16,
    'SET_VERTEX_STREAM_*'       : 16,
    'BIND_GROUP_CONSTANT_BUFFER': 16,
}

METHOD_IS_FLOAT = [
    'SET_BLEND_CONST_*',
    'SET_DEPTH_BIAS',
    'SET_SLOPE_SCALE_DEPTH_BIAS',
    'SET_DEPTH_BIAS_CLAMP',
    'SET_DEPTH_BOUNDS_M*',
    'SET_LINE_WIDTH_FLOAT',
    'SET_ALIASED_LINE_WIDTH_FLOAT',
    'SET_VIEWPORT_SCALE_*',
    'SET_VIEWPORT_OFFSET_*',
    'SET_VIEWPORT_CLIP_MIN_Z',
    'SET_VIEWPORT_CLIP_MAX_Z',
]

def glob_match(glob, name):
    if glob.endswith('*'):
        return name.startswith(glob[:-1])
    else:
        assert '*' not in glob
        return name == glob

class method(object):
    @property
    def array_size(self):
        for (glob, value) in METHOD_ARRAY_SIZES.items():
            if glob_match(glob, self.name):
                return value
        return 0

    @property
    def is_float(self):
        for glob in METHOD_IS_FLOAT:
            if glob_match(glob, self.name):
                assert len(self.field_defs) == 1
                return True
        return False

# Simple state machine
# state 0 looking for a new method define
# state 1 looking for new fields in a method
# state 2 looking for enums for a fields in a method
# blank lines reset the state machine to 0

state = 0
mthddict = {}
curmthd = {}
for line in f:

    if line.strip() == "":
        state = 0
        if (curmthd):
            if not len(curmthd.field_name_start):
                del mthddict[curmthd.name]
        curmthd = {}
        continue

    if line.startswith("#define"):
        list = line.split();
        if "_cl_" in list[1]:
            continue

        if not list[1].startswith(nvcl):
            continue

        if list[1].endswith("TYPEDEF"):
            continue

        if state == 2:
            teststr = nvcl + "_" + curmthd.name + "_" + curfield + "_"
            if ":" in list[2]:
                state = 1
            elif teststr in list[1]:
                curmthd.field_defs[curfield][list[1].removeprefix(teststr)] = list[2]
            else:
                state = 1

        if state == 1:
            teststr = nvcl + "_" + curmthd.name + "_"
            if teststr in list[1]:
                if ("0x" in list[2]):
                    state = 1
                else:
                    field = list[1].removeprefix(teststr)
                    bitfield = list[2].split(":")
                    curmthd.field_name_start[field] = bitfield[1]
                    curmthd.field_name_end[field] = bitfield[0]
                    curmthd.field_defs[field] = {}
                    curfield = field
                    state = 2
            else:
                if not len(curmthd.field_name_start):
                    del mthddict[curmthd.name]
                    curmthd = {}
                state = 0

        if state == 0:
            if (curmthd):
                if not len(curmthd.field_name_start):
                    del mthddict[curmthd.name]
            teststr = nvcl + "_"
            is_array = 0
            if (':' in list[2]):
                continue
            name = list[1].removeprefix(teststr)
            if name.endswith("(i)"):
                is_array = 1
                name = name.removesuffix("(i)")
            if name.endswith("(j)"):
                is_array = 1
                name = name.removesuffix("(j)")
            x = method()
            x.name = name
            x.addr = list[2]
            x.is_array = is_array
            x.field_name_start = {}
            x.field_name_end = {}
            x.field_defs = {}
            mthddict[x.name] = x

            curmthd = x
            state = 1

sys.stdout = fout
print("/* parsed class " + nvcl + " */")

print("#include \"" + clheader + "\"")
for mthd in mthddict:
    structname = "nv_" + nvcl.lower() + "_" + mthd
    print("struct " + structname + " {")
    for field_name in mthddict[mthd].field_name_start:
        print("\tuint32_t " + field_name.lower() + ";")
    print("};")
    print("static inline void __" + nvcl.strip() + "_" + mthd + "(uint32_t *val_out, struct " + structname + " st" + ") {")
    print("\tuint32_t val = 0;")
    for field_name in mthddict[mthd].field_name_start:
        field_width = int(mthddict[mthd].field_name_end[field_name]) - int(mthddict[mthd].field_name_start[field_name]) + 1
        if (field_width == 32):
            print("\tval |= st." + field_name.lower() + ";")
        else:
            print("\tassert(st." + field_name.lower() + " < (1ULL << " + str(field_width) + "));")
            print("\tval |= (st." + field_name.lower() + " & ((1ULL << " + str(field_width) + ") - 1)) << " + mthddict[mthd].field_name_start[field_name] + ";");
    print("\t*val_out = val;");
    print("}")
    print("#define V_" + nvcl + "_" + mthd + "(val, args...) { \\")
    for field_name in mthddict[mthd].field_name_start:
        if len(mthddict[mthd].field_defs[field_name]):
            for d in mthddict[mthd].field_defs[field_name]:
                print("UNUSED uint32_t " + field_name + "_" + d + " = " + nvcl + "_" + mthd + "_" + field_name + "_" + d+"; \\")
    if len(mthddict[mthd].field_name_start) > 1:
        print("\tstruct " + structname + " __data = args; \\")
    else:
        print("\tstruct " + structname + " __data = { ." + next(iter(mthddict[mthd].field_name_start)).lower() + " = (args) }; \\")
    print("\t__" + nvcl.strip() + "_" + mthd + "(&val, __data); \\")
    print("\t}")
    if mthddict[mthd].is_array:
        print("#define VA_" + nvcl + "_" + mthd + "(i) V_" + nvcl + "_" + mthd)
    else:
        print("#define VA_" + nvcl + "_" + mthd + " V_" + nvcl + "_" + mthd)
    print("#define P_" + nvcl + "_" + mthd + "(push, " + ("idx, " if mthddict[mthd].is_array else "") + "args...) do { \\")
    for field_name in mthddict[mthd].field_name_start:
        if len(mthddict[mthd].field_defs[field_name]):
            for d in mthddict[mthd].field_defs[field_name]:
                print("UNUSED uint32_t " + field_name + "_" + d + " = " + nvcl + "_" + mthd + "_" + field_name + "_" + d+"; \\")
    print("\tuint32_t nvk_p_ret;\\")
    print("\tV_" + nvcl + "_" + mthd + "(nvk_p_ret, args)\\");
    print("\tnvk_push_val(push, " + nvcl + "_" + mthd + ("(idx), " if mthddict[mthd].is_array else ",") + " nvk_p_ret);\\");
    print("\t} while(0)")

print("static inline const char* P_PARSE_" + nvcl + "_MTHD(uint16_t idx) {")
print("\tswitch (idx) {")
for mthd in mthddict:
    if mthddict[mthd].is_array and mthddict[mthd].array_size == 0:
        continue

    if mthddict[mthd].is_array:
        for i in range(mthddict[mthd].array_size):
            print("\tcase " + nvcl + "_" + mthd + "(" + str(i) + "):")
            print("\t\treturn \"" + nvcl + "_" + mthd + "(" + str(i) + ")\";")
    else:
        print("\tcase " + nvcl + "_" + mthd + ":")
        print("\t\treturn \"" + nvcl + "_" + mthd + "\";")
print("\tdefault:")
print("\t\treturn \"unknown method\";")
print("\t};")
print("}")

print("static inline void P_DUMP_" + nvcl + "_MTHD_DATA(uint16_t idx, uint32_t data, const char *prefix) {")
print("\tuint32_t parsed;")
print("\tswitch (idx) {")
for mthd in mthddict:
    if mthddict[mthd].is_array and mthddict[mthd].array_size == 0:
        continue

    if mthddict[mthd].is_array:
        for i in range(mthddict[mthd].array_size):
            print("\tcase " + nvcl + "_" + mthd + "(" + str(i) + "):")
    else:
        print("\tcase " + nvcl + "_" + mthd + ":")
    for field_name in mthddict[mthd].field_name_start:
        field_width = int(mthddict[mthd].field_name_end[field_name]) - int(mthddict[mthd].field_name_start[field_name]) + 1
        if (field_width == 32):
            print("\t\tparsed = data;")
        else:
            print("\t\tparsed = (data >> " + mthddict[mthd].field_name_start[field_name] + ") & ((1u << " + str(field_width) + ") - 1);")
        print("\t\tprintf(\"%s." + field_name + " = \", prefix);")
        if len(mthddict[mthd].field_defs[field_name]):
            print("\t\tswitch (parsed) {")
            for d in mthddict[mthd].field_defs[field_name]:
                print("\t\tcase " + nvcl + "_" + mthd + "_" + field_name + "_" + d + ":")
                print("\t\t\tprintf(\"" + d + "\\n\");")
                print("\t\t\tbreak;")
            print("\t\tdefault:")
            print("\t\t\tprintf(\"0x%x\\n\", parsed);")
            print("\t\t\tbreak;")
            print("\t\t}")
        else:
            if mthddict[mthd].is_float:
                print("\t\t\tprintf(\"%ff (0x%x)\\n\", *(float *)&parsed, parsed);")
            else:
                print("\t\tprintf(\"0x%x\\n\", parsed);")
    print("\t\tbreak;")

print("\tdefault:")
print("\t\tprintf(\"%s.VALUE = 0x%x\\n\", prefix, data);")
print("\t\tbreak;")
print("\t};")
print("}")

fout.close()
f.close()
