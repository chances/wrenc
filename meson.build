project('wrencc', 'cpp', 'c')

fmt_lib = static_library('fmt',
  'lib/fmt-8.1.1/src/format.cc',
  include_directories: ['lib/fmt-8.1.1/include'],
)
fmt = declare_dependency(
  link_with: [fmt_lib],
  include_directories: ['lib/fmt-8.1.1/include'],
)

subdir('lib/antlr')
subdir('lib/qbe-1.0')

java = find_program('java')
antlr_src = custom_target('gen_antlr',
  input: ['lib/antlr/antlr-4.10.1-complete.jar', 'src/Wren.g4'],
  output: [
    'WrenBaseListener.cpp',
    'WrenListener.cpp',
    'WrenBaseVisitor.cpp',
    'WrenVisitor.cpp',
    'WrenLexer.cpp',
    'WrenParser.cpp',
  ],
  command: [java, '-jar', '@INPUT0@', '@INPUT1@', '-o', '@OUTDIR@', '-Dlanguage=Cpp', '-Xexact-output-dir', '-visitor'],
)

rtlib_lib = shared_library('wren-rtlib',
  'rtsrc/common.cpp',
  'rtsrc/Obj.cpp',
  'rtsrc/ObjString.cpp',
  'rtsrc/ObjMap.cpp',
  include_directories: ['rtsrc'],
)
rtlib = declare_dependency(
  link_with: [rtlib_lib],
  include_directories: ['rtsrc'],
)

executable('wrencc',
  'src/main.cpp',
  'src/wren_compiler.cpp',
  'src/Module.cpp',
  'src/IRNode.cpp',
  'src/CompContext.cpp',
  'src/SymbolTable.cpp',
  'src/ConstantsPool.cpp',
  'src/ArenaAllocator.cpp',
  'src/CcValue.cpp',
  'src/Scope.cpp',
  'src/ClassInfo.cpp',
  'src/passes/IRCleanup.cpp',
  'src/backend_qbe/QbeBackend.cpp',
  antlr_src,

  dependencies: [
    antlr,
    fmt,
    rtlib,
  ],
  cpp_args: [
    '-fPIC', # Required for Qt on my system
  ],
  include_directories: [
    'src',
  ],
  override_options: ['cpp_std=c++20'],
)
