global switch_context
global user_entry_trampoline

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

user_entry_trampoline:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    iret

switch_context:

    mov eax, [esp+4]
    mov ecx, [esp+8]

    pushfd
    push ebp
    push ebx
    push esi
    push edi

    mov [eax], esp

    mov esp, ecx

    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd

    ret
