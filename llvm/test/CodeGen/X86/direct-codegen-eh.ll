; RUN: llc -O0 -mtriple=x86_64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s --check-prefix=FRAME
; RUN: llvm-readelf -S -s %t.o | FileCheck %s --check-prefix=ELF

;; An invoke gets a call-site entry pointing at its landing pad, the exception
;; pointer and selector arrive in RAX and RDX, and resume calls _Unwind_Resume.

@_ZTIi = external constant ptr
declare void @may_throw(i32)
declare i32 @__gxx_personality_v0(...)
declare i32 @llvm.eh.typeid.for.p0(ptr)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

define i32 @catcher(i32 %x) uwtable personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may_throw(i32 %x)
          to label %cont unwind label %lpad

cont:
  ret i32 0

lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  %exn = extractvalue { ptr, i32 } %lp, 0
  %sel = extractvalue { ptr, i32 } %lp, 1
  %id = call i32 @llvm.eh.typeid.for.p0(ptr @_ZTIi)
  %match = icmp eq i32 %sel, %id
  br i1 %match, label %catch, label %resume

catch:
  %p = call ptr @__cxa_begin_catch(ptr %exn)
  call void @__cxa_end_catch()
  ret i32 1

resume:
  resume { ptr, i32 } %lp
}

; CHECK-LABEL: <catcher>:
; CHECK:         callq
; CHECK-NEXT:      R_X86_64_PLT32 may_throw-0x4
; CHECK:         movq %rax, -0x{{[0-9a-f]+}}(%rbp)
; CHECK-NEXT:    movq %rdx, -0x{{[0-9a-f]+}}(%rbp)
;; The type id of the only catch clause is 1.
; CHECK:         movl $0x1, %[[ID:.*]]
; CHECK-NEXT:    cmpl %[[ID]], %{{.*}}
; CHECK:           R_X86_64_PLT32 __cxa_begin_catch-0x4
;; As in the regular pipeline, no trap follows the noreturn _Unwind_Resume.
; CHECK:           R_X86_64_PLT32 _Unwind_Resume-0x4
; CHECK-NOT:     ud2

; FRAME: Augmentation:          "zPLR"
; FRAME: Personality Address:
; FRAME: LSDA Address:

; ELF: .gcc_except_table
; ELF: DW.ref.__gxx_personality_v0
