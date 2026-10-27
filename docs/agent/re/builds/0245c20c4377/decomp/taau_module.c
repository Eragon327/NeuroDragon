/* ND-RE v2 - generated, do not edit
 * key          : taau_module
 * build        : 0245c20c4377 (1.26.51.01)
 * sha256       : 0245c20c4377d74a25f3644bf441b460d6b3b45a88617387edccaacd27418377
 * exe          : E:\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe
 * idb          : D:\Documents\DecompileData\version\minecraft-windows-26.51.01\Minecraft.Windows.exe.i64
 * resolve      : sig: 55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 0D 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 C0 0C 00 00 48 C7 85 B8 0C 00 00 FE FF FF FF 4C 89 CB
 * rva          : 0xb74f860
 * file_off     : 0xb74ec60
 * ea           : 0x14b74f860
 * func         : sub_14B74F860  [0xb74f860 - 0xb751693, 7731 bytes]
 * proto        : void sub_14B74F860()
 * why          : TAAU 入口的完整伪代码：确认 12 个参数怎么取、BindFramebufferColor[4] 的顺序、以及 colorTargets 的判据
 * requested_by : agent
 * served       : 2026-10-27T13:16:15.338+08:00 by re_serve.py v2 on IDA 9.3
 */

void __fastcall sub_14B74F860(
        _QWORD *a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        unsigned __int16 *a5,
        __int64 a6,
        int *a7,
        __int64 a8,
        __int64 a9,
        __int64 a10,
        __int64 a11,
        __int64 a12)
{
  __int64 v15; // rax
  __int64 v16; // rcx
  __int64 v17; // rcx
  __int64 v18; // rax
  volatile signed __int32 *v19; // rbx
  volatile signed __int32 *v20; // rbx
  __int64 v21; // rsi
  __int64 v22; // rdi
  int v23; // r14d
  unsigned __int64 v24; // rdx
  char v25; // al
  __int64 (__fastcall ***v26)(); // rcx
  __int64 (__fastcall **v27)(); // rcx
  __int64 v28; // rcx
  __int64 v29; // rcx
  _QWORD *v30; // rbx
  __int64 v31; // r14
  __int64 v32; // rbx
  _QWORD *v33; // r9
  __int64 v34; // r15
  __int64 v35; // rsi
  unsigned __int64 v36; // rax
  __int128 v37; // xmm0
  __int64 v38; // rdx
  __int64 v39; // rsi
  __m256i *v40; // rdi
  __int64 v41; // r12
  __int64 v42; // rdx
  _QWORD *v43; // rbx
  __int64 v44; // rsi
  __int64 (__fastcall *v45)(void *, __int64); // rax
  __int64 v46; // r13
  unsigned __int64 v47; // r15
  _QWORD *v48; // rcx
  _QWORD *v49; // rsi
  __int64 v50; // rax
  _QWORD *v51; // r14
  __int64 v52; // r8
  __int64 v53; // rdi
  unsigned __int64 v54; // rax
  unsigned __int64 v55; // r12
  unsigned __int64 v56; // rdx
  unsigned __int64 v57; // r12
  __int64 v58; // r13
  __int64 v59; // rbx
  unsigned __int64 v60; // rdx
  _QWORD *v61; // rax
  _QWORD *v62; // rcx
  __int64 v63; // rcx
  __int64 v64; // r8
  __int64 v65; // rax
  __int64 v66; // r8
  __int64 v67; // r9
  __int64 v68; // rcx
  __int64 v69; // r8
  __int64 v70; // r8
  _QWORD *v71; // rsi
  __int64 v72; // r14
  unsigned __int128 *v73; // r9
  _QWORD *v74; // rbx
  unsigned __int64 v75; // rax
  __int64 v76; // rcx
  __int64 v77; // rcx
  unsigned __int64 v78; // rcx
  unsigned __int64 v79; // rcx
  __int64 v80; // rcx
  __int64 v81; // rcx
  __int64 v82; // rcx
  __int64 v83; // rbx
  __int64 v84; // rsi
  volatile signed __int32 *v85; // rbx
  __int64 *v86; // rdx
  __int64 *i; // rax
  __int64 v88; // rcx
  __int64 **v89; // rcx
  __m256i *v90; // rdi
  __int64 v91; // rdx
  __int64 v92; // r8
  __m256i *v93; // rdi
  __int64 v94; // rdx
  __int64 v95; // r8
  __m256i *v96; // rdi
  __int64 v97; // rdx
  __int64 v98; // r8
  __m256i *v99; // rbx
  unsigned __int64 v100; // rdx
  __int64 v101; // rcx
  __int64 v102; // rcx
  __int64 v103; // rcx
  __int8 *v104; // rdi
  __int64 v105; // r8
  __int8 *v106; // rdi
  __int64 v107; // r8
  __int8 *v108; // rdi
  __int64 v109; // r8
  unsigned __int64 v110; // r8
  __int64 v111; // r8
  __m256i v112; // [rsp+30h] [rbp-50h] BYREF
  _BYTE v113[664]; // [rsp+50h] [rbp-30h] BYREF
  _BYTE v114[56]; // [rsp+2E8h] [rbp+268h] BYREF
  _BYTE *v115; // [rsp+320h] [rbp+2A0h]
  __int64 v116; // [rsp+328h] [rbp+2A8h]
  _BYTE v117[120]; // [rsp+330h] [rbp+2B0h] BYREF
  __int64 *v118; // [rsp+3A8h] [rbp+328h]
  __m256i v119; // [rsp+3B0h] [rbp+330h] BYREF
  _BYTE v120[664]; // [rsp+3D0h] [rbp+350h] BYREF
  _BYTE v121[56]; // [rsp+668h] [rbp+5E8h] BYREF
  _BYTE *v122; // [rsp+6A0h] [rbp+620h]
  __int64 v123; // [rsp+6A8h] [rbp+628h] BYREF
  unsigned __int128 v124; // [rsp+6B0h] [rbp+630h] BYREF
  __int128 v125; // [rsp+6C0h] [rbp+640h]
  __int64 v126; // [rsp+6D0h] [rbp+650h]
  _OWORD v127[2]; // [rsp+6D8h] [rbp+658h] BYREF
  __int64 v128; // [rsp+6F8h] [rbp+678h]
  _OWORD v129[2]; // [rsp+700h] [rbp+680h] BYREF
  __int64 v130; // [rsp+720h] [rbp+6A0h]
  _OWORD v131[2]; // [rsp+728h] [rbp+6A8h] BYREF
  __int64 v132; // [rsp+748h] [rbp+6C8h]
  __int128 v133; // [rsp+750h] [rbp+6D0h] BYREF
  __int64 v134; // [rsp+760h] [rbp+6E0h]
  __int128 v135; // [rsp+770h] [rbp+6F0h] BYREF
  __int128 v136; // [rsp+780h] [rbp+700h]
  __int128 v137; // [rsp+790h] [rbp+710h]
  __int64 v138; // [rsp+7A0h] [rbp+720h] BYREF
  _QWORD v139[7]; // [rsp+7A8h] [rbp+728h] BYREF
  _QWORD *v140; // [rsp+7E0h] [rbp+760h]
  __int64 *v141; // [rsp+7E8h] [rbp+768h] BYREF
  __int64 **v142; // [rsp+7F0h] [rbp+770h]
  int v143; // [rsp+7F8h] [rbp+778h] BYREF
  __int128 v144; // [rsp+800h] [rbp+780h]
  __int128 v145; // [rsp+810h] [rbp+790h]
  __int128 v146; // [rsp+820h] [rbp+7A0h]
  __int128 v147; // [rsp+830h] [rbp+7B0h]
  __int64 v148; // [rsp+840h] [rbp+7C0h]
  char v149[8]; // [rsp+848h] [rbp+7C8h] BYREF
  __int128 v150; // [rsp+850h] [rbp+7D0h] BYREF
  __int64 v151; // [rsp+860h] [rbp+7E0h]
  __int64 v152; // [rsp+868h] [rbp+7E8h]
  _BYTE v153[24]; // [rsp+870h] [rbp+7F0h] BYREF
  char v154; // [rsp+888h] [rbp+808h]
  __int128 v155; // [rsp+890h] [rbp+810h]
  __int64 v156; // [rsp+8A0h] [rbp+820h]
  _QWORD *v157; // [rsp+8A8h] [rbp+828h]
  int v158; // [rsp+8B0h] [rbp+830h]
  __int128 v159; // [rsp+8B4h] [rbp+834h]
  int v160; // [rsp+8C4h] [rbp+844h]
  __int128 v161; // [rsp+8C8h] [rbp+848h]
  int v162; // [rsp+8D8h] [rbp+858h]
  __int128 v163; // [rsp+8DCh] [rbp+85Ch]
  __int64 v164; // [rsp+8ECh] [rbp+86Ch]
  __int128 v165; // [rsp+8F4h] [rbp+874h]
  int v166; // [rsp+904h] [rbp+884h]
  __int128 v167; // [rsp+908h] [rbp+888h]
  int v168; // [rsp+918h] [rbp+898h]
  __int128 v169; // [rsp+91Ch] [rbp+89Ch]
  int v170; // [rsp+92Ch] [rbp+8ACh]
  __int64 v171; // [rsp+930h] [rbp+8B0h]
  unsigned __int64 v172; // [rsp+938h] [rbp+8B8h]
  __int64 v173; // [rsp+940h] [rbp+8C0h]
  __int128 v174; // [rsp+948h] [rbp+8C8h]
  int v175; // [rsp+958h] [rbp+8D8h]
  __int128 v176; // [rsp+95Ch] [rbp+8DCh]
  int v177; // [rsp+96Ch] [rbp+8ECh]
  __int128 v178; // [rsp+970h] [rbp+8F0h]
  __int64 v179; // [rsp+980h] [rbp+900h]
  __int128 v180; // [rsp+988h] [rbp+908h]
  int v181; // [rsp+998h] [rbp+918h]
  __int128 v182; // [rsp+99Ch] [rbp+91Ch]
  int v183; // [rsp+9ACh] [rbp+92Ch]
  __int128 v184; // [rsp+9B0h] [rbp+930h]
  int v185; // [rsp+9C0h] [rbp+940h]
  __int128 v186; // [rsp+9C4h] [rbp+944h]
  int v187; // [rsp+9D4h] [rbp+954h]
  char v188; // [rsp+9D8h] [rbp+958h]
  __int16 v189; // [rsp+9D9h] [rbp+959h]
  char v190; // [rsp+9DBh] [rbp+95Bh]
  int v191; // [rsp+9DCh] [rbp+95Ch]
  __int16 v192; // [rsp+9E0h] [rbp+960h]
  char v193; // [rsp+9E2h] [rbp+962h]
  int v194; // [rsp+9E4h] [rbp+964h]
  char v195; // [rsp+9E8h] [rbp+968h]
  _DWORD v196[7]; // [rsp+9ECh] [rbp+96Ch] BYREF
  char v197; // [rsp+A08h] [rbp+988h]
  __int16 v198; // [rsp+A09h] [rbp+989h]
  char v199; // [rsp+A0Bh] [rbp+98Bh]
  __int64 v200; // [rsp+A0Ch] [rbp+98Ch]
  int v201; // [rsp+A14h] [rbp+994h]
  char v202; // [rsp+A18h] [rbp+998h]
  __int16 v203; // [rsp+A19h] [rbp+999h]
  char v204; // [rsp+A1Bh] [rbp+99Bh]
  _WORD v205[7]; // [rsp+A1Ch] [rbp+99Ch] BYREF
  char v206; // [rsp+A2Ah] [rbp+9AAh]
  __int64 v207; // [rsp+A2Ch] [rbp+9ACh]
  __int64 v208; // [rsp+A34h] [rbp+9B4h]
  char v209; // [rsp+A3Ch] [rbp+9BCh]
  int v210; // [rsp+A40h] [rbp+9C0h]
  int v211; // [rsp+A44h] [rbp+9C4h]
  int v212; // [rsp+A48h] [rbp+9C8h]
  char v213; // [rsp+A4Ch] [rbp+9CCh]
  char v214; // [rsp+A4Dh] [rbp+9CDh]
  int v215; // [rsp+A50h] [rbp+9D0h]
  int v216; // [rsp+A54h] [rbp+9D4h]
  int v217; // [rsp+A58h] [rbp+9D8h]
  int v218; // [rsp+A5Ch] [rbp+9DCh]
  int v219; // [rsp+A60h] [rbp+9E0h]
  int v220; // [rsp+A64h] [rbp+9E4h]
  int v221; // [rsp+A68h] [rbp+9E8h]
  int v222; // [rsp+A6Ch] [rbp+9ECh]
  int v223; // [rsp+A70h] [rbp+9F0h]
  int v224; // [rsp+A74h] [rbp+9F4h]
  int v225; // [rsp+A78h] [rbp+9F8h]
  int v226; // [rsp+A7Ch] [rbp+9FCh]
  int v227; // [rsp+A80h] [rbp+A00h]
  int v228; // [rsp+A84h] [rbp+A04h]
  int v229; // [rsp+A88h] [rbp+A08h]
  int v230; // [rsp+A8Ch] [rbp+A0Ch]
  int v231; // [rsp+A90h] [rbp+A10h]
  int v232; // [rsp+A94h] [rbp+A14h]
  int v233; // [rsp+A98h] [rbp+A18h]
  int v234; // [rsp+A9Ch] [rbp+A1Ch]
  int v235; // [rsp+AA0h] [rbp+A20h]
  int v236; // [rsp+AA4h] [rbp+A24h]
  int v237; // [rsp+AA8h] [rbp+A28h]
  int v238; // [rsp+AACh] [rbp+A2Ch]
  int v239; // [rsp+AB0h] [rbp+A30h]
  int v240; // [rsp+AB4h] [rbp+A34h]
  int v241; // [rsp+AB8h] [rbp+A38h]
  int v242; // [rsp+ABCh] [rbp+A3Ch]
  int v243; // [rsp+AC0h] [rbp+A40h]
  int v244; // [rsp+AC4h] [rbp+A44h]
  int v245; // [rsp+AC8h] [rbp+A48h]
  int v246; // [rsp+ACCh] [rbp+A4Ch]
  int v247; // [rsp+AD0h] [rbp+A50h]
  int v248; // [rsp+AD4h] [rbp+A54h]
  int v249; // [rsp+AD8h] [rbp+A58h]
  int v250; // [rsp+ADCh] [rbp+A5Ch]
  int v251; // [rsp+AE0h] [rbp+A60h]
  int v252; // [rsp+AE4h] [rbp+A64h]
  _QWORD v253[2]; // [rsp+AF0h] [rbp+A70h] BYREF
  _QWORD v254[3]; // [rsp+B00h] [rbp+A80h] BYREF
  unsigned __int64 v255; // [rsp+B18h] [rbp+A98h]
  unsigned __int64 v256; // [rsp+B20h] [rbp+AA0h]
  __int64 v257; // [rsp+B28h] [rbp+AA8h]
  unsigned __int64 v258; // [rsp+B30h] [rbp+AB0h]
  __int64 v259; // [rsp+B38h] [rbp+AB8h]
  __int64 v260; // [rsp+B40h] [rbp+AC0h]
  _QWORD v261[3]; // [rsp+B48h] [rbp+AC8h] BYREF
  unsigned __int64 v262; // [rsp+B60h] [rbp+AE0h]
  __int64 v263; // [rsp+B68h] [rbp+AE8h]
  __int64 v264; // [rsp+B70h] [rbp+AF0h] BYREF
  unsigned __int64 v265; // [rsp+B78h] [rbp+AF8h]
  unsigned __int64 v266; // [rsp+B80h] [rbp+B00h]
  __int128 v267; // [rsp+B88h] [rbp+B08h]
  __int64 v268; // [rsp+BA0h] [rbp+B20h]
  unsigned __int128 v269; // [rsp+BA8h] [rbp+B28h]
  __int128 v270; // [rsp+BB8h] [rbp+B38h]
  __int64 v271; // [rsp+BC8h] [rbp+B48h]
  __int128 v272; // [rsp+BD0h] [rbp+B50h]
  __int128 v273; // [rsp+BE0h] [rbp+B60h]
  __int64 v274; // [rsp+BF0h] [rbp+B70h]
  __int128 v275; // [rsp+BF8h] [rbp+B78h]
  __int128 v276; // [rsp+C08h] [rbp+B88h]
  __int64 v277; // [rsp+C18h] [rbp+B98h]
  __int128 v278; // [rsp+C20h] [rbp+BA0h]
  __int128 v279; // [rsp+C30h] [rbp+BB0h]
  __int64 v280; // [rsp+C40h] [rbp+BC0h]
  __int128 v281; // [rsp+C48h] [rbp+BC8h]
  __int64 v282; // [rsp+C58h] [rbp+BD8h]
  __int64 v283; // [rsp+C60h] [rbp+BE0h] BYREF
  volatile signed __int32 *v284; // [rsp+C68h] [rbp+BE8h]
  __int64 v285; // [rsp+C70h] [rbp+BF0h]
  __int64 v286; // [rsp+C78h] [rbp+BF8h]
  __int64 v287; // [rsp+C80h] [rbp+C00h]
  __int64 v288; // [rsp+C88h] [rbp+C08h]
  __int64 v289; // [rsp+C90h] [rbp+C10h]
  __m256i v290; // [rsp+C98h] [rbp+C18h] BYREF
  _BYTE v291[7]; // [rsp+CBBh] [rbp+C3Bh]
  _BYTE v292[7]; // [rsp+CC2h] [rbp+C42h]
  _BYTE v293[7]; // [rsp+CC9h] [rbp+C49h]
  _QWORD v294[3]; // [rsp+CD0h] [rbp+C50h] BYREF
  unsigned __int64 v295; // [rsp+CE8h] [rbp+C68h]
  int v296; // [rsp+CF3h] [rbp+C73h]
  char v297; // [rsp+CF7h] [rbp+C77h]
  int v298; // [rsp+CF8h] [rbp+C78h]
  char v299; // [rsp+CFCh] [rbp+C7Ch]
  int v300; // [rsp+CFDh] [rbp+C7Dh]
  char v301; // [rsp+D01h] [rbp+C81h]
  int v302; // [rsp+D02h] [rbp+C82h]
  char v303; // [rsp+D06h] [rbp+C86h]
  __int16 v304; // [rsp+D07h] [rbp+C87h]
  char v305; // [rsp+D09h] [rbp+C89h]
  __int16 v306; // [rsp+D0Ah] [rbp+C8Ah]
  char v307; // [rsp+D0Ch] [rbp+C8Ch]
  __int16 v308; // [rsp+D0Dh] [rbp+C8Dh]
  char v309; // [rsp+D0Fh] [rbp+C8Fh]
  __int128 v310; // [rsp+D10h] [rbp+C90h]
  __int64 v311; // [rsp+D20h] [rbp+CA0h]
  _QWORD *v312; // [rsp+D28h] [rbp+CA8h]
  _QWORD *v313; // [rsp+D30h] [rbp+CB0h]
  __int64 v314; // [rsp+D38h] [rbp+CB8h]

  v314 = -2;
  v313 = a1;
  if ( dword_151F2ED70 > *(_DWORD *)(*((_QWORD *)NtCurrentTeb()->ThreadLocalStoragePointer + (unsigned int)TlsIndex)
                                   + 4LL) )
  {
    sub_14E024890(&dword_151F2ED70);
    if ( dword_151F2ED70 == -1 )
    {
      v90 = &v119;
      sub_140079200(&v119, "InputFinalColor");
      if ( v119.m256i_i64[3] >= 0x10uLL )
        v90 = (__m256i *)v119.m256i_i64[0];
      if ( v119.m256i_i64[2] )
      {
        if ( v119.m256i_i64[2] >= 4uLL )
        {
          v91 = 0xCBF29CE484222325uLL;
          v92 = 0;
          do
          {
            v91 = 0x100000001B3LL
                * ((0x100000001B3LL
                  * ((0x100000001B3LL * ((0x100000001B3LL * (v91 ^ v90->m256i_u8[v92])) ^ v90->m256i_u8[v92 + 1]))
                   ^ v90->m256i_u8[v92 + 2]))
                 ^ v90->m256i_u8[v92 + 3]);
            v92 += 4;
          }
          while ( (v119.m256i_i64[2] & 0xFFFFFFFFFFFFFFFCuLL) != v92 );
        }
        else
        {
          v91 = 0xCBF29CE484222325uLL;
          v92 = 0;
        }
        v101 = v91;
        if ( (v119.m256i_i8[16] & 3) != 0 )
        {
          v104 = &v90->m256i_i8[v92];
          v105 = 0;
          do
          {
            v101 = 0x100000001B3LL * (v91 ^ (unsigned __int8)v104[v105++]);
            v91 = v101;
          }
          while ( (v119.m256i_i8[16] & 3) != v105 );
        }
      }
      else
      {
        v101 = 0xCBF29CE484222325uLL;
      }
      qword_151F2ED68 = v101;
      sub_140073350(&v119, v91);
      sub_14E024950(&dword_151F2ED70);
    }
  }
  if ( dword_151F2ED80 > *(_DWORD *)(*((_QWORD *)NtCurrentTeb()->ThreadLocalStoragePointer + (unsigned int)TlsIndex)
                                   + 4LL) )
  {
    sub_14E024890(&dword_151F2ED80);
    if ( dword_151F2ED80 == -1 )
    {
      v93 = &v119;
      sub_140079200(&v119, "InputBufferMotionVectors");
      if ( v119.m256i_i64[3] >= 0x10uLL )
        v93 = (__m256i *)v119.m256i_i64[0];
      if ( v119.m256i_i64[2] )
      {
        if ( v119.m256i_i64[2] >= 4uLL )
        {
          v94 = 0xCBF29CE484222325uLL;
          v95 = 0;
          do
          {
            v94 = 0x100000001B3LL
                * ((0x100000001B3LL
                  * ((0x100000001B3LL * ((0x100000001B3LL * (v94 ^ v93->m256i_u8[v95])) ^ v93->m256i_u8[v95 + 1]))
                   ^ v93->m256i_u8[v95 + 2]))
                 ^ v93->m256i_u8[v95 + 3]);
            v95 += 4;
          }
          while ( (v119.m256i_i64[2] & 0xFFFFFFFFFFFFFFFCuLL) != v95 );
        }
        else
        {
          v94 = 0xCBF29CE484222325uLL;
          v95 = 0;
        }
        v102 = v94;
        if ( (v119.m256i_i8[16] & 3) != 0 )
        {
          v106 = &v93->m256i_i8[v95];
          v107 = 0;
          do
          {
            v102 = 0x100000001B3LL * (v94 ^ (unsigned __int8)v106[v107++]);
            v94 = v102;
          }
          while ( (v119.m256i_i8[16] & 3) != v107 );
        }
      }
      else
      {
        v102 = 0xCBF29CE484222325uLL;
      }
      qword_151F2ED78 = v102;
      sub_140073350(&v119, v94);
      sub_14E024950(&dword_151F2ED80);
    }
  }
  if ( dword_151F2ED90 > *(_DWORD *)(*((_QWORD *)NtCurrentTeb()->ThreadLocalStoragePointer + (unsigned int)TlsIndex)
                                   + 4LL) )
  {
    sub_14E024890(&dword_151F2ED90);
    if ( dword_151F2ED90 == -1 )
    {
      v96 = &v119;
      sub_140079200(&v119, "InputTAAHistory");
      if ( v119.m256i_i64[3] >= 0x10uLL )
        v96 = (__m256i *)v119.m256i_i64[0];
      if ( v119.m256i_i64[2] )
      {
        if ( v119.m256i_i64[2] >= 4uLL )
        {
          v97 = 0xCBF29CE484222325uLL;
          v98 = 0;
          do
          {
            v97 = 0x100000001B3LL
                * ((0x100000001B3LL
                  * ((0x100000001B3LL * ((0x100000001B3LL * (v97 ^ v96->m256i_u8[v98])) ^ v96->m256i_u8[v98 + 1]))
                   ^ v96->m256i_u8[v98 + 2]))
                 ^ v96->m256i_u8[v98 + 3]);
            v98 += 4;
          }
          while ( (v119.m256i_i64[2] & 0xFFFFFFFFFFFFFFFCuLL) != v98 );
        }
        else
        {
          v97 = 0xCBF29CE484222325uLL;
          v98 = 0;
        }
        v103 = v97;
        if ( (v119.m256i_i8[16] & 3) != 0 )
        {
          v108 = &v96->m256i_i8[v98];
          v109 = 0;
          do
          {
            v103 = 0x100000001B3LL * (v97 ^ (unsigned __int8)v108[v109++]);
            v97 = v103;
          }
          while ( (v119.m256i_i8[16] & 3) != v109 );
        }
      }
      else
      {
        v103 = 0xCBF29CE484222325uLL;
      }
      qword_151F2ED88 = v103;
      sub_140073350(&v119, v97);
      sub_14E024950(&dword_151F2ED90);
    }
  }
  v138 = a2;
  v139[0] = &off_14E939310;
  v140 = v139;
  v141 = (__int64 *)&v141;
  v142 = &v141;
  v143 = 0;
  v144 = 0;
  v145 = 0;
  v146 = 0;
  v147 = 0;
  v148 = 0;
  ((void (__fastcall *)(int *, __int64))unk_14E5A4D60)(&v143, 2);
  sub_140905490(v149);
  if ( dword_151F2EDA8 > *(_DWORD *)(*((_QWORD *)NtCurrentTeb()->ThreadLocalStoragePointer + (unsigned int)TlsIndex)
                                   + 4LL) )
  {
    sub_14E024890(&dword_151F2EDA8);
    if ( dword_151F2EDA8 == -1 )
    {
      v135 = 0;
      v254[0] = "Upscaling";
      v254[1] = 9;
      sub_1452E10E0(&unk_151F2ED98, v254, &v135);
      sub_14E024950(&dword_151F2EDA8);
    }
  }
  *(_OWORD *)v119.m256i_i8 = 0;
  v112.m256i_i64[0] = (__int64)"Upscaling";
  v112.m256i_i64[1] = 9;
  ((void (__fastcall *)(__int64, __int64 *, void *, __int64, __m256i *, __m256i *))sub_1452F8AA0)(
    a3,
    &v283,
    &unk_151F2ED98,
    a4,
    &v112,
    &v119);
  v15 = v283;
  *(_OWORD *)&v119.m256i_u64[1] = 0;
  v119.m256i_i64[0] = (__int64)&off_14E775460;
  v16 = *(_QWORD *)(v283 + 16);
  if ( v16 )
  {
    _InterlockedIncrement((volatile signed __int32 *)(v16 + 8));
    v17 = *(_QWORD *)(v15 + 8);
    v18 = *(_QWORD *)(v15 + 16);
    v19 = (volatile signed __int32 *)v119.m256i_i64[2];
    v119.m256i_i64[1] = v17;
    v119.m256i_i64[2] = v18;
    if ( v19 )
    {
      if ( !_InterlockedDecrement(v19 + 2) )
      {
        (**(void (__fastcall ***)(volatile signed __int32 *))v19)(v19);
        if ( !_InterlockedDecrement(v19 + 3) )
          (*(void (__fastcall **)(volatile signed __int32 *))(*(_QWORD *)v19 + 8LL))(v19);
      }
    }
  }
  else
  {
    *(_OWORD *)&v119.m256i_u64[1] = *(unsigned __int64 *)(v283 + 8);
  }
  v116 = **(_QWORD **)(v119.m256i_i64[1] + 24);
  sub_14DFF3040(v117, *(_QWORD *)(v116 + 368), v138);
  v118 = &v138;
  v119.m256i_i64[0] = (__int64)&off_14E775460;
  v20 = (volatile signed __int32 *)v119.m256i_i64[2];
  if ( v119.m256i_i64[2] )
  {
    if ( !_InterlockedDecrement((volatile signed __int32 *)(v119.m256i_i64[2] + 8)) )
    {
      (**(void (__fastcall ***)(volatile signed __int32 *))v20)(v20);
      if ( !_InterlockedDecrement(v20 + 3) )
        (*(void (__fastcall **)(volatile signed __int32 *))(*(_QWORD *)v20 + 8LL))(v20);
    }
  }
  if ( dword_151F2EDB8 > *(_DWORD *)(*((_QWORD *)NtCurrentTeb()->ThreadLocalStoragePointer + (unsigned int)TlsIndex)
                                   + 4LL) )
  {
    sub_14E024890(&dword_151F2EDB8);
    if ( dword_151F2EDB8 == -1 )
    {
      v99 = &v119;
      sub_140079200(&v119, "Color0");
      if ( v119.m256i_i64[3] >= 0x10uLL )
        v99 = (__m256i *)v119.m256i_i64[0];
      if ( v119.m256i_i64[2] )
      {
        if ( v119.m256i_i64[2] >= 8uLL )
        {
          v110 = v119.m256i_i64[2] & 0xFFFFFFFFFFFFFFF8uLL;
          v100 = 0xCBF29CE484222325uLL;
          do
          {
            v100 = v99->m256i_u8[7]
                 ^ (0x100000001B3LL
                  * (v99->m256i_u8[6]
                   ^ (0x100000001B3LL
                    * (v99->m256i_u8[5]
                     ^ (0x100000001B3LL
                      * (v99->m256i_u8[4]
                       ^ (0x100000001B3LL
                        * (v99->m256i_u8[3]
                         ^ (0x100000001B3LL
                          * (v99->m256i_u8[2]
                           ^ (0x100000001B3LL
                            * (v99->m256i_u8[1] ^ (0x100000001B3LL * (v99->m256i_u8[0] ^ (0x100000001B3LL * v100)))))))))))))));
            v99 = (__m256i *)((char *)v99 + 8);
            v110 -= 8LL;
          }
          while ( v110 );
        }
        else
        {
          v100 = 0xCBF29CE484222325uLL;
        }
        if ( (v119.m256i_i8[16] & 7) != 0 )
        {
          v111 = 0;
          do
            v100 = v99->m256i_u8[v111++] ^ (0x100000001B3LL * v100);
          while ( (v119.m256i_i8[16] & 7) != v111 );
        }
      }
      else
      {
        v100 = 0xCBF29CE484222325uLL;
      }
      qword_151F2EDB0 = v100;
      sub_140073350(&v119, v100);
      sub_14E024950(&dword_151F2EDB8);
    }
  }
  *(_QWORD *)&v310 = qword_151F2EDB0;
  *((_QWORD *)&v310 + 1) = *(_QWORD *)(a10 + 8);
  v285 = qword_151F2ED68;
  v311 = *(_QWORD *)(a9 + 8);
  v286 = *(_QWORD *)(a12 + 8);
  v287 = *(_QWORD *)(a11 + 8);
  v136 = 0;
  v21 = *a5;
  v22 = *((unsigned int *)a5 + 1);
  v23 = a5[1];
  v137 = 0;
  v150 = 0;
  v151 = 0;
  v24 = 0xCCCCCCCCCCCCCCCDuLL * ((__int64)(*(_QWORD *)(a8 + 8) - *(_QWORD *)a8) >> 3);
  v288 = qword_151F2ED88;
  v289 = qword_151F2ED78;
  sub_14533C5F0(&v150, v24, a8, a8 + 8);
  v152 = *(_QWORD *)(a8 + 24);
  v154 = -1;
  v25 = *(_BYTE *)(a8 + 56);
  v26 = (__int64 (__fastcall ***)())(a8 + 32);
  switch ( v25 )
  {
    case 0:
      goto LABEL_24;
    case 1:
      v27 = *v26;
      goto LABEL_23;
    case 2:
      *(_OWORD *)v153 = *(_OWORD *)v26;
      goto LABEL_24;
    case 3:
      *(_QWORD *)v153 = &off_14E775460;
      *(_OWORD *)&v153[8] = 0;
      v28 = *(_QWORD *)(a8 + 48);
      if ( v28 )
      {
        _InterlockedIncrement((volatile signed __int32 *)(v28 + 8));
        v29 = *(_QWORD *)(a8 + 48);
      }
      else
      {
        v29 = 0;
      }
      *(_QWORD *)&v153[8] = *(_QWORD *)(a8 + 40);
      *(_QWORD *)&v153[16] = v29;
      v27 = &off_14E775460;
LABEL_23:
      *(_QWORD *)v153 = v27;
LABEL_24:
      v154 = v25;
      break;
    default:
      break;
  }
  v156 = *(_QWORD *)(a8 + 80);
  v155 = *(_OWORD *)(a8 + 64);
  v30 = (_QWORD *)(*(__int64 (__fastcall **)(void *, __int64))(*(_QWORD *)off_151805230 + 8LL))(off_151805230, 136);
  if ( !v30 )
    sub_1409049B0(136, 0);
  *v30 = v116;
  v312 = v30;
  ((void (__fastcall *)(_QWORD *, _BYTE *))unk_14DFF1BE0)(v30 + 1, v117);
  v31 = (unsigned int)(v23 << 16);
  v32 = v31 | v21 | (v22 << 32);
  v33 = v312;
  v312[16] = v118;
  v118 = 0;
  v157 = v33;
  v158 = 1065353216;
  v159 = 0;
  v160 = 1065353216;
  v161 = 0;
  v162 = 1065353216;
  v163 = 0;
  v164 = 0x3F8000003F800000LL;
  v165 = 0;
  v166 = 1065353216;
  v167 = 0;
  v168 = 1065353216;
  v169 = 0;
  v170 = 1065353216;
  v171 = v32;
  v172 = v21 | v31 | ((unsigned __int64)WORD1(v22) << 48) | ((unsigned __int64)(unsigned __int16)v22 << 32);
  v173 = 0x3F80000000000001LL;
  v174 = 0;
  v175 = 1065353216;
  v176 = 0;
  v177 = 1065353216;
  v178 = 0;
  v179 = 0x3F8000003F800000LL;
  v180 = 0;
  v181 = 1065353216;
  v182 = 0;
  v183 = 1065353216;
  v184 = 0;
  v185 = 1065353216;
  v186 = v137;
  v187 = 0;
  v188 = 15;
  v189 = v304;
  v190 = v305;
  v191 = 0;
  v192 = 0;
  v193 = 2;
  v194 = 0;
  v195 = 0;
  memset(v196, 0, sizeof(v196));
  v197 = 8;
  v198 = v306;
  v199 = v307;
  v200 = 0;
  v201 = 0;
  v202 = 8;
  v204 = v309;
  v203 = v308;
  memset(v205, 0, sizeof(v205));
  v206 = -1;
  v207 = *(_QWORD *)a5;
  v208 = off_14EAE3E90[*(unsigned int *)(a6 + 52)];
  v209 = *((_BYTE *)a7 + 224);
  v210 = *(_DWORD *)(a6 + 4);
  v211 = *(_DWORD *)(a6 + 44);
  v212 = *(_DWORD *)(a6 + 48);
  v213 = *(_BYTE *)(a6 + 42);
  v214 = *(_BYTE *)(a6 + 43);
  v215 = *a7;
  v216 = a7[1];
  v217 = a7[2];
  v218 = a7[3];
  v219 = a7[4];
  v220 = a7[5];
  v221 = a7[6];
  v222 = a7[7];
  v223 = a7[8];
  v224 = a7[9];
  v225 = a7[10];
  v226 = a7[11];
  v227 = a7[12];
  v228 = a7[13];
  v229 = a7[14];
  v230 = a7[15];
  v231 = a7[16];
  v232 = a7[17];
  v233 = a7[18];
  v234 = a7[19];
  v235 = a7[20];
  v236 = a7[21];
  v237 = a7[22];
  v238 = a7[23];
  v239 = a7[24];
  v240 = a7[25];
  v241 = a7[26];
  v242 = a7[27];
  v243 = a7[28];
  v244 = a7[29];
  v245 = a7[30];
  v246 = a7[31];
  v247 = a7[32];
  v248 = a7[33];
  v249 = a7[34];
  v250 = a7[35];
  v251 = a7[36];
  v252 = a7[37];
  BYTE5(v294[1]) = 0;
  HIWORD(v294[1]) = 0;
  v294[2] = 12;
  v295 = 15;
  strcpy((char *)v294, "TAAUpscaling");
  BYTE5(v261[1]) = 0;
  HIWORD(v261[1]) = 0;
  v262 = 15;
  v263 = 0;
  v261[2] = 12;
  strcpy((char *)v261, "TAAUpscaling");
  v260 = 0x33404347445A915DLL;
  v253[0] = v294;
  v253[1] = 12;
  sub_14530A6B0(&v290, v253);
  v112 = v290;
  v290.m256i_i64[2] = 0;
  v290.m256i_i64[3] = 15;
  v290.m256i_i8[0] = 0;
  sub_14B7520D0(v113, &v150);
  v115 = 0;
  v34 = (*(__int64 (__fastcall **)(void *, __int64))(*(_QWORD *)off_151805230 + 8LL))(off_151805230, 304);
  if ( !v34 )
    sub_1409049B0(304, 0);
  *(_QWORD *)v34 = &off_14EAEDF60;
  *(_DWORD *)(v34 + 8) = 1065353216;
  *(_OWORD *)(v34 + 12) = 0;
  *(_DWORD *)(v34 + 28) = 1065353216;
  *(_OWORD *)(v34 + 32) = 0;
  *(_DWORD *)(v34 + 48) = 1065353216;
  *(_OWORD *)(v34 + 52) = 0;
  *(_QWORD *)(v34 + 68) = 0x3F8000003F800000LL;
  *(_OWORD *)(v34 + 76) = 0;
  *(_DWORD *)(v34 + 92) = 1065353216;
  *(_OWORD *)(v34 + 96) = 0;
  *(_DWORD *)(v34 + 112) = 1065353216;
  *(_OWORD *)(v34 + 116) = 0;
  *(_DWORD *)(v34 + 132) = 1065353216;
  *(_QWORD *)(v34 + 136) = v32;
  *(_QWORD *)(v34 + 144) = v32;
  *(_QWORD *)(v34 + 152) = 0x3F80000000000001LL;
  *(_OWORD *)(v34 + 160) = 0;
  *(_DWORD *)(v34 + 176) = 1065353216;
  *(_OWORD *)(v34 + 180) = 0;
  *(_DWORD *)(v34 + 196) = 1065353216;
  *(_OWORD *)(v34 + 200) = 0;
  *(_QWORD *)(v34 + 216) = 0x3F8000003F800000LL;
  *(_OWORD *)(v34 + 224) = 0;
  *(_DWORD *)(v34 + 240) = 1065353216;
  *(_OWORD *)(v34 + 244) = 0;
  *(_DWORD *)(v34 + 260) = 1065353216;
  *(_OWORD *)(v34 + 264) = 0;
  *(_DWORD *)(v34 + 280) = 1065353216;
  *(_OWORD *)(v34 + 284) = v136;
  if ( v115 )
    sub_14009B4A0();
  v115 = (_BYTE *)v34;
  sub_1406ACB20(&v264, v294);
  v35 = v264;
  v36 = v265;
  v37 = v267;
  *(_QWORD *)&v267 = 0;
  *((_QWORD *)&v267 + 1) = 15;
  LOBYTE(v265) = 0;
  v259 = 0;
  v255 = v36;
  v256 = v266;
  v254[2] = v264;
  *(_DWORD *)((char *)&v273 + 1) = *(_DWORD *)v293;
  DWORD1(v273) = *(_DWORD *)&v293[3];
  *(_DWORD *)((char *)&v274 + 3) = v302;
  HIBYTE(v274) = v303;
  DWORD1(v276) = *(_DWORD *)&v292[3];
  *(_DWORD *)((char *)&v276 + 1) = *(_DWORD *)v292;
  HIBYTE(v277) = v301;
  *(_DWORD *)((char *)&v277 + 3) = v300;
  DWORD1(v279) = *(_DWORD *)&v291[3];
  *(_DWORD *)((char *)&v279 + 1) = *(_DWORD *)v291;
  HIBYTE(v280) = v299;
  *(_DWORD *)((char *)&v280 + 3) = v298;
  HIBYTE(v282) = v297;
  *(_DWORD *)((char *)&v282 + 3) = v296;
  v257 = 0;
  v258 = 15;
  LOBYTE(v255) = 0;
  *(_QWORD *)&v272 = v289;
  WORD4(v272) = 0;
  HIDWORD(v272) = 0;
  LOBYTE(v273) = 0;
  *((_QWORD *)&v273 + 1) = v287;
  BYTE2(v274) = 0;
  *(_QWORD *)&v275 = v288;
  WORD4(v275) = 0;
  HIDWORD(v275) = 0;
  LOBYTE(v276) = 0;
  *((_QWORD *)&v276 + 1) = v286;
  BYTE2(v277) = 0;
  *(_QWORD *)&v278 = v285;
  WORD4(v278) = 0;
  HIDWORD(v278) = 0;
  LOBYTE(v279) = 0;
  *((_QWORD *)&v279 + 1) = v311;
  BYTE2(v280) = 0;
  v281 = v310;
  BYTE2(v282) = 0;
  v271 = 0;
  v269 = __PAIR128__(v266, v36);
  v270 = v37;
  v268 = v264;
  v119 = v112;
  v112.m256i_i64[2] = 0;
  v112.m256i_i64[3] = 15;
  v112.m256i_i8[0] = 0;
  sub_14B7520D0(v120, v113);
  v122 = 0;
  if ( v115 )
  {
    if ( v115 == v114 )
    {
      v122 = (_BYTE *)(*(__int64 (__fastcall **)(_BYTE *, _BYTE *))(*(_QWORD *)v115 + 8LL))(v115, v121);
      if ( !v115 )
        goto LABEL_36;
      LOBYTE(v38) = v115 != v114;
      (*(void (__fastcall **)(_BYTE *, __int64))(*(_QWORD *)v115 + 32LL))(v115, v38);
    }
    else
    {
      v122 = v115;
    }
    v115 = 0;
  }
LABEL_36:
  v126 = 0;
  v124 = v269;
  v125 = v270;
  *(_QWORD *)&v270 = 0;
  *((_QWORD *)&v270 + 1) = 15;
  LOBYTE(v269) = 0;
  v123 = v35;
  v128 = v274;
  v127[1] = v273;
  v127[0] = v272;
  v130 = v277;
  v129[1] = v276;
  v129[0] = v275;
  v132 = v280;
  v131[1] = v279;
  v131[0] = v278;
  v134 = v282;
  v133 = v281;
  v39 = (*(__int64 (__fastcall **)(void *, __int64))(*(_QWORD *)off_151805230 + 8LL))(off_151805230, 832);
  if ( !v39 )
    sub_1409049B0(832, 0);
  *(_QWORD *)(v39 + 8) = 0x100000001LL;
  *(_QWORD *)v39 = &off_14E774A90;
  v40 = &v119;
  if ( v119.m256i_i64[3] >= 0x10uLL )
    v40 = (__m256i *)v119.m256i_i64[0];
  v41 = v119.m256i_i64[2];
  *(_OWORD *)(v39 + 40) = 0;
  *(_OWORD *)(v39 + 24) = 0;
  *(_QWORD *)(v39 + 56) = 15;
  *(_QWORD *)(v39 + 64) = 0;
  *(_QWORD *)(v39 + 16) = off_14EAEDF40;
  *(_QWORD *)(v39 + 128) = 0;
  if ( v122 )
  {
    if ( v122 == v121 )
    {
      *(_QWORD *)(v39 + 128) = (*(__int64 (__fastcall **)(_BYTE *, __int64))(*(_QWORD *)v122 + 8LL))(v122, v39 + 72);
      if ( !v122 )
        goto LABEL_46;
      LOBYTE(v42) = v122 != v121;
      (*(void (__fastcall **)(_BYTE *, __int64))(*(_QWORD *)v122 + 32LL))(v122, v42);
    }
    else
    {
      *(_QWORD *)(v39 + 128) = v122;
    }
    v122 = 0;
  }
LABEL_46:
  *((_QWORD *)&v310 + 1) = v39 + 72;
  *(_QWORD *)&v310 = v39 + 16;
  v312 = (_QWORD *)v39;
  sub_14B7520D0(v39 + 136, v120);
  v43 = v312;
  *((_OWORD *)v312 + 51) = 0;
  *((_OWORD *)v43 + 50) = 0;
  if ( v41 < 0 )
  {
    v311 = v39 + 136;
    sub_140075A30();
  }
  if ( (unsigned __int64)v41 > 0xF )
  {
    v311 = v39 + 136;
    v44 = 22;
    if ( (v41 | 0xFuLL) >= 0x17 )
      v44 = v41 | 0xF;
    v45 = *(__int64 (__fastcall **)(void *, __int64))(*(_QWORD *)off_151805230 + 8LL);
    if ( (v41 | 0xFuLL) < 0xFFF )
    {
      v47 = v45(off_151805230, v44 + 1);
      if ( !v47 )
      {
        v47 = 0;
        sub_1409049B0(v44 + 1, 0);
      }
    }
    else
    {
      v46 = v45(off_151805230, v44 + 40);
      if ( !v46 )
        sub_1409049B0(v44 + 40, 0);
      v47 = (v46 + 39) & 0xFFFFFFFFFFFFFFE0uLL;
      *(_QWORD *)(v47 - 8) = v46;
    }
    v43 = v312;
    v312[100] = v47;
    v43[102] = v41;
    v43[103] = v44;
    ((void (__fastcall *)(unsigned __int64, __m256i *, __int64))unk_14E5A6390)(v47, v40, v41);
    *(_BYTE *)(v47 + v41) = 0;
  }
  else
  {
    v43[102] = v41;
    v43[103] = 15;
    ((void (__fastcall *)(_QWORD *, __m256i *, __int64))unk_14E5A6390)(v43 + 100, v40, v41);
    *((_BYTE *)v43 + v41 + 800) = 0;
  }
  v48 = v313;
  v49 = (_QWORD *)v313[2];
  if ( v49 == (_QWORD *)v313[3] )
  {
    v51 = v313 + 1;
    v52 = (__int64)v43 + 12;
    v53 = (__int64)v49 - v313[1];
    v54 = v53 >> 4;
    v55 = (unsigned __int64)(v53 >> 4) >> 1;
    v56 = 0xFFFFFFFFFFFFFFFLL - v55;
    v57 = (v53 >> 4) + v55;
    if ( v57 <= (v53 >> 4) + 1 )
      v57 = v54 + 1;
    if ( v54 > v56 )
      v57 = 0xFFFFFFFFFFFFFFFLL;
    if ( v57 >> 60 )
    {
      *((_QWORD *)&v310 + 1) = v43 + 1;
      v311 = (__int64)v43 + 12;
      ((void (__noreturn *)(void))sub_140075A50)();
    }
    if ( v57 )
    {
      v311 = (__int64)v43 + 12;
      *((_QWORD *)&v310 + 1) = v43 + 1;
      if ( v57 < 0x100 )
      {
        v60 = (*(__int64 (__fastcall **)(void *, unsigned __int64, __int64))(*(_QWORD *)off_151805230 + 8LL))(
                off_151805230,
                16 * v57,
                v52);
        if ( !v60 )
        {
          sub_1409049B0(16 * v57, 0);
          v60 = 0;
        }
      }
      else
      {
        if ( v57 >= 0xFFFFFFFFFFFFFFELL )
          sub_140075A50(v43 + 1, v56, v52);
        v58 = 16 * v57 + 39;
        v59 = (*(__int64 (__fastcall **)(void *, __int64, __int64))(*(_QWORD *)off_151805230 + 8LL))(
                off_151805230,
                v58,
                v52);
        if ( !v59 )
          sub_1409049B0(v58, 0);
        v60 = (v59 + 39) & 0xFFFFFFFFFFFFFFE0uLL;
        *(_QWORD *)(v60 - 8) = v59;
      }
    }
    else
    {
      v60 = 0;
    }
    *(_QWORD *)(v60 + v53) = v310;
    *(_QWORD *)(v60 + v53 + 8) = v312;
    v61 = (_QWORD *)v313[1];
    v62 = (_QWORD *)v313[2];
    if ( v49 == v62 )
    {
      if ( v61 != v49 )
      {
        v68 = 0;
        do
        {
          *(_OWORD *)(v60 + v68 * 8) = 0;
          *(_QWORD *)(v60 + v68 * 8) = v61[v68];
          *(_QWORD *)(v60 + v68 * 8 + 8) = v61[v68 + 1];
          *(_OWORD *)&v61[v68] = 0;
          v69 = (__int64)&v61[v68 + 2];
          v68 += 2;
        }
        while ( (_QWORD *)v69 != v49 );
      }
    }
    else
    {
      if ( v61 != v49 )
      {
        v63 = 0;
        do
        {
          *(_OWORD *)(v60 + v63 * 8) = 0;
          *(_QWORD *)(v60 + v63 * 8) = v61[v63];
          *(_QWORD *)(v60 + v63 * 8 + 8) = v61[v63 + 1];
          *(_OWORD *)&v61[v63] = 0;
          v64 = (__int64)&v61[v63 + 2];
          v63 += 2;
        }
        while ( (_QWORD *)v64 != v49 );
        v62 = (_QWORD *)v313[2];
      }
      if ( v49 != v62 )
      {
        v65 = v53 + v60 + 16;
        v66 = 0;
        do
        {
          *(_OWORD *)(v65 + v66 * 8) = 0;
          *(_QWORD *)(v65 + v66 * 8) = v49[v66];
          *(_QWORD *)(v65 + v66 * 8 + 8) = v49[v66 + 1];
          *(_OWORD *)&v49[v66] = 0;
          v67 = (__int64)&v49[v66 + 2];
          v66 += 2;
        }
        while ( (_QWORD *)v67 != v62 );
      }
    }
    sub_14013C6D0(v51, v60, (v53 >> 4) + 1, v57);
    v48 = v313;
    v50 = v313[2];
  }
  else
  {
    *v49 = v310;
    v49[1] = v43;
    v50 = v48[2] + 16LL;
    v48[2] = v50;
  }
  *((_BYTE *)v48 + 96) = 1;
  v48[11] = *(_QWORD *)(v50 - 16);
  sub_14530BDD0(v48, &v133);
  sub_145313430(v313, v131);
  sub_145313430(v313, v129);
  sub_145313430(v313, v127);
  v71 = (_QWORD *)v313[11];
  if ( v71 + 1 != &v123 )
  {
    v72 = v125;
    if ( *((_QWORD *)&v125 + 1) < 0x10u )
      v73 = &v124;
    else
      v73 = (unsigned __int128 *)v124;
    v74 = v71 + 2;
    v75 = v71[5];
    if ( (unsigned __int64)v125 <= v75 )
    {
      if ( v75 >= 0x10 )
        v74 = (_QWORD *)v71[2];
      v71[4] = v125;
      ((void (__fastcall *)(_QWORD *, unsigned __int128 *, __int64))unk_14E5A62C0)(v74, v73, v72);
      *((_BYTE *)v74 + v72) = 0;
    }
    else
    {
      sub_14009DAF0(v71 + 2, v125, v70, v73);
    }
  }
  v71[1] = v123;
  v71[6] = 0;
  v313[11] = 0;
  if ( *((_QWORD *)&v125 + 1) >= 0x10u )
  {
    v76 = v124;
    if ( (unsigned __int64)(*((_QWORD *)&v125 + 1) + 1LL) >= 0x1000 )
    {
      if ( (unsigned __int64)(v124 - 8 - *(_QWORD *)(v124 - 8)) >= 0x20 )
        goto LABEL_158;
      v76 = *(_QWORD *)(v124 - 8);
    }
    ((void (__fastcall *)(__int64))unk_14E021350)(v76);
  }
  *(_QWORD *)&v125 = 0;
  *((_QWORD *)&v125 + 1) = 15;
  LOBYTE(v124) = 0;
  sub_14B751F60(&v119);
  if ( *((_QWORD *)&v270 + 1) >= 0x10u )
  {
    v77 = v269;
    if ( (unsigned __int64)(*((_QWORD *)&v270 + 1) + 1LL) >= 0x1000 )
    {
      if ( (unsigned __int64)(v269 - 8 - *(_QWORD *)(v269 - 8)) >= 0x20 )
        goto LABEL_158;
      v77 = *(_QWORD *)(v269 - 8);
    }
    ((void (__fastcall *)(__int64))unk_14E021350)(v77);
  }
  if ( v258 >= 0x10 )
  {
    v78 = v255;
    if ( v258 + 1 >= 0x1000 )
    {
      if ( v255 - 8 - *(_QWORD *)(v255 - 8) >= 0x20 )
        goto LABEL_158;
      v78 = *(_QWORD *)(v255 - 8);
    }
    ((void (__fastcall *)(unsigned __int64))unk_14E021350)(v78);
  }
  if ( *((_QWORD *)&v267 + 1) >= 0x10u )
  {
    v79 = v265;
    if ( (unsigned __int64)(*((_QWORD *)&v267 + 1) + 1LL) >= 0x1000 )
    {
      if ( v265 - 8 - *(_QWORD *)(v265 - 8) >= 0x20 )
        goto LABEL_158;
      v79 = *(_QWORD *)(v265 - 8);
    }
    ((void (__fastcall *)(unsigned __int64))unk_14E021350)(v79);
  }
  sub_14B751F60(&v112);
  if ( v290.m256i_i64[3] >= 0x10uLL )
  {
    v80 = v290.m256i_i64[0];
    if ( (unsigned __int64)(v290.m256i_i64[3] + 1) >= 0x1000 )
    {
      if ( (unsigned __int64)(v290.m256i_i64[0] - 8 - *(_QWORD *)(v290.m256i_i64[0] - 8)) >= 0x20 )
        goto LABEL_158;
      v80 = *(_QWORD *)(v290.m256i_i64[0] - 8);
    }
    ((void (__fastcall *)(__int64))unk_14E021350)(v80);
  }
  if ( v262 >= 0x10 )
  {
    v81 = v261[0];
    if ( v262 + 1 >= 0x1000 )
    {
      if ( (unsigned __int64)(v261[0] - 8LL - *(_QWORD *)(v261[0] - 8LL)) >= 0x20 )
        goto LABEL_158;
      v81 = *(_QWORD *)(v261[0] - 8LL);
    }
    ((void (__fastcall *)(__int64))unk_14E021350)(v81);
  }
  if ( v295 >= 0x10 )
  {
    v82 = v294[0];
    if ( v295 + 1 >= 0x1000 )
    {
      if ( (unsigned __int64)(v294[0] - 8LL - *(_QWORD *)(v294[0] - 8LL)) >= 0x20 )
        goto LABEL_158;
      v82 = *(_QWORD *)(v294[0] - 8LL);
    }
    ((void (__fastcall *)(__int64))unk_14E021350)(v82);
  }
  if ( v157 )
    ((void (__fastcall *)(_QWORD *, __int64))unk_14E021350)(v157, 136);
  if ( (unsigned __int64)(v154 + 1LL) >= 4 )
    (**(void (__fastcall ***)(_BYTE *, _QWORD))v153)(v153, 0);
  v83 = v150;
  if ( (_QWORD)v150 )
  {
    v84 = *((_QWORD *)&v150 + 1);
    if ( (_QWORD)v150 != *((_QWORD *)&v150 + 1) )
    {
      do
      {
        if ( (unsigned __int64)(*(char *)(v83 + 24) + 1LL) >= 3 )
          (**(void (__fastcall ***)(__int64, _QWORD))v83)(v83, 0);
        v83 += 40;
      }
      while ( v83 != v84 );
      v83 = v150;
    }
    if ( (unsigned __int64)(v151 - v83) < 0x1000 )
      goto LABEL_145;
    if ( (unsigned __int64)(v83 - 8 - *(_QWORD *)(v83 - 8)) < 0x20 )
    {
      v83 = *(_QWORD *)(v83 - 8);
LABEL_145:
      ((void (__fastcall *)(__int64))unk_14E021350)(v83);
      goto LABEL_146;
    }
LABEL_158:
    invoke_watson(0, 0, 0, 0, 0);
  }
LABEL_146:
  v85 = v284;
  if ( v284 )
  {
    if ( !_InterlockedDecrement(v284 + 2) )
    {
      (**(void (__fastcall ***)(volatile signed __int32 *))v85)(v85);
      if ( !_InterlockedDecrement(v85 + 3) )
        (*(void (__fastcall **)(volatile signed __int32 *))(*(_QWORD *)v85 + 8LL))(v85);
    }
  }
  sub_14535CF30(v139);
  sub_140905570(v149);
  for ( i = v141; v141 != (__int64 *)&v141; i = v141 )
  {
    v88 = *i;
    v86 = (__int64 *)i[1];
    *v86 = *i;
    *(_QWORD *)(v88 + 8) = v86;
    *i = (__int64)i;
    i[1] = (__int64)i;
  }
  if ( i != (__int64 *)&v141 )
  {
    v89 = v142;
    *v142 = i;
    i[1] = (__int64)v89;
    v141 = (__int64 *)&v141;
    v142 = &v141;
  }
  if ( v140 )
  {
    LOBYTE(v86) = v140 != v139;
    (*(void (__fastcall **)(_QWORD *, __int64 *))(*v140 + 32LL))(v140, v86);
  }
}
