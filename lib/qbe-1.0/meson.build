# Not supplied with QBE

qbe_lib = shared_library('qbe',
  # Platform-independant
  'util.c',
  'parse.c',
  'cfg.c',
  'mem.c',
  'ssa.c',
  'alias.c',
  'load.c',
  'copy.c',
  'fold.c',
  'live.c',
  'spill.c',
  'rega.c',
  'gas.c',

  # AMD64
  'amd64/targ.c',
  'amd64/sysv.c',
  'amd64/isel.c',
  'amd64/emit.c',

  # AArch64
  'arm64/targ.c',
  'arm64/abi.c',
  'arm64/isel.c',
  'arm64/emit.c',

  # RV-64
  'rv64/targ.c',
  'rv64/abi.c',
  'rv64/isel.c',
  'rv64/emit.c',

  include_directories: ['.'],
  override_options: ['c_std=c99', 'b_lundef=false'],
)

qbe = declare_dependency(
  link_with: [qbe_lib],
  include_directories: ['.'],
)

executable('qbe_bin',
  'main.c',
  dependencies: [qbe],
)
