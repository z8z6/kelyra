// Linux x86-64 process entry. The kernel provides argc and argv on the
// initial stack; no C runtime has run at this point.
  .text
  .globl _start
  .type _start, @function
_start:
  xorl %ebp, %ebp
  movq (%rsp), %rdi
  leaq 8(%rsp), %rsi
  leaq 16(%rsp,%rdi,8), %rdx
  andq $-16, %rsp
  call __kelyra_start

  // exit_group terminates all threads if the program created any.
  movl %eax, %edi
  movl $231, %eax
  syscall
  ud2
  .size _start, .-_start

  .section .note.GNU-stack,"",@progbits
