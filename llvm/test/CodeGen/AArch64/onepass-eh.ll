; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -onepass-codegen -onepass-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s --check-prefix=FRAME
; RUN: llvm-readelf -S -s %t.o | FileCheck %s --check-prefix=ELF

;; An invoke gets a call-site entry pointing at its landing pad, the exception
;; pointer and selector arrive in X0 and X1, and resume calls _Unwind_Resume.

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
; CHECK:         bl
; CHECK-NEXT:      R_AARCH64_CALL26 may_throw
; CHECK:         str x0, [x29, #0x[[#%x,SLOT:]]]
; CHECK-NEXT:    str x1, [x29, #0x[[#%x,SLOT+8]]]
;; The type id of the only catch clause is 1.
; CHECK:         mov w[[#ID:]], #0x1
; CHECK-NEXT:    cmp w{{[0-9]+}}, w[[#ID]]
; CHECK:           R_AARCH64_CALL26 __cxa_begin_catch
; CHECK:           R_AARCH64_CALL26 _Unwind_Resume
;; Both returns share the epilogue at the end.
; CHECK-NEXT:    ldp x29, x30, [sp], #
; CHECK-NEXT:    ret

; FRAME: Augmentation:          "zPLR"
; FRAME: Personality Address:
; FRAME: LSDA Address:
; FRAME: DW_CFA_def_cfa: W29 +[[#]]
; FRAME: DW_CFA_def_cfa: WSP +0
; FRAME-NEXT: DW_CFA_restore: W30
; FRAME-NEXT: DW_CFA_restore: W29

; ELF: .gcc_except_table
; ELF: DW.ref.__gxx_personality_v0
