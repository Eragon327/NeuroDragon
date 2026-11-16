
E:/LeviLauncher/versions/1.26.51.01/nvngx_dlssg.dll:     file format pei-x86-64


Disassembly of section .text:

0000000180011ef0 <.text+0x10ef0>:
   180011ef0:	40 53                	rex push %rbx
   180011ef2:	48 83 ec 20          	sub    $0x20,%rsp
   180011ef6:	8b 0d ec 40 72 00    	mov    0x7240ec(%rip),%ecx        # 0x180735fe8
   180011efc:	65 48 8b 04 25 58 00 	mov    %gs:0x58,%rax
   180011f03:	00 00 
   180011f05:	ba 04 00 00 00       	mov    $0x4,%edx
   180011f0a:	48 8b 0c c8          	mov    (%rax,%rcx,8),%rcx
   180011f0e:	8b 04 0a             	mov    (%rdx,%rcx,1),%eax
   180011f11:	39 05 41 d6 71 00    	cmp    %eax,0x71d641(%rip)        # 0x18072f558
   180011f17:	7f 0d                	jg     0x180011f26
   180011f19:	48 8d 05 c0 d4 71 00 	lea    0x71d4c0(%rip),%rax        # 0x18072f3e0
   180011f20:	48 83 c4 20          	add    $0x20,%rsp
   180011f24:	5b                   	pop    %rbx
   180011f25:	c3                   	ret
   180011f26:	48 8d 0d 2b d6 71 00 	lea    0x71d62b(%rip),%rcx        # 0x18072f558
   180011f2d:	e8 46 9d 05 00       	call   0x18006bc78
   180011f32:	83 3d 1f d6 71 00 ff 	cmpl   $0xffffffff,0x71d61f(%rip)        # 0x18072f558
   180011f39:	75 de                	jne    0x180011f19
   180011f3b:	48 8d 1d 9e d4 71 00 	lea    0x71d49e(%rip),%rbx        # 0x18072f3e0
   180011f42:	48 8b cb             	mov    %rbx,%rcx
   180011f45:	e8 96 f7 ff ff       	call   0x1800116e0
   180011f4a:	48 8d 0d 0f 08 08 00 	lea    0x8080f(%rip),%rcx        # 0x180092760
   180011f51:	e8 7a 9b 05 00       	call   0x18006bad0
   180011f56:	90                   	nop
   180011f57:	48 8d 0d fa d5 71 00 	lea    0x71d5fa(%rip),%rcx        # 0x18072f558
   180011f5e:	e8 b5 9c 05 00       	call   0x18006bc18
   180011f63:	48 8b c3             	mov    %rbx,%rax
   180011f66:	48 83 c4 20          	add    $0x20,%rsp
   180011f6a:	5b                   	pop    %rbx
   180011f6b:	c3                   	ret
   180011f6c:	cc                   	int3
   180011f6d:	cc                   	int3
   180011f6e:	cc                   	int3
   180011f6f:	cc                   	int3
