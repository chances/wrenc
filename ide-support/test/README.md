This directory contains tests for the IDE support system. They can be run
using the ide_test.py script in the root test directory, which shares a lot
in common with the compiler's test.py script (which itself is based off
upstream Wren's one).

Many of these files notably aren't valid Wren files: IDEs very frequently
operate on half-written programmes, those with major syntactical errors,
and so on. Thus these files aren't compiled, nor are they expected to do
so without errors.

(As a future test, we should use the IDE to index Wren's regular test suite,
since obviously it should be able to parse all valid programmes too.)

Most IDE actions happen at some specific point in the code. Whether it's
auto-completion, go-to definition, line actions (like in IntelliJ when you
hit alt-enter), they are all related to the point in the code where the user
triggers the action.

For these, an escape character is needed. The logical-not '¬' symbol
is used for this (since it won't conflict with anything in the
code to test, thus doesn't need escaping, and is present on all
(British) keyboards). This character is followed by a special assertion
string, which is read by the test runner.

The basic syntax is `<wren code>•my assertion!<wren code>`, with the dot
and exclamation mark denoting the start and end of the assertion,
respectively.

The assertion is one of the following assertions:

* `complete:<item,item,etc>` - Trigger an auto-complete action, and require
  the listed items are present in it.
