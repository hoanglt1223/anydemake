# Generates a .def, x64 MASM jump stubs and a name table for one proxy DLL.
# Inputs: EXPORTS (list file), OWN (names implemented in C as proxy_<name>), LIB, OUT_DIR.
# Run with cmake -P, which starts with old policies (IN_LIST needs CMP0057).
cmake_minimum_required(VERSION 3.24)
file(STRINGS ${EXPORTS} lines REGEX "^[0-9]")
set(def "LIBRARY ${LIB}\nEXPORTS\n")
# Each stub jumps through the table. An empty entry (a forwarded export called
# before anything else in the proxy) goes through potatify_fwd_slow, which
# loads the real DLL first and keeps the argument registers intact.
set(asm "EXTERN potatify_forward_table:QWORD\nEXTERN potatify_resolve:PROC\n.code\n")
# PROC FRAME gives the slow path unwind info for stack walks and exceptions
# raised while the real DLL loads; xmm0-3 are volatile and need no .savexmm128.
string(APPEND asm "potatify_fwd_slow PROC FRAME
    push rcx
    .pushreg rcx
    push rdx
    .pushreg rdx
    push r8
    .pushreg r8
    push r9
    .pushreg r9
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    mov ecx, r10d
    call potatify_resolve
    movdqu xmm0, [rsp+20h]
    movdqu xmm1, [rsp+30h]
    movdqu xmm2, [rsp+40h]
    movdqu xmm3, [rsp+50h]
    add rsp, 68h
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp rax
potatify_fwd_slow ENDP
")
set(names "const char *const potatify_forward_names[] = {\n")
set(i 0)
foreach(line IN LISTS lines)
  string(REPLACE " " ";" parts "${line}")
  list(GET parts 0 ord)
  list(GET parts 1 name)
  if(name IN_LIST OWN)
    string(APPEND def "    ${name}=proxy_${name} @${ord}\n")
  else()
    math(EXPR off "${i} * 8")
    string(APPEND def "    ${name}=potatify_fwd_${i} @${ord}\n")
    string(APPEND asm "potatify_fwd_${i} PROC
    mov r10d, ${i}
    mov rax, qword ptr [potatify_forward_table + ${off}]
    test rax, rax
    jz potatify_fwd_slow
    jmp rax
potatify_fwd_${i} ENDP
")
    string(APPEND names "    \"${name}\",\n")
    math(EXPR i "${i} + 1")
  endif()
endforeach()
string(APPEND asm "END\n")
string(APPEND names "};\nvoid *potatify_forward_table[${i}];\nconst int potatify_forward_count = ${i};\n")
foreach(f def asm names)
  set(ext ${f})
  if(f STREQUAL "names")
    set(ext c)
  endif()
  set(path ${OUT_DIR}/${LIB}-forward.${ext})
  set(old "")
  if(EXISTS ${path})
    file(READ ${path} old)
  endif()
  if(NOT "${old}" STREQUAL "${${f}}")
    file(WRITE ${path} "${${f}}")
  endif()
endforeach()
