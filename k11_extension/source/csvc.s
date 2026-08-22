.arm
.balign 4

.macro SVC_BEGIN name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
    .align 2
\name:
.endm

.macro SVC_END
.endm

SVC_BEGIN svcQueryMemory
    push {r4-r7}
    mov  r6, r0
    mov  r7, r1
    mov  r0, r2
    svc  0x02
    stmia r6, {r1-r4}
    str  r5, [r7]
    pop  {r4-r7}
    bx   lr
SVC_END

SVC_BEGIN svcCloseHandle
    svc 0x23
    bx lr
SVC_END

SVC_BEGIN svcSleepThread
    svc 0x0A
    bx lr
SVC_END

SVC_BEGIN svcOpenProcess
    push {r0}
    mov  r0, r1
    svc  0x33
    pop  {r2}
    str  r1, [r2]
    bx   lr
SVC_END

SVC_BEGIN svcGetProcessId
    push {r0}
    mov  r0, r1
    svc  0x35
    pop  {r2}
    str  r1, [r2]
    bx   lr
SVC_END

SVC_BEGIN svcControlProcessMemory
    push {r4, r5}
    ldr  r4, [sp, #8]
    ldr  r5, [sp, #12]
    svc  0x70
    pop  {r4, r5}
    bx   lr
SVC_END

SVC_BEGIN svcFlushEntireDataCache
    svc 0x92
    bx lr
SVC_END

SVC_BEGIN svcInvalidateEntireInstructionCache
    svc 0x94
    bx lr
SVC_END

SVC_BEGIN svcControlMemoryUnsafe
    str r4, [sp, #-4]!
    ldr r4, [sp, #4]
    svc 0xA3
    ldr r4, [sp], #4
    bx lr
SVC_END