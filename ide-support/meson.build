
tree_sitter_cmd = find_program('tree-sitter',
  required: get_option('ide_support'),
)

# If we can't find tree-sitter (including if support was disabled), stop
# running this script.
if not tree_sitter_cmd.found()
    subdir_done()
endif

# Compile the Tree-sitter runtime library
tree_sitter_lib_dir = '../lib/tree-sitter-0.20.7/lib'
tree_sitter_rtlib = static_library('tree-sitter-rtlib',
  tree_sitter_lib_dir + '/src/lib.c', # Includes all the other source files
  include_directories: [tree_sitter_lib_dir + '/include'],
)
tree_sitter_rtlib_dep = declare_dependency(
  link_with: [tree_sitter_rtlib],
  include_directories: [tree_sitter_lib_dir + '/include'],
)

# Compile the generated tree-sitter lexer/parser
# Tree-sitter itself is quite opinionated about where it puts it's output
# files (which is unsurprisingly a pain when integrating it with other tools),
# so use a Python wrapper script to run it.
tree_sitter_generated = custom_target('tree-sitter-wren',
  output: ['tree_sitter_wren.gen.c'],
  input: ['tree_sitter_wrapper.py', '../tree-sitter-wren/grammar.js'],
  command: [prog_python, '@INPUT0@',
    '--output', '@OUTPUT@',
    '--grammar-file', '@INPUT1@',
    '--ts-path', tree_sitter_cmd,
    '--tmp-dir', '@PRIVATE_DIR@',
  ],
)

ide_test_tool = executable('ide-test-tool',
  'ide_devtool.cpp',
  'ide_util.cpp',
  'ActiveFile.cpp',
  'GrammarInfo.cpp',

  '../tree-sitter-wren/src/scanner.c', # Manually written, not generated
  tree_sitter_generated,

  dependencies: [
    tree_sitter_rtlib_dep,
  ],
)
