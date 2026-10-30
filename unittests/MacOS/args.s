.intel_syntax noprefix
.text
.globl _start
_start:
    mov r12, qword ptr [rsp]
    mov r13, qword ptr [rsp + 16]

    mov rsi, r13
    xor edx, edx
count_length:
    cmp byte ptr [rsi + rdx], 0
    je print_argument
    inc rdx
    jmp count_length
print_argument:
    mov rax, 0x2000004
    mov rdi, 1
    syscall

    mov rax, 0x2000004
    mov rdi, 1
    lea rsi, [rip + newline]
    mov rdx, 1
    syscall

    mov rax, 0x2000004
    mov rdi, -1
    lea rsi, [rip + newline]
    mov rdx, 1
    syscall
    jc carry_was_set
    lea rsi, [rip + carry_missing]
    mov rdx, 14
    jmp report_carry
carry_was_set:
    lea rsi, [rip + carry_ok]
    mov rdx, 22
report_carry:
    mov rax, 0x2000004
    mov rdi, 1
    syscall

    mov rax, 0x2000001
    mov rdi, r12
    syscall

newline:
    .ascii "\n"
carry_ok:
    .ascii "carry set on EBADF ok\n"
carry_missing:
    .ascii "carry missing\n"
