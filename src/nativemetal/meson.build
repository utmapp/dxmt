winemetal_src = [
  '../winemetal/winemetal_thunks.c',
  '../winemetal/unix/winemetal_unix.c',
  '../winemetal/unix/cache.c',
]
winemetal_link_depends = []

winemetal_ld_args = [
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',   '-weak_framework', 'Cocoa',
  '-lsqlite3',
]

winemetal_lib = static_library('winemetal', winemetal_src,
  dependencies        : [ airconv_dep_darwin ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  c_args              : ['-ObjC'],
)

# The frameworks ride along to whatever links this, i.e. the single dylib.
winemetal_dep = declare_dependency(
  link_with           : [ winemetal_lib ],
  include_directories : [ include_directories('../winemetal') ],
  link_args           : winemetal_ld_args,
)
