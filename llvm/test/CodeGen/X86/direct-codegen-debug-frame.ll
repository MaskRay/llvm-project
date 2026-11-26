; RUN: llc -O0 -mtriple=x86_64-linux-gnu -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-dwarfdump --verify %t.o
; RUN: llvm-dwarfdump --debug-info --debug-frame %t.o | FileCheck %s
; RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=CODE
; RUN: opt -strip-debug %s | llc -O0 -mtriple=x86_64-linux-gnu -direct-codegen -direct-codegen-abort -filetype=obj -o %t.s.o
; RUN: llvm-objdump -d --no-show-raw-insn %t.s.o | FileCheck %s --check-prefix=CODE

;; Without unwind tables, CFI goes to .debug_frame. DWARF64 and Key Instructions
;; (on @g) are accepted. The frame base is RBP even when "frame-pointer" allows
;; omitting it, since directly compiled code always has one. A variable behind a
;; pointer argument that lives only in a register is dropped: describing it
;; would need a store, and debug info must not change the code.

; CHECK:      DW_TAG_subprogram
; CHECK:        DW_AT_frame_base (DW_OP_reg6 RBP)
; CHECK:        DW_AT_name ("f")
; CHECK:        DW_TAG_variable
; CHECK-NEXT:     DW_AT_location (DW_OP_fbreg -4)
; CHECK-NEXT:     DW_AT_name ("x")
; CHECK-NOT:    DW_AT_name ("y")

; CHECK:      DW_TAG_subprogram
; CHECK:        DW_AT_name ("g")
; CHECK:        DW_TAG_variable
; CHECK-NEXT:     DW_AT_location (DW_OP_fbreg -4)
; CHECK-NEXT:     DW_AT_name ("field")
; CHECK-NOT:    DW_AT_name ("v")

; CHECK:      .debug_frame contents:
; CHECK:      FDE
; CHECK:        DW_CFA_def_cfa_offset: +16

; CODE-LABEL: <f>:
; CODE-NEXT:    pushq %rbp
; CODE-NEXT:    movq %rsp, %rbp
; CODE-NEXT:    movl (%rdi), %edi
; CODE-NEXT:    movl %edi, -0x4(%rbp)
; CODE-NEXT:    movl -0x4(%rbp), %eax
; CODE-NEXT:    leave
; CODE-NEXT:    retq

; CODE-LABEL: <g>:
; CODE-NEXT:    pushq %rbp
; CODE-NEXT:    movq %rsp, %rbp
; CODE-NEXT:    movl %edi, -0x4(%rbp)
; CODE-NEXT:    movl -0x4(%rbp), %eax

define i32 @f(ptr %p) "frame-pointer"="none" !dbg !6 {
entry:
  %x = alloca i32
    #dbg_declare(ptr %x, !12, !DIExpression(), !14)
    #dbg_declare(ptr %p, !11, !DIExpression(), !13)
  %0 = load i32, ptr %p, !dbg !15
  store i32 %0, ptr %x, !dbg !14
  %1 = load i32, ptr %x, !dbg !16
  ret i32 %1, !dbg !16
}

;; Records that cannot be described from the code as generated are dropped
;; rather than making the function fall back with debug info only. A declare of
;; a field of a frame object is described in place.
define i32 @g(i32 %a) "frame-pointer"="all" !dbg !20 {
entry:
  %s = alloca { i32, i32 }
  %f = getelementptr { i32, i32 }, ptr %s, i32 0, i32 1
    #dbg_declare(ptr %f, !21, !DIExpression(), !22)
  store i32 %a, ptr %f, !dbg !22
    #dbg_value(i32 %a, !23, !DIExpression(), !22)
  %v = load i32, ptr %f, !dbg !22
  ret i32 %v, !dbg !22
}

;; A field whose offset does not fit a displacement is dropped, not a reason to
;; fall back.
define i32 @h(i32 %a) !dbg !30 {
entry:
  %s = alloca [4 x i8]
  %f = getelementptr i8, ptr %s, i64 4294967296
  %dead = load i8, ptr %f
    #dbg_declare(ptr %f, !31, !DIExpression(), !32)
  ret i32 %a, !dbg !32
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang", emissionKind: FullDebug)
!1 = !DIFile(filename: "a.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"DWARF64", i32 1}
!6 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !7, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !10)
!7 = !DISubroutineType(types: !8)
!8 = !{!9, !9}
!9 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!10 = !{}
!11 = !DILocalVariable(name: "y", scope: !6, file: !1, line: 1, type: !9)
!12 = !DILocalVariable(name: "x", scope: !6, file: !1, line: 2, type: !9)
!13 = !DILocation(line: 1, column: 11, scope: !6)
!14 = !DILocation(line: 2, column: 7, scope: !6)
!15 = !DILocation(line: 2, column: 11, scope: !6)
!16 = !DILocation(line: 3, column: 3, scope: !6)
!20 = distinct !DISubprogram(name: "g", scope: !1, file: !1, line: 5, type: !7, scopeLine: 5, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !10, keyInstructions: true)
!21 = !DILocalVariable(name: "field", scope: !20, file: !1, line: 6, type: !9)
!22 = !DILocation(line: 6, column: 3, scope: !20)
!23 = !DILocalVariable(name: "v", scope: !20, file: !1, line: 6, type: !9)
!30 = distinct !DISubprogram(name: "h", scope: !1, file: !1, line: 8, type: !7, scopeLine: 8, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !10)
!31 = !DILocalVariable(name: "far", scope: !30, file: !1, line: 9, type: !9)
!32 = !DILocation(line: 9, column: 3, scope: !30)
