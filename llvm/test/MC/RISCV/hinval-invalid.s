# RUN: llvm-mc -triple=riscv32 -mattr=+svinval --verify=no-h %s
# RUN: llvm-mc -triple=riscv64 -mattr=+svinval --verify=no-h %s
# RUN: llvm-mc -triple=riscv32 -mattr=+h --verify=no-svinval %s
# RUN: llvm-mc -triple=riscv64 -mattr=+h --verify=no-svinval %s

hinval.vvma a0, a1
# no-h-error@-1:1 {{instruction requires the following: 'H' (Hypervisor)}}
# no-svinval-error@-2:1 {{instruction requires the following: 'Svinval' (Fine-Grained Address-Translation Cache Invalidation)}}

hinval.gvma a0, a1
# no-h-error@-1:1 {{instruction requires the following: 'H' (Hypervisor)}}
# no-svinval-error@-2:1 {{instruction requires the following: 'Svinval' (Fine-Grained Address-Translation Cache Invalidation)}}
