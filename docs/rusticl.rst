Rusticl
=======

The easiest way to build Rusticl is by either installing ``rustc``,
``rustfmt`` and ``bindgen`` from your distribution or to simply use
rustup. Just make sure that the ``rustfmt`` and ``bindgen`` components
are installed. Then simply add ``-Dgallium-rusticl=true -Dllvm=enabled
-Drust_std=2021`` to your build options.

Most of the code related to Mesa's C code lives inside ``/mesa``, with
the occasional use of enums, structs or constants through the code base.

If you need help ping ``karolherbst`` either in ``#dri-devel`` or
``#rusticl`` on OFTC.

Also, make sure that before submitting code to verify the formatting is
in order. That can easily be done via ``git ls-files */{lib,app}.rs
| xargs rustfmt``

When submitting Merge Requests or filing bugs related to Rusticl, make
sure to add the ``Rusticl`` label so people subscribed to that Label get
pinged.

Known issues
------------

One issue you might come across is, that the Rust edition meson sets is
not right. This is a known `meson bug
<https://github.com/mesonbuild/meson/issues/10664>`__ and in order to
fix it, simply run ``meson configure $your_build_dir -Drust_std=2021``
