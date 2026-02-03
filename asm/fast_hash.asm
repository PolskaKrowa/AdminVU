; Fast hash function in x86_64 assembly
; uint64_t fast_hash(const char* str, size_t len);

section .text
global fast_hash

fast_hash:
    ; rdi = str pointer
    ; rsi = length
    
    xor rax, rax        ; hash = 0
    xor rcx, rcx        ; counter = 0
    
    test rsi, rsi       ; check if length is 0
    jz .done
    
.loop:
    movzx rdx, byte [rdi + rcx]  ; load byte
    imul rax, 31        ; hash *= 31
    add rax, rdx        ; hash += byte
    inc rcx             ; counter++
    cmp rcx, rsi        ; check if done
    jl .loop
    
.done:
    ret
