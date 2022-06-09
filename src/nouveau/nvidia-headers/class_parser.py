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

class method(object):
    pass

# Simple state machine
# state 0 looking for a new method define
# state 1 looking for new fields in a method
# state 2 looking for enums for a fields in a method
# blank lines reset the state machine to 0

state = 0
mthddict = {}
for line in f:

    if line.strip() == "":
        state = 0
        curmthd = {}
        continue

    if line.startswith("#define"):
        list = line.split();
        if "_cl_" in list[1]:
            continue

        if not list[1].startswith(nvcl):
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
                state = 0

        if state == 0:
            teststr = nvcl + "_"
            is_array = 0
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
    print("#define P_" + nvcl + "_" + mthd + "(push, " + ("idx, " if mthddict[mthd].is_array else "") + "args...) do { \\")
    for field_name in mthddict[mthd].field_name_start:
        if len(mthddict[mthd].field_defs[field_name]):
            for d in mthddict[mthd].field_defs[field_name]:
                print("UNUSED uint32_t " + field_name + "_" + d + " = " + nvcl + "_" + mthd + "_" + field_name + "_" + d+"; \\")
    print("\tuint32_t nvk_p_ret;\\")
    print("\tV_" + nvcl + "_" + mthd + "(nvk_p_ret, args)\\");
    print("\tnvk_push_val(push, " + nvcl + "_" + mthd + ("(idx), " if mthddict[mthd].is_array else ",") + " nvk_p_ret);\\");
    print("\t} while(0)")

fout.close()
f.close()
