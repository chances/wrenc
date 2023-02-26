#!/bin/zsh
# Go through all the normal tests, parsing them to find any that don't pass.

num=0

for f in $(find ../lib/wren-main/test -name '*.wren'); do
	num=$(( $num + 1 ))

	# Check if this test expects a compile error, and if so skip it.
	grep -q '// expect error' $f; found_expect_error=$?
	if [[ $found_expect_error == 0 ]]; then
		echo "Skipping expect-error test $num $f"
		continue
	fi

	output=$(tree-sitter parse $f)
	result=$?

	if [[ $result == 0 ]]; then
		continue
	fi

	echo "Test num $num failed: $f"
	echo $output
	exit
done

