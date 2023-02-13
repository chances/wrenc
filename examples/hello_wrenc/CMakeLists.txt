project("hello-wrenc")
cmake_minimum_required(VERSION 3.10)

# The path where wrenc is installed
set(WREN_COMPILER ${CMAKE_SOURCE_DIR}/../../build/wrencc)

set(wren_object_files)

# Compile the Wren file
function(compile_wren module_name wren_file)
	set(obj_path ${CMAKE_BINARY_DIR}/${wren_file}${CMAKE_C_OUTPUT_EXTENSION})
	add_custom_command(OUTPUT ${obj_path}
		COMMAND
		${WREN_COMPILER}  # Run the Wren compiler
		-c                # Only compile the file, don't link it
		-o ${obj_path}    # The name of the output file
		-Ofast            # Enable optimisations
		-m hello          # Set the name of the module

		# Specify the source file, and tell CMake that if
		# it changes, we have to recompile it.
		${CMAKE_SOURCE_DIR}/${wren_file}
		MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/${wren_file}
		)
	set(wren_object_files ${wren_object_files} ${obj_path} PARENT_SCOPE)
endfunction()

compile_wren(hello hello.wren)

# Discover the path of the include directory and where the
# runtime library is stored.
function(get_sys_path name dest)
	execute_process(COMMAND ${WREN_COMPILER} --print-sys-path ${name} OUTPUT_VARIABLE output
		OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
	set(${dest} ${output} PARENT_SCOPE)
endfunction()
get_sys_path(header-dir WREN_INC_DIR)
get_sys_path(rtlib-dir WREN_RTLIB_DIR)
get_sys_path(rtlib-name WREN_RTLIB_NAME)

# Tell CMake about the wrenc runtime library. This is where
# all the wrenWhatever functions are implemented, and it's
# where stuff like the Object or String classes live.
add_library(wrenc_rt INTERFACE)
target_include_directories(wrenc_rt INTERFACE ${WREN_INC_DIR})
target_link_libraries(wrenc_rt INTERFACE -Wl,-rpath=${WREN_RTLIB_DIR} -L${WREN_RTLIB_DIR} ${WREN_RTLIB_NAME})

add_executable(hello_wrenc
	hello.c
	${wren_object_files}
	)
target_link_libraries(hello_wrenc PRIVATE wrenc_rt)
