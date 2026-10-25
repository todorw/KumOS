global gdt_flush
global tss_flush

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]

    jmp 0x08:.flush
.flush:

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

tss_flush:
    mov ax, 0x2B
    ltr ax
    ret
