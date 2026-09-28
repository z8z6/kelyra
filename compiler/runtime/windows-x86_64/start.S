// Windows x86-64 PE entry. The loader establishes the process environment.
  .text
  .globl mainCRTStartup
mainCRTStartup:
  // The Win64 ABI requires 32 bytes of shadow space at each call site.
  subq $40, %rsp
  call __kelyra_start
  movl %eax, %ecx
  call ExitProcess
  ud2
