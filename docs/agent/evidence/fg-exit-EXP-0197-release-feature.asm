
E:/LeviLauncher/versions/1.26.51.01/nvngx_dlssg.dll:     file format pei-x86-64


Disassembly of section .text:

0000000180013bb0 <.text+0x12bb0>:
   180013bb0:	40 53                	rex push %rbx
   180013bb2:	48 81 ec 60 02 00 00 	sub    $0x260,%rsp
   180013bb9:	48 8b 05 50 84 70 00 	mov    0x708450(%rip),%rax        # 0x18071c010
   180013bc0:	48 33 c4             	xor    %rsp,%rax
   180013bc3:	48 89 84 24 50 02 00 	mov    %rax,0x250(%rsp)
   180013bca:	00 
   180013bcb:	48 8b 94 24 68 02 00 	mov    0x268(%rsp),%rdx
   180013bd2:	00 
   180013bd3:	4c 8d 44 24 30       	lea    0x30(%rsp),%r8
   180013bd8:	48 8b d9             	mov    %rcx,%rbx
   180013bdb:	48 c7 44 24 30 00 00 	movq   $0x0,0x30(%rsp)
   180013be2:	00 00 
   180013be4:	b9 06 00 00 00       	mov    $0x6,%ecx
   180013be9:	ff 15 29 f5 07 00    	call   *0x7f529(%rip)        # 0x180093118
   180013bef:	85 c0                	test   %eax,%eax
   180013bf1:	75 29                	jne    0x180013c1c
   180013bf3:	4c 8d 0d de 1c 08 00 	lea    0x81cde(%rip),%r9        # 0x1800958d8
   180013bfa:	ba 98 00 00 00       	mov    $0x98,%edx
   180013bff:	4c 8d 05 9a 22 08 00 	lea    0x8229a(%rip),%r8        # 0x180095ea0
   180013c06:	48 8d 0d f3 20 08 00 	lea    0x820f3(%rip),%rcx        # 0x180095d00
   180013c0d:	e8 2e 9f ff ff       	call   0x18000db40
   180013c12:	b8 02 00 d0 ba       	mov    $0xbad00002,%eax
   180013c17:	e9 97 00 00 00       	jmp    0x180013cb3
   180013c1c:	48 8b 4c 24 30       	mov    0x30(%rsp),%rcx
   180013c21:	48 8d 54 24 40       	lea    0x40(%rsp),%rdx
   180013c26:	41 b8 04 01 00 00    	mov    $0x104,%r8d
   180013c2c:	ff 15 4e f4 07 00    	call   *0x7f44e(%rip)        # 0x180093080
   180013c32:	48 8d 15 57 1d 08 00 	lea    0x81d57(%rip),%rdx        # 0x180095990
   180013c39:	48 8d 4c 24 40       	lea    0x40(%rsp),%rcx
   180013c3e:	e8 cd a8 05 00       	call   0x18006e510
   180013c43:	48 85 c0             	test   %rax,%rax
   180013c46:	75 30                	jne    0x180013c78
   180013c48:	48 8d 44 24 40       	lea    0x40(%rsp),%rax
   180013c4d:	ba 98 00 00 00       	mov    $0x98,%edx
   180013c52:	4c 8d 0d 4f 1d 08 00 	lea    0x81d4f(%rip),%r9        # 0x1800959a8
   180013c59:	48 89 44 24 20       	mov    %rax,0x20(%rsp)
   180013c5e:	4c 8d 05 3b 22 08 00 	lea    0x8223b(%rip),%r8        # 0x180095ea0
   180013c65:	48 8d 0d 94 20 08 00 	lea    0x82094(%rip),%rcx        # 0x180095d00
   180013c6c:	e8 cf 9e ff ff       	call   0x18000db40
   180013c71:	b8 02 00 d0 ba       	mov    $0xbad00002,%eax
   180013c76:	eb 3b                	jmp    0x180013cb3
   180013c78:	48 85 db             	test   %rbx,%rbx
   180013c7b:	75 26                	jne    0x180013ca3
   180013c7d:	4c 8d 0d bc 1e 08 00 	lea    0x81ebc(%rip),%r9        # 0x180095b40
   180013c84:	ba 9c 00 00 00       	mov    $0x9c,%edx
   180013c89:	4c 8d 05 10 22 08 00 	lea    0x82210(%rip),%r8        # 0x180095ea0
   180013c90:	48 8d 0d 69 20 08 00 	lea    0x82069(%rip),%rcx        # 0x180095d00
   180013c97:	e8 74 9c ff ff       	call   0x18000d910
   180013c9c:	b8 05 00 d0 ba       	mov    $0xbad00005,%eax
   180013ca1:	eb 10                	jmp    0x180013cb3
   180013ca3:	e8 48 e2 ff ff       	call   0x180011ef0
   180013ca8:	48 8b d3             	mov    %rbx,%rdx
   180013cab:	48 8b c8             	mov    %rax,%rcx
   180013cae:	e8 6d 2e 00 00       	call   0x180016b20
   180013cb3:	48 8b 8c 24 50 02 00 	mov    0x250(%rsp),%rcx
   180013cba:	00 
   180013cbb:	48 33 cc             	xor    %rsp,%rcx
   180013cbe:	e8 dd 81 05 00       	call   0x18006bea0
   180013cc3:	48 81 c4 60 02 00 00 	add    $0x260,%rsp
   180013cca:	5b                   	pop    %rbx
   180013ccb:	c3                   	ret
   180013ccc:	cc                   	int3
   180013ccd:	cc                   	int3
   180013cce:	cc                   	int3
   180013ccf:	cc                   	int3
