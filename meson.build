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

# Common library shared by both the runtime and compiler
common_lib = static_library('common-lib',
  'common/common.cpp',
  'common/ClassDescription.cpp',
  'common/HashUtil.cpp',
  include_directories: ['common'],
)
common = declare_dependency(
  link_with: [common_lib],
  include_directories: ['common'],
)

# Make the main programme library almost runnable, so we can link it in to make a standalone Wren programme
# To do this, declare it as an executable then mess around with the linker flags
rtlib_main_stub = executable('wren-rtlib-stub',
  'rtsrc/standalone_main_stub.cpp',

  # TODO make something similar work on Windows
  # Important flags here:
  # - Build relocatable output: the output of the command is an object file we can link into a bigger programme later
  # - Disable position-independent executable mode: that conflicts with relocatable, we have to turn it on in the
  #   final linking step.
  # - Disable the default libraries. Since ld won't search for shared objects in relocatable mode (at least I think
  #   that's what's happening), it can't find libgcc_s.so. We'll put those libraries back in at the end.
  link_args: ['-no-pie', '-Wl,--relocatable', '-nodefaultlibs'],
)

wrenc = executable('wrencc',
  'src/main.cpp',
  'src/wren_compiler.cpp',
  'src/RunProgramme.cpp',
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
    common,
  ],
  cpp_args: [
    '-fPIC', # Required for Qt on my system
  ],
  include_directories: [
    'src',
  ],
  override_options: ['cpp_std=c++20'],
)

# Compile the Wren part of the main runtime library
rtlib_wren_core = custom_target('wren-core',
  output: ['wren_core.o'],
  input: ['lib/wren-main/src/vm/wren_core.wren'],
  command: [
    wrenc,
    '-c',
    '-o', '@OUTPUT@',
    '@INPUT@',
    '--module=wren_core',
    '--internal-build-core-lib',
  ]
)

# For the runtime library, generate a file that contains functions for Wren to call that then call
# the native C++ methods.
prog_python = import('python').find_installation('python3')
rtlib_bindings = custom_target(
  'rtlib-bindings',
  output : ['bindings.gen.cpp'],
  input : [
    'rtsrc/gen_bindings.py',
    'rtsrc/Obj.h',
    'rtsrc/ObjString.h',
    'rtsrc/ObjMap.h',
    'rtsrc/ObjSystem.h',
    'rtsrc/ObjBool.h',
    'rtsrc/ObjList.h',
    'rtsrc/ObjNum.h',
    'rtsrc/ObjFibre.h',
    'rtsrc/ObjFn.h',
  ],
  command : [prog_python, '@INPUT@', '--output', '@OUTPUT@'],
)

rtlib_lib = shared_library('wren-rtlib',
  'rtsrc/GenEntry.cpp',
  'rtsrc/Obj.cpp',
  'rtsrc/ObjString.cpp',
  'rtsrc/ObjMap.cpp',
  'rtsrc/ObjClass.cpp',
  'rtsrc/ObjSystem.cpp',
  'rtsrc/ObjBool.cpp',
  'rtsrc/ObjList.cpp',
  'rtsrc/ObjNum.cpp',
  'rtsrc/ObjFibre.cpp',
  'rtsrc/ObjFn.cpp',
  'rtsrc/ObjManaged.cpp',
  'rtsrc/CoreClasses.cpp',
  'rtsrc/WrenRuntime.cpp',
  rtlib_wren_core,
  rtlib_bindings,
  include_directories: ['rtsrc'],
  override_options: ['cpp_std=c++20'],
  dependencies: [common],

  # Hide symbols. This is not only good practice, but without it we'd have to have all of wren_core's
  # symbols go through the GOT which would be a big pain.
  gnu_symbol_visibility: 'hidden',
)
rtlib = declare_dependency(
  link_with: [rtlib_lib],
  include_directories: ['rtsrc'],
)
