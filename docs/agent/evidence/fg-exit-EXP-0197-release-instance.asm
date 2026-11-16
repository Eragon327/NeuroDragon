
E:/LeviLauncher/versions/1.26.51.01/nvngx_dlssg.dll:     file format pei-x86-64


Disassembly of section .text:

0000000180016b20 <.text+0x15b20>:
   180016b20:	48 89 5c 24 10       	mov    %rbx,0x10(%rsp)
   180016b25:	48 89 6c 24 18       	mov    %rbp,0x18(%rsp)
   180016b2a:	48 89 74 24 20       	mov    %rsi,0x20(%rsp)
   180016b2f:	57                   	push   %rdi
   180016b30:	41 54                	push   %r12
   180016b32:	41 55                	push   %r13
   180016b34:	41 56                	push   %r14
   180016b36:	41 57                	push   %r15
   180016b38:	48 83 ec 40          	sub    $0x40,%rsp
   180016b3c:	48 8b fa             	mov    %rdx,%rdi
   180016b3f:	4c 8b f1             	mov    %rcx,%r14
   180016b42:	48 8d b1 90 00 00 00 	lea    0x90(%rcx),%rsi
   180016b49:	48 89 74 24 30       	mov    %rsi,0x30(%rsp)
   180016b4e:	c6 44 24 38 00       	movb   $0x0,0x38(%rsp)
   180016b53:	48 8b ce             	mov    %rsi,%rcx
   180016b56:	e8 2d 40 05 00       	call   0x18006ab88
   180016b5b:	85 c0                	test   %eax,%eax
   180016b5d:	0f 85 36 04 00 00    	jne    0x180016f99
   180016b63:	38 86 e4 00 00 00    	cmp    %al,0xe4(%rsi)
   180016b69:	74 1d                	je     0x180016b88
   180016b6b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
   180016b70:	48 8b d6             	mov    %rsi,%rdx
   180016b73:	48 8d 8e 98 00 00 00 	lea    0x98(%rsi),%rcx
   180016b7a:	e8 ad 43 05 00       	call   0x18006af2c
   180016b7f:	80 be e4 00 00 00 00 	cmpb   $0x0,0xe4(%rsi)
   180016b86:	75 e8                	jne    0x180016b70
   180016b88:	c6 86 e4 00 00 00 01 	movb   $0x1,0xe4(%rsi)
   180016b8f:	83 be e0 00 00 00 00 	cmpl   $0x0,0xe0(%rsi)
   180016b96:	76 1d                	jbe    0x180016bb5
   180016b98:	0f 1f 84 00 00 00 00 	nopl   0x0(%rax,%rax,1)
   180016b9f:	00 
   180016ba0:	48 8b d6             	mov    %rsi,%rdx
   180016ba3:	48 8d 4e 50          	lea    0x50(%rsi),%rcx
   180016ba7:	e8 80 43 05 00       	call   0x18006af2c
   180016bac:	83 be e0 00 00 00 00 	cmpl   $0x0,0xe0(%rsi)
   180016bb3:	77 eb                	ja     0x180016ba0
   180016bb5:	48 8b ce             	mov    %rsi,%rcx
   180016bb8:	e8 ef 3f 05 00       	call   0x18006abac
   180016bbd:	b0 01                	mov    $0x1,%al
   180016bbf:	88 44 24 38          	mov    %al,0x38(%rsp)
   180016bc3:	48 8b 3f             	mov    (%rdi),%rdi
   180016bc6:	48 89 7c 24 70       	mov    %rdi,0x70(%rsp)
   180016bcb:	48 8b d7             	mov    %rdi,%rdx
   180016bce:	48 c1 ea 18          	shr    $0x18,%rdx
   180016bd2:	4c 8b c7             	mov    %rdi,%r8
   180016bd5:	49 c1 e8 10          	shr    $0x10,%r8
   180016bd9:	4c 8b cf             	mov    %rdi,%r9
   180016bdc:	49 c1 e9 08          	shr    $0x8,%r9
   180016be0:	48 8b c7             	mov    %rdi,%rax
   180016be3:	48 c1 e8 20          	shr    $0x20,%rax
   180016be7:	83 f8 0b             	cmp    $0xb,%eax
   180016bea:	74 14                	je     0x180016c00
   180016bec:	ba 54 01 00 00       	mov    $0x154,%edx
   180016bf1:	e9 94 00 00 00       	jmp    0x180016c8a
   180016bf6:	66 66 0f 1f 84 00 00 	data16 nopw 0x0(%rax,%rax,1)
   180016bfd:	00 00 00 
   180016c00:	40 0f b6 cf          	movzbl %dil,%ecx
   180016c04:	49 bd 25 23 22 84 e4 	movabs $0xcbf29ce484222325,%r13
   180016c0b:	9c f2 cb 
   180016c0e:	49 33 cd             	xor    %r13,%rcx
   180016c11:	49 bc b3 01 00 00 00 	movabs $0x100000001b3,%r12
   180016c18:	01 00 00 
   180016c1b:	49 0f af cc          	imul   %r12,%rcx
   180016c1f:	41 0f b6 c1          	movzbl %r9b,%eax
   180016c23:	48 33 c8             	xor    %rax,%rcx
   180016c26:	49 0f af cc          	imul   %r12,%rcx
   180016c2a:	41 0f b6 c0          	movzbl %r8b,%eax
   180016c2e:	48 33 c8             	xor    %rax,%rcx
   180016c31:	49 0f af cc          	imul   %r12,%rcx
   180016c35:	0f b6 c2             	movzbl %dl,%eax
   180016c38:	48 33 c8             	xor    %rax,%rcx
   180016c3b:	49 0f af cc          	imul   %r12,%rcx
   180016c3f:	49 8b 5e 78          	mov    0x78(%r14),%rbx
   180016c43:	48 8b c3             	mov    %rbx,%rax
   180016c46:	48 23 c1             	and    %rcx,%rax
   180016c49:	4d 8b 7e 60          	mov    0x60(%r14),%r15
   180016c4d:	48 03 c0             	add    %rax,%rax
   180016c50:	49 8b 4c c7 08       	mov    0x8(%r15,%rax,8),%rcx
   180016c55:	4d 8b 56 50          	mov    0x50(%r14),%r10
   180016c59:	49 3b ca             	cmp    %r10,%rcx
   180016c5c:	74 19                	je     0x180016c77
   180016c5e:	49 8b 04 c7          	mov    (%r15,%rax,8),%rax
   180016c62:	3b 79 10             	cmp    0x10(%rcx),%edi
   180016c65:	74 12                	je     0x180016c79
   180016c67:	48 3b c8             	cmp    %rax,%rcx
   180016c6a:	74 0b                	je     0x180016c77
   180016c6c:	48 8b 49 08          	mov    0x8(%rcx),%rcx
   180016c70:	3b 79 10             	cmp    0x10(%rcx),%edi
   180016c73:	75 f2                	jne    0x180016c67
   180016c75:	eb 02                	jmp    0x180016c79
   180016c77:	33 c9                	xor    %ecx,%ecx
   180016c79:	48 85 c9             	test   %rcx,%rcx
   180016c7c:	49 0f 44 ca          	cmove  %r10,%rcx
   180016c80:	49 3b ca             	cmp    %r10,%rcx
   180016c83:	75 29                	jne    0x180016cae
   180016c85:	ba 5c 01 00 00       	mov    $0x15c,%edx
   180016c8a:	4c 8d 0d 37 f8 07 00 	lea    0x7f837(%rip),%r9        # 0x1800964c8
   180016c91:	4c 8d 05 70 f8 07 00 	lea    0x7f870(%rip),%r8        # 0x180096508
   180016c98:	48 8d 0d 41 f6 07 00 	lea    0x7f641(%rip),%rcx        # 0x1800962e0
   180016c9f:	e8 6c 6c ff ff       	call   0x18000d910
   180016ca4:	bf 04 00 d0 ba       	mov    $0xbad00004,%edi
   180016ca9:	e9 9c 02 00 00       	jmp    0x180016f4a
   180016cae:	44                   	rex.R
   180016caf:	8b                   	.byte 0x8b
