# formatting.py
# Sphinx extension providing formatting for Gallium-specific data
# (c) Corbin Simpson 2010
# Public domain to the extent permitted; contact author for special licensing

import docutils.nodes
import sphinx.addnodes

from sphinx.util.nodes import split_explicit_title
from docutils import nodes, utils

def parse_envvar(env, sig, signode):
    envvar, t, default = sig.split(" ", 2)
    envvar = envvar.strip().upper()
    t = "Type: %s" % t.strip(" <>").lower()
    default = "Default: %s" % default.strip(" ()")
    signode += sphinx.addnodes.desc_name(envvar, envvar)
    signode += docutils.nodes.Text(' ')
    signode += sphinx.addnodes.desc_type(t, t)
    signode += docutils.nodes.Text(', ')
    signode += sphinx.addnodes.desc_annotation(default, default)
    return envvar

def parse_opcode(env, sig, signode):
    opcode, desc = sig.split("-", 1)
    opcode = opcode.strip().upper()
    desc = " (%s)" % desc.strip()
    signode += sphinx.addnodes.desc_name(opcode, opcode)
    signode += sphinx.addnodes.desc_annotation(desc, desc)
    return opcode


def ext_role(name, rawtext, text, lineno, inliner, options={}, content=[]):
    text = utils.unescape(text)
    has_explicit_title, title, ext = split_explicit_title(text)

    parts = ext.split('_', 2)
    if parts[0] == 'VK':
        full_url = f'https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/{ext}.html'
    elif parts[0] == 'GL':
        full_url = f'https://registry.khronos.org/OpenGL/extensions/{parts[1]}/{parts[1]}_{parts[2]}.txt'
    else:
        raise Exception(f'Unexpected API: {parts[0]}')

    pnode = nodes.reference(title, title, internal=False, refuri=full_url)
    return [pnode], []

def setup(app):
    app.add_object_type("envvar", "envvar", "%s (environment variable)",
        parse_envvar)
    app.add_object_type("opcode", "opcode", "%s (TGSI opcode)",
        parse_opcode)
    app.add_role('ext', ext_role)
