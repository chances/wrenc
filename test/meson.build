# Build the API tests - this binary will be used by the Python test runner
api_test_dir = '../lib/wren-main/test/api/'
executable('api-test-runner',
  'api_test_runner.cpp',
  api_test_dir + 'api_tests.c',
  api_test_dir + 'benchmark.c',
  api_test_dir + 'call.c',
  api_test_dir + 'call_calls_foreign.c',
  api_test_dir + 'call_wren_call_root.c',
  api_test_dir + 'error.c',
  api_test_dir + 'foreign_class.c',
  api_test_dir + 'get_variable.c',
  api_test_dir + 'handle.c',
  api_test_dir + 'lists.c',
  api_test_dir + 'maps.c',
  api_test_dir + 'new_vm.c',
  api_test_dir + 'reset_stack_after_call_abort.c',
  api_test_dir + 'reset_stack_after_foreign_construct.c',
  api_test_dir + 'resolution.c',
  api_test_dir + 'slots.c',
  api_test_dir + 'user_data.c',

  # Use the regular Wren includes but link to the
  # wrenc runtime, to make sure we're ABI-compatible.
  include_directories: [
    '..', # For the api_test_runner.
    '../lib/wren-main/src/include',
    api_test_dir,
  ],
  link_with: [
    rtlib_lib,
  ],
  dependencies: [
    common,
  ],
)

