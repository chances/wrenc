%macro save_registers 0
    ; Set up RBP properly so GDB can stackwalk - this is unsurprisingly
    ; quite helpful when debugging.
    push rbp
    mov rbp, rsp

    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    push rbx
%endmacro

%macro restore_registers 0
    pop rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi

    pop rbp
%endmacro

section .text

; Transfer to a new stack, and invoke a function on it.
; ResumeFibreArgs* fibreAsm_invokeOnNewStack(rcx void* newStack, rdx void* func, r8 void* arg)
; Where func is: void (*func)(void *arg, void *oldStack)
; Note this function doesn't return the usual way - it's return value comes from
; switchToExisting running and making this function appear to return.
global fibreAsm_invokeOnNewStack
fibreAsm_invokeOnNewStack:
    ; Save all the callee-saved registers, since we might return to them before the
    ; function we're about to call has returned.
    save_registers

    ; Store the stack pointer, we'll pass it into the new function so it can get back
    ; to the old stack.
    mov r10, rsp

    ; Switch to the new stack
    mov rsp, rcx

    ; Set the base pointer to avoid callstacks winding back through the stack of the
    ; calling fibre, as that will change and break the GC.
    mov rbp, 0

    ; Push a bunch of zeros, so we'll trap if we end up here and try returning
    push qword 0
    push qword 0
    push qword 0
    push qword 0

    ; Stash away the function address, since we need that register for it's argument
    mov rax, rdx

    ; Pass in the old stack pointer and the argument
    mov rcx, r10
    mov rdx, r8

    ; Call the function
    call rax

    ; todo - crash on purpose for now
    push 0
    ret


; Resume on an existing stack, left as it was from invokeOnNewStack or switchToExisting.
; The previous stack pointer is written into the first qword of arg.
; ResumeFibreArgs* fibreAsm_invokeOnNewStack(rcx void* stack, rdx ResumeFibreArgs* arg)
global fibreAsm_switchToExisting
fibreAsm_switchToExisting:
    save_registers

    ; save the stack to be passed back to the resuming function, by writing it into
    ; the argument structure.
    mov [rdx], rsp

    ; And put this the argument in rax, making it the return argument
    mov rax, rdx

    ; switch back to the old function
    mov rsp, rcx

    restore_registers

    ; The existing stack and return address etc is all still in memory, so use that
    ; to make the originally-called switch-away function for this stack look like
    ; an ordinary function.
    ret
