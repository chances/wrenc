project('wrencc', 'cpp', 'c', 'nasm')

fmt_lib = static_library('fmt',
  'lib/fmt-8.1.1/src/format.cc',
  include_directories: ['lib/fmt-8.1.1/include'],
)
fmt = declare_dependency(
  link_with: [fmt_lib],
  include_directories: ['lib/fmt-8.1.1/include'],
)

subdir('lib/qbe-1.0')

llvm = dependency('llvm',
  required: get_option('use_llvm'),
  modules: [
    'core',
    'passes',
    'codegen',
    'all-targets', # Include target (eg amd64, aarch64) specific libraries for all available targets
  ],
)

conf_data = configuration_data()
conf_data.set('USE_LLVM', llvm.found())
configure_file(
    input : 'common/wrencc_config.h.in',
    output : 'wrencc_config.h',
    configuration : conf_data,
)

# Common library shared by both the runtime and compiler
common_lib = static_library('common-lib',
  'common/common.cpp',
  'common/AttributePack.cpp',
  'common/ClassDescription.cpp',
  'common/StackMapDescription.cpp',
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

# Functionality that will be used by LLVM when processing this IR. This is provided as a separate library so you
# can manually run LLVM passes (loading this with -load plugin.dll) for testing purposes.
llvm_plugin = shared_library('wren_llvm_plugin',
  'src/backend_llvm/WrenGCStrategy.cpp',
  'src/backend_llvm/WrenGCMetadataPrinter.cpp',

  name_prefix: '',  # Don't use a 'lib' prefix on Linux
  dependencies: [
    llvm,
    common,
    fmt,
  ],
  cpp_args: [
    # Extending a class with a vtable and with RTTI on generates a reference to the parent's RTTI info, and
    # since LLVM is usually compiled without RTTI this causes a link error.
    '-fno-rtti',
  ],
)

wrenc = executable('wrencc',
  'src/main.cpp',
  'src/wren_compiler.cpp',
  'src/RunProgramme.cpp',
  'src/Module.cpp',
  'src/IRNode.cpp',
  'src/IRPrinter.cpp',
  'src/CompContext.cpp',
  'src/SymbolTable.cpp',
  'src/ConstantsPool.cpp',
  'src/ArenaAllocator.cpp',
  'src/CcValue.cpp',
  'src/Scope.cpp',
  'src/ClassInfo.cpp',
  'src/Utils.cpp',
  'src/IBackend.cpp',
  'src/passes/IRCleanup.cpp',
  'src/passes/BasicBlockPass.cpp',
  'src/backend_qbe/QbeBackend.cpp',

  dependencies: [
    fmt,
    common,
    meson.get_compiler('cpp').find_library('dl')
  ],
  cpp_args: [
    '-fPIC', # Required for Qt on my system
  ],
  include_directories: [
    'src',
  ],
  override_options: ['cpp_std=c++20'],
  export_dynamic: true, # Export the symbols so the LLVM backend can access them
)

# Build the LLVM driver as it's own shared library that gets loaded into the compiler
# at runtime. This is mostly for debugging: GDB doesn't have to load all the LLVM
# symbols when the compiler starts, so it should speed up debugging the frontend components
# by a lot.
# Use a shared module, which means we can link to symbols that exist in the frontend without
# splitting that into it's own library.
llvm_backend = shared_module('llvm_backend',
  'src/backend_llvm/LLVMBackend.cpp',
  name_prefix: '', # Don't prepend a 'lib' prefix on Linux, this isn't really a library
  link_with: [
    llvm_plugin,
  ],
  include_directories: [
    'src',
  ],
  dependencies: [
    fmt,
    common,
    llvm,
  ],
  cpp_args: [
    '-fPIC',
  ],
  override_options: ['cpp_std=c++20'],
)

# Compile the Wren part of the main runtime library
rtlib_wren_core = custom_target('wren-core',
  output: ['wren_core.o'],
  input: ['lib/wren-main/src/vm/wren_core.wren'],
  depends: [llvm_backend],
  command: [
    wrenc,
    '-c',
    '-o', '@OUTPUT@',
    '@INPUT@',
    '--module=wren_core',
    '--internal-build-core-lib',
    '--backend-llvm',
  ]
)

# For the runtime library, generate a file that contains functions for Wren to call that then call
# the native C++ methods.
binding_options = []
if get_option('pre_entry_gc')
  binding_options += '--pre-entry-gc'
endif
prog_python = import('python').find_installation('python3')
rtlib_bindings = custom_target(
  'rtlib-bindings',
  output : ['bindings.gen.cpp'],
  input : [
    'rtsrc/gen_bindings.py',
    'rtsrc/Obj.h',
    'rtsrc/ObjClass.h',
    'rtsrc/ObjString.h',
    'rtsrc/ObjMap.h',
    'rtsrc/ObjRange.h',
    'rtsrc/ObjSequence.h',
    'rtsrc/ObjSystem.h',
    'rtsrc/ObjBool.h',
    'rtsrc/ObjList.h',
    'rtsrc/ObjNull.h',
    'rtsrc/ObjNum.h',
    'rtsrc/ObjFibre.h',
    'rtsrc/ObjFn.h',
  ],
  command : [prog_python, '@INPUT@', '--output', '@OUTPUT@', binding_options],
)

rtlib_lib = shared_library('wren-rtlib',
  'rtsrc/GenEntry.cpp',
  'rtsrc/Errors.cpp',
  'rtsrc/Obj.cpp',
  'rtsrc/ObjString.cpp',
  'rtsrc/ObjMap.cpp',
  'rtsrc/ObjRange.cpp',
  'rtsrc/ObjClass.cpp',
  'rtsrc/ObjSequence.cpp',
  'rtsrc/ObjSystem.cpp',
  'rtsrc/ObjBool.cpp',
  'rtsrc/ObjList.cpp',
  'rtsrc/ObjNull.cpp',
  'rtsrc/ObjNum.cpp',
  'rtsrc/ObjFibre.cpp',
  'rtsrc/ObjFibreX86.asm',
  'rtsrc/ObjFn.cpp',
  'rtsrc/ObjManaged.cpp',
  'rtsrc/CoreClasses.cpp',
  'rtsrc/LinkedList.cpp',
  'rtsrc/GCTracingScanner.cpp',
  'rtsrc/RtModule.cpp',
  'rtsrc/SlabObjectAllocator.cpp',
  'rtsrc/WrenRuntime.cpp',
  rtlib_wren_core,
  rtlib_bindings,
  include_directories: ['rtsrc'],
  override_options: ['cpp_std=c++20'],
  dependencies: [
    common,
    dependency('libunwind'),
  ],

  # Hide symbols. This is not only good practice, but without it we'd have to have all of wren_core's
  # symbols go through the GOT which would be a big pain.
  gnu_symbol_visibility: 'hidden',
)
rtlib = declare_dependency(
  link_with: [rtlib_lib],
  include_directories: ['rtsrc'],
)
