.intel_syntax noprefix
.set MESSAGE_LENGTH, 31
.text
.globl _start
_start:
    mov rax, 0x2000004
    mov rdi, 1
    lea rsi, [rip + message]
    mov rdx, MESSAGE_LENGTH
    syscall
    mov rax, 0x2000001
    mov rdi, 7
    syscall

message:
    .ascii "hello from x86_64 thru FEXMacx\n"
