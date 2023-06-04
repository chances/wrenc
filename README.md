# Wrenc - ahead-of-time Wren compiler

Wrenc is an LLVM-based compiler for the Wren programming language. It passes
almost the entire Wren test suite.

This repo contains four projects:

## The wrenc compiler frontend

This is based on the upstream Wren lexer/parser, but makes it's way through
a number of optimisations before being written out as LLVM IR.

Values are NaN-tagged, just like in Wren, but there aren't any special values
aside from null, which is just a NaN-tagged zero. This reduces the amount
of speecial-case checks on the code interacting with it.

The source code for this is located in the src directory.

## The runtime library

This contains a compiled version of wren_core.wren, taken from upstream Wren.

It also contains the memory allocator, GC, and native parts of the standard library.

This is located in rtsrc.

## Tree-sitter module

This is locateed in tree-sitter-wren, and can be used to incrementally parse
Wren source code with the tree-sitter programme.

This is intended for use in the IDE support module listed below, rather than for
direct use in an IDE; as such it doesn't have support for the syntax colouring
stuff that you might know tree-sitter for if your IDE uses it a lot. Adding this
probably isn't too hard, and patches for that are welcome.

## IDE Support Module

This project's goal is to build a full code completion and refactoring engine,
using the general principal used in the IntelliJ IDEs to effectively handle
large codebases.

It has it's own README in it's directory.

# Licence

In the interests of compatibility with the Luxe engine, this project is licenced
under a dual MIT/Apache-2 licence as seen in the LICENCE file.
