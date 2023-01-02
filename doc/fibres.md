Fibres are implemented with a simple stack-switching system: each fibre gets
it's own stack, and switching between them switches between stacks.

These are not threads, however: they're not managed by the OS, and all we're
doing is editing the stack pointer.

Consider the following stack, where square brackets indicate the values are
all part of the same function call (the actual stack contents for frames
we're not involved with are omitted for conciseness). The functions that
each frame belong to are shown above the frames, and the contents of the
stack is shown below (note each function has a pointer into the one before so
it knows where to return to), and the current position of the stack pointer
shown with an arrow (`<-- SP`):

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo
  []     [main]        [test:__root_func]       [ObjFibre::Call]   <--- SP
```

Now, `ObjFibre::StartAndSwitchTo` calls a function written in assembly,
called `fibreAsm_invokeOnNewStack`. I'll call it `asm_ins` here:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo]  <--- SP
```

At this point, `asm_ins` dumps all the callee-saved registers \[1] to the stack, leaving
all the information there that's needed to resume running it:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc]  <--- SP
```

Then `asm_ins` edits the stack pointer, pointing it at a brand-new stack that we just allocated
and is completely empty:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins (no longer running)
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc]

ObjFibre new:
asm_ins
  []  <--- SP
```

At this point, `asm_ins` calls `ObjFibre::RunOnNewStack`, which in turns runs the `ObjFn`. It passes the address
of the last stack frame to `RunOnNewStack`, which stores it away for later in the ObjFibre instance representing
the main stack.

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins (no longer running)
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc] <-- main.resumeAddr

ObjFibre new:
asm_ins  RunOnNewStack  ObjFn::Call    test::my_closure
  []      [asm_ins]    [RunOnNewStack]    [ObjFn::Call]  <--- SP
```

This closure then calls `ObjFibre::Yield` - this means we have to switch back to the original
fibre and continue running it. `Yield` calls `ObjFibre::ResumeSuspended`, which in turn calls a second
assembly function, `fibreAsm_switchToExisting` which I'll abbreviate to `asm_switch` (I'll also omit
the call to `Yield` to keep the stacks shorter):

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins (no longer running)
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc] <-- main.resumeAddr

ObjFibre new:
asm_ins  RunOnNewStack  ObjFn::Call    test::my_closure    ResumeSuspend      asm_switch
  []      [asm_ins]    [RunOnNewStack]    [ObjFn::Call]  [test::my_closure] [ResumeSuspend]  <--- SP
```

Now, `asm_switch` does the same thing as `asm_ins` did: spill all the callee-saved registers to
the stack:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo     asm_ins (no longer running)
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc] <-- main.resumeAddr

ObjFibre new:
asm_ins  RunOnNewStack  ObjFn::Call    test::my_closure    ResumeSuspend      asm_switch
  []      [asm_ins]    [RunOnNewStack]    [ObjFn::Call]  [test::my_closure] [ResumeSuspend rdi rsi r12 r13 etc]  <--- SP
```

Then it sets the stack pointer to the address that `RunOnNewStack` stored earlier (`main.resumeAddr`), having
been given it by `asm_ins`. This means we're back on the original stack, so the stack frame on the original stack
that `asm_ins` made earlier is effectively being shared by the two functions, using them one after the other:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo   was asm_ins, is now asm_switch
  []     [main]        [test:__root_func]       [ObjFibre::Call]     [StartAndSwitchTo rdi rsi r12 r13 etc]  <-- SP

ObjFibre new:
asm_ins  RunOnNewStack  ObjFn::Call    test::my_closure    ResumeSuspend      asm_switch
  []      [asm_ins]    [RunOnNewStack]    [ObjFn::Call]  [test::my_closure] [ResumeSuspend rdi rsi r12 r13 etc]
```

Then `asm_switch` pops all the registers off and returns. From `StartAndSwitchTo`'s perspective, `asm_ins`
was just a regular function call. `asm_switch` returns a pointer to a struct to `StartAndSwitchTo`, which
was prepared by `ResumeSuspend` and contains things like the `ObjFibre*` pointer to `main` and `new`, the
`Value` that was passed to `ObjFibre::Yield`, and (set by `asm_switch`) the stack pointer of the second stack:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::StartAndSwitchTo
  []     [main]        [test:__root_func]       [ObjFibre::Call]  <-- SP

ObjFibre new:
asm_ins  RunOnNewStack  ObjFn::Call    test::my_closure  ResumeSuspend               asm_switch
  []      [asm_ins]    [RunOnNewStack]   [ObjFn::Call]  [test::my_closure] [ResumeSuspend rdi rsi r12 r13 etc] <-- new.resumeAddr
```

The main fibre continues, and if it calls `new.call()` again then `ResumeSuspend`
runs on the original stack to switch back to it, and this repeats until the new
fibre terminates:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::ResumeSuspend          asm_switch
  []     [main]        [test:__root_func]    [ObjFibre::Call]     [ResumeSuspend rdi rsi r12 r13 etc]  <-- main.resumeAddr

ObjFibre new:
asm_ins  RunOnNewStack
  []      [asm_ins]    <-- SP
```

Now, `asm_ins` doesn't have enough information to switch back to the main fibre
when `RunOnStack` returns. Instead, it does a regular switch back to `main` with
one exception: a flag is set in the return struct to indicate it has completed. When
`ResumeSuspend` (it's actually in a separate handling method, so this works if
the function exited without switching and this is passed back to `StartAndSwitchTo`)
receives a struct with this flag set, it destroys the stack of the new fibre:

```
ObjFibre main:
 main test:__root_func   ObjFibre::Call   ObjFibre::ResumeSuspend    DeleteStack
  []     [main]        [test:__root_func]     [ObjFibre::Call]      [ResumeSuspend]  <-- SP

ObjFibre new:
  *** gone ***
```

\[1] These are the registers that must remain the same at the end of a function call
compared to at the start. They vary between calling conventions, but on X86 (the only
architecture currently supported), we'll always use the Microsoft calling convention
to avoid writing the assembly twice, once for Windows and once for Linux.
