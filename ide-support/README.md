This is the IDE support backend. It's designed to index Wren code and provide
auto-completion, error highlighting (to the extent that's possible), warnings
and so on.

The main part of this project is a library that intends to provide a stable
ABI, which can then be used in IDEs directly (for IntelliJ and the
Godot Editor), or to implement an LSP server.

This uses the Tree-sitter library to provide an incremental parser, which
this library then indexes. Tree-sitter builds an AST of the source file,
and parts which haven't change keep the same nodes. This allows the indexer
to skip unchanged parts of the programme on edit.

See Alex Kladov (aka matklad)'s excellent post about how IntelliJ works,
which is also how this project is structured:
https://rust-analyzer.github.io/blog/2020/07/20/three-architectures-for-responsive-ide.html
