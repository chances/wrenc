Tinylink is a custom PE/COFF linker. The idea is that for compiling
stand-alone Wren programmes, we first compile a 'stub' executable that
has all the required C code, then 'tack on' the standalone modules.

This is only for Windows; on Linux, we'll just assume a system linker
is available and abuse that into linking an already-mostly-linked executable.

As such, we want to link a few COFF objects (the Wren modules) with an
already-almost-linked PE object (the stub).

For security, we won't put a great deal of effort (or really any at all)
into bounds-checking: the stub programme is built as part of wrenc, and
the object files are built by wrenc. I can't see any plausable way an
attacker could cause them to be invalid where they don't already have
the same access as the linker does.

This doesn't mean an attacker should be able to get code execution when
arbitrary Wren code is compiled - they need to modify the compiled
object files.
