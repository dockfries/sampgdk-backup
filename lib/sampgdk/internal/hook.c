/* Copyright (C) 2012-2016 Zeex
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sampgdk/bool.h>
#include <sampgdk/platform.h>

#if SAMPGDK_WINDOWS
  #include <windows.h>
#else
  #include <stdint.h>
  #include <unistd.h>
  #include <sys/mman.h>

  void __builtin___clear_cache(void *, void *);  /* GCC/Clang builtin */
#endif

#include "log.h"
#include "hook.h"
#include "types.h"

#ifdef SAMPGDK_64BIT
#  define _SAMPGDK_HOOK_JMP_SIZE 14
#else
#  define _SAMPGDK_HOOK_JMP_SIZE 5
#endif
#define _SAMPGDK_HOOK_MAX_INSN_LEN 15
/* Address-size displacement used by MOV moffs (A0-A3): 64-bit default 8,
 * 4 under the 0x67 address-size override; 4 on 32-bit (2 under 0x67). */
#ifdef SAMPGDK_64BIT
#  define _SAMPGDK_HOOK_ADDR_SIZE(addr32) ((addr32) ? 4 : 8)
#else
#  define _SAMPGDK_HOOK_ADDR_SIZE(addr32) ((addr32) ? 2 : 4)
#endif
/* Trampoline must hold: copied instructions (up to JMP_SIZE-1+MAX_INSN_LEN)
 * + back-jump (JMP_SIZE).  The original formula (JMP_SIZE+MAX_INSN_LEN-1)
 * never accounted for the back-jump. */
#define _SAMPGDK_HOOK_TRAMPOLINE_SIZE \
  (_SAMPGDK_HOOK_JMP_SIZE * 2 + _SAMPGDK_HOOK_MAX_INSN_LEN - 1)

#pragma pack(push, 1)

#ifdef SAMPGDK_64BIT
/* FF 25 00 00 00 00 = jmp [rip+0] (6B) + 8B absolute address */
struct _sampgdk_hook_jmp {
  uint8_t  opcode;    /* 0xFF */
  uint8_t  modrm;     /* 0x25 */
  int32_t  disp;      /* 0 */
  uintptr_t target;   /* 8-byte absolute address */
};
#else
struct _sampgdk_hook_jmp {
  uint8_t opcode;     /* 0xE9 */
  int32_t offset;
};
#endif

#pragma pack(pop)

struct _sampgdk_hook {
  uint8_t trampoline[_SAMPGDK_HOOK_TRAMPOLINE_SIZE];
};

#if SAMPGDK_WINDOWS

static void *_sampgdk_hook_unprotect(void *address, size_t size) {
  DWORD old;

  if (VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) == 0) {
    return NULL;
  }

  return address;
}

#else /* SAMPGDK_WINDOWS */

static void *_sampgdk_hook_unprotect(void *address, size_t size) {
  long pagesize;

  pagesize = sysconf(_SC_PAGESIZE);
  address = (void *)((uintptr_t)address & ~((uintptr_t)(pagesize - 1)));

  if (mprotect(address, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return NULL;
  }

  return address;
}

#endif /* !SAMPGDK_WINDOWS */

static size_t _sampgdk_hook_disasm(uint8_t *code, int *reloc) {
  /* Length-only disassembler for x86/x64.
   * Derives instruction length from encoding rules, not from an opcode table.
   * Based on the standard approach used by Detours/mhook etc.
   */
  int len = 0;

  int opsize16 = 0;  /* 0x66 operand-size override */
  int addr32 = 0;    /* 0x67 address-size override */

  /* Consume legacy prefixes */
  while (1) {
    uint8_t b = code[len];
    /* LOCK/REPNE/REPE */  if (b == 0xF0 || b == 0xF2 || b == 0xF3) { len++; continue; }
    /* CS/SS/DS/ES/FS/GS */ if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { len++; continue; }
    /* Operand-size override */ if (b == 0x66) { len++; opsize16 = 1; continue; }
    /* Address-size override */ if (b == 0x67) { len++; addr32 = 1; continue; }
    break;
  }

#ifdef SAMPGDK_64BIT
  int rex = 0;
  /* REX prefix (0x40-0x4F) */
  if ((code[len] & 0xF0) == 0x40) {
    rex = code[len] & 0x0F;  /* save full REX for W bit */
    len++;
  }
#else
  int rex = 0;
#endif

  /* VEX/EVEX/XOP prefixes (64-bit only). Skip the whole prefix so the
   * embedded opcode (right after the last prefix byte) is decoded. */
  int vex_map = -1;   /* VEX.pp[1:0] selects the opcode map:
                       * 0=0F, 1=0F38, 2=0F3A, 3=unused */
#ifdef SAMPGDK_64BIT
  if (code[len] == 0x62) {
    /* EVEX: 62 P0 P1 P2 <opcode>. The opcode map is in P0[2:0]
     * (P0 is the byte after the 0x62): 1=0F, 2=0F38, 3=0F3A. */
    vex_map = code[len + 1] & 7;
    len += 4;
  } else if (code[len] == 0xC4) {
    /* VEX 3-byte: C4 P1 P2 <opcode>. Map in P1[2:0]: 1=0F,2=0F38,3=0F3A. */
    vex_map = code[len + 1] & 7;
    len += 3;
  } else if (code[len] == 0xC5) {
    /* VEX 2-byte: C5 P1 <opcode> (map is always 0F) */
    vex_map = 1;  /* 0F map */
    len += 2;
  }
#endif

  /* Check for 2-byte opcode (0x0F prefix) */
  int two_byte = 0;
  int map = 0;   /* 0=1-byte, 1=0F, 2=0F38, 3=0F3A */
  if (code[len] == 0x0F) {
    two_byte = 1;
    len++;
    if (code[len] == 0x38 || code[len] == 0x3A) {
      map = (code[len] == 0x38) ? 2 : 3;
      len++;
    } else {
      map = 1;
    }
  }

  /* Read the primary opcode byte */
  int opcode = code[len++];

  /* Determine if this instruction has ModRM.
   * On x86/x64, most opcodes have ModRM. The exceptions are:
   * - Opcodes 0x00-0x03: ADD/OR/ADC/SBB with ModRM (actually all have ModRM up to 0x3F)
   * - Actually, most opcodes have ModRM. The few that don't:
   *   0x40-0x4F: INC/DEC (x86) / REX prefix (x64 - already handled)
   *   0x50-0x5F: PUSH/POP r32 (PLUS_R style, no ModRM)
   *   0x60-0x6F: various (some have ModRM, some don't)
   *   0x70-0x7F: Jcc rel8 (no ModRM)
   *   0x90-0x97: XCHG/NOP (no ModRM)
   *   0x98-0x9F: various flags/convert (no ModRM)
   *   0xA0-0xA7: MOV moffs (no ModRM, but AL/EAX specific)
   *   0xA8-0xAF: TEST/STOS/LODS/SCAS (no ModRM)
   *   0xB0-0xBF: MOV r8/r32, imm (PLUS_R style, no ModRM)
   *   0xC0-0xC1: Shift/rotate r/m8/32, imm8 (ModRM)
   *   0xC2-0xC3: RET (no ModRM)
   *   0xC4-0xC5: LES/LDS / VEX prefix (no ModRM)
   *   0xC6-0xC7: MOV r/m8/32, imm (ModRM)
   *   0xC8-0xC9: ENTER/LEAVE (no ModRM)
   *   0xCA-0xCB: RET far (no ModRM)
   *   0xCC-0xCE: INT/INTO/IRET (no ModRM)
   *   0xD0-0xD3: Shift/rotate r/m8/32 (ModRM)
   *   0xD4-0xD5: AAM/AAD (no ModRM)
   *   0xD6: SETALC (undocumented, no ModRM)
   *   0xD7: XLAT (no ModRM)
   *   0xE0-0xE3: LOOP/JCXZ (no ModRM)
   *   0xE4-0xE7: IN/OUT (no ModRM)
   *   0xE8-0xE9: CALL/JMP rel (no ModRM)
   *   0xEA-0xEB: JMP far / JMP rel8 (no ModRM)
   *   0xEC-0xEF: IN/OUT (no ModRM)
   *   0xF0-0xF3: LOCK/REP prefixes (already consumed)
   *   0xF4: HLT (no ModRM)
   *   0xF5: CMC (no ModRM)
   *   0xF6-0xF7: Group3 (ModRM)
   *   0xF8-0xFD: CLC/STC/CLI/STI/CLD/STD (no ModRM)
   *   0xFE-0xFF: Group4/5 (ModRM)
   */

  int has_modrm = 1;  /* default: most instructions have ModRM */
  int imm_size = 0;
  int moffs_size = 0;    /* MOV moffs address-size displacement */
  int reloc_offset = 0;
  int riprel = 0;        /* ModRM mod=00 rm=101 on x64 (RIP-relative) */
  int group3 = 0;        /* F6/F7 Group3 opcode, imm decided by ModRM.reg */

  /* Determine ModRM and immediate based on opcode */
  /* Single-byte opcodes (not 0x0F prefix) */
  if (!two_byte) {
    if ((opcode >= 0x00 && opcode <= 0x03) ||  /* ADD/OR/ADC/SBB r/m8,r8 */
        (opcode >= 0x08 && opcode <= 0x0B) ||  /* OR/ADC/SBB/AND r/m8,r8 */
        (opcode >= 0x10 && opcode <= 0x13) ||  /* ADC/SBB/AND/SUB r/m8,r8 */
        (opcode >= 0x18 && opcode <= 0x1B) ||  /* SBB/AND/SUB/XOR r/m8,r8 */
        (opcode >= 0x20 && opcode <= 0x23) ||  /* AND/SUB/XOR/CMP r/m8,r8 */
        (opcode >= 0x28 && opcode <= 0x2B) ||  /* SUB/XOR/CMP/ADD r/m8,r8 */
        (opcode >= 0x30 && opcode <= 0x33) ||  /* XOR/CMP/ADD/OR r/m8,r8 */
        (opcode >= 0x38 && opcode <= 0x3B)) {  /* CMP/ADD/OR/ADC r/m8,r8 */
      has_modrm = 1; /* register/memory forms */
    }
    else if ((opcode == 0x04 || opcode == 0x05) ||  /* ADD AL/EAX, imm */
             (opcode == 0x0C || opcode == 0x0D) ||  /* OR  AL/EAX, imm */
             (opcode == 0x14 || opcode == 0x15) ||  /* ADC AL/EAX, imm */
             (opcode == 0x1C || opcode == 0x1D) ||  /* SBB AL/EAX, imm */
             (opcode == 0x24 || opcode == 0x25) ||  /* AND AL/EAX, imm */
             (opcode == 0x2C || opcode == 0x2D) ||  /* SUB AL/EAX, imm */
             (opcode == 0x34 || opcode == 0x35) ||  /* XOR AL/EAX, imm */
             (opcode == 0x3C || opcode == 0x3D)) {  /* CMP AL/EAX, imm */
      has_modrm = 0;
      imm_size = (opcode & 1) ? 4 : 1; /* EAX forms imm32, AL forms imm8 */
    }
    else if (opcode == 0x06 || opcode == 0x07 ||  /* PUSH/POP ES */
             opcode == 0x0E ||                   /* PUSH CS */
             opcode == 0x16 || opcode == 0x17 ||  /* PUSH/POP SS */
             opcode == 0x1E || opcode == 0x1F ||  /* PUSH/POP DS */
             opcode == 0x27 || opcode == 0x2F ||  /* DAA/DAS */
             opcode == 0x37 || opcode == 0x3F) {  /* AAA/AAS */
      has_modrm = 0; /* one-byte, no ModRM */
    }
    else if (opcode >= 0x40 && opcode <= 0x4F) {
      has_modrm = 0; /* INC/DEC r32 (x86) - on x64 these are REX prefixes, already handled */
    }
    else if (opcode >= 0x50 && opcode <= 0x5F) {
      has_modrm = 0; /* PUSH/POP r32 */
    }
    else if (opcode >= 0x60 && opcode <= 0x6F) {
      if (opcode == 0x60 || opcode == 0x61) {
        has_modrm = 0; /* PUSHA/POPA (x86) */
      } else if (opcode == 0x62) {
        has_modrm = 1; /* BOUND (x86); 64-bit EVEX prefix already consumed */
      } else if (opcode == 0x63) {
        has_modrm = 1; /* MOVSXD (x64) / ARPL (x86) */
      } else if (opcode == 0x68) {
        has_modrm = 0; imm_size = 4; /* PUSH imm32 */
      } else if (opcode == 0x69) {
        has_modrm = 1; imm_size = 4; /* IMUL r32,r/m32,imm32 */
      } else if (opcode == 0x6A) {
        has_modrm = 0; imm_size = 1; /* PUSH imm8 */
      } else if (opcode == 0x6B) {
        has_modrm = 1; imm_size = 1; /* IMUL r32,r/m32,imm8 */
      } else {
        has_modrm = 0; /* INS/OUTS (0x6C-0x6F); 0x64-0x67 prefixes consumed */
      }
    }
    else if (opcode >= 0x70 && opcode <= 0x7F) {
      has_modrm = 0;
      imm_size = 1; /* Jcc rel8 */
    }
    else if (opcode >= 0x80 && opcode <= 0x83) {
      /* Group1 r/m, imm (0x82 is an alias of 0x80: CMP r/m8, imm8) */
      has_modrm = 1;
      imm_size = (opcode == 0x80 || opcode == 0x82 || opcode == 0x83) ? 1 : 4;
    }
    else if (opcode == 0x84 || opcode == 0x85) {
      has_modrm = 1; /* TEST r/m8,r8 / TEST r/m32,r32 */
    }
    else if (opcode == 0x86 || opcode == 0x87) {
      has_modrm = 1; /* XCHG r8,r/m8 / XCHG r32,r/m32 */
    }
    else if (opcode >= 0x88 && opcode <= 0x8F) {
      has_modrm = 1; /* MOV/LEA/POP r/m (0x8F: POP r/m, reg field is 0) */
    }
    else if (opcode >= 0x90 && opcode <= 0x97) {
      has_modrm = 0; /* NOP/XCHG */
    }
    else if (opcode >= 0x98 && opcode <= 0x9F) {
      has_modrm = 0; /* CBW/CWD/CDQ/CWDE/CDQE/WAIT/PUSHF/POPF/SAHF/LAHF */
      if (opcode == 0x9A) imm_size = 6; /* CALL far ptr16:32 */
    }
    else if (opcode >= 0xA0 && opcode <= 0xA3) {
      has_modrm = 0; /* MOV AL/EAX/RAX, moffs / MOV moffs, AL/EAX/RAX */
      moffs_size = _SAMPGDK_HOOK_ADDR_SIZE(addr32);
    }
    else if (opcode >= 0xA4 && opcode <= 0xA7) {
      has_modrm = 0; /* MOVS/CMPS */
    }
    else if (opcode == 0xA8 || opcode == 0xA9) {
      has_modrm = 0; /* TEST AL/EAX, imm */
      imm_size = (opcode == 0xA9) ? 4 : 1;
    }
    else if (opcode >= 0xAA && opcode <= 0xAF) {
      has_modrm = 0; /* STOS/LODS/SCAS */
    }
    else if (opcode >= 0xB0 && opcode <= 0xB7) {
      has_modrm = 0; imm_size = 1; /* MOV r8, imm8 */
    }
    else if (opcode >= 0xB8 && opcode <= 0xBF) {
      has_modrm = 0; /* MOV r32/r64, imm32/imm64 */
      if (rex & 0x08) imm_size = 8;      /* REX.W: MOV r64, imm64 */
      else imm_size = 4;
    }
    else if (opcode >= 0xC0 && opcode <= 0xC1) {
      has_modrm = 1; imm_size = 1; /* Shift/rotate r/m8/32, imm8 */
    }
    else if (opcode >= 0xC2 && opcode <= 0xC3) {
      has_modrm = 0;
      if (opcode == 0xC2) imm_size = 2; /* RET imm16 */
    }
    else if (opcode == 0xC4 || opcode == 0xC5) {
      has_modrm = 1; /* LES/LDS (x86); 64-bit VEX prefixes already consumed */
    }
    else if (opcode >= 0xC6 && opcode <= 0xC7) {
      has_modrm = 1; /* MOV r/m8, imm8 / MOV r/m32, imm32 */
      imm_size = (opcode == 0xC6) ? 1 : 4;
    }
    else if (opcode >= 0xC8 && opcode <= 0xCF) {
      has_modrm = 0;
      if (opcode == 0xC8) imm_size = 3; /* ENTER imm16,imm8 */
      else if (opcode == 0xCA) imm_size = 2; /* RET far imm16 */
      else if (opcode == 0xCD) imm_size = 1; /* INT imm8 */
    }
    else if (opcode >= 0xD0 && opcode <= 0xD3) {
      has_modrm = 1; /* Shift/rotate r/m, 1/CL/imm8 */
    }
    else if (opcode >= 0xD4 && opcode <= 0xD7) {
      has_modrm = 0; /* AAM/AAD/SETALC/XLAT */
      if (opcode == 0xD4 || opcode == 0xD5) imm_size = 1; /* AAM/AAD imm8 */
    }
    else if (opcode >= 0xD8 && opcode <= 0xDF) {
      has_modrm = 1; /* FPU instructions */
    }
    else if (opcode >= 0xE0 && opcode <= 0xE3) {
      has_modrm = 0; imm_size = 1; /* LOOP/JCXZ rel8 */
    }
    else if (opcode == 0xE4 || opcode == 0xE5) {
      has_modrm = 0; imm_size = 1; /* IN AL/EAX, imm8 */
    }
    else if (opcode == 0xE6 || opcode == 0xE7) {
      has_modrm = 0; imm_size = 1; /* OUT imm8, AL/EAX */
    }
    else if (opcode == 0xE8) {
      has_modrm = 0; imm_size = 4; reloc_offset = len; /* CALL rel32 */
    }
    else if (opcode == 0xE9) {
      has_modrm = 0; imm_size = 4; reloc_offset = len; /* JMP rel32 */
    }
    else if (opcode == 0xEA || opcode == 0xEB) {
      has_modrm = 0;
      imm_size = (opcode == 0xEB) ? 1 : 6; /* JMP rel8 / JMP far ptr16:32 */
    }
    else if (opcode >= 0xEC && opcode <= 0xEF) {
      has_modrm = 0; /* IN/OUT AL/EAX, DX */
    }
    else if (opcode == 0xF1) {
      has_modrm = 0; /* INT1/ICEBP */
    }
    else if (opcode == 0xF4) {
      has_modrm = 0; /* HLT */
    }
    else if (opcode == 0xF5) {
      has_modrm = 0; /* CMC */
    }
    else if (opcode >= 0xF6 && opcode <= 0xF7) {
      has_modrm = 1;
      group3 = opcode; /* Group3: imm present only for /0 /1 (TEST) */
    }
    else if (opcode >= 0xF8 && opcode <= 0xFD) {
      has_modrm = 0; /* CLC/STC/CLI/STI/CLD/STD */
    }
    else if (opcode >= 0xFE && opcode <= 0xFF) {
      has_modrm = 1; /* Group4/5 (INC/DEC/CALL/JMP r/m) */
    }
  } else {
    /* 0x0F-prefixed (two-byte) opcodes */
    if (opcode == 0x05 || opcode == 0x06 || opcode == 0x07 ||  /* SYSCALL/CLTS/SYSRET */
        opcode == 0x08 || opcode == 0x09 ||  /* INVD/WBINVD */
        opcode == 0x0B || opcode == 0x0E ||  /* UD2/FEMMS */
        opcode == 0x30 || opcode == 0x31 ||  /* WRMSR/RDTSC */
        opcode == 0x32 || opcode == 0x33 ||  /* RDMSR/RDPMC */
        opcode == 0x34 || opcode == 0x35 ||  /* SYSENTER/SYSEXIT */
        opcode == 0x37 || opcode == 0x77 ||  /* GETSEC/EMMS */
        opcode == 0xA0 || opcode == 0xA1 ||  /* PUSH/POP FS */
        opcode == 0xA2 || opcode == 0xA8 ||  /* CPUID/PUSH GS */
        opcode == 0xA9 || opcode == 0xAA ||  /* POP GS/RSM */
        opcode == 0xB9 ||                     /* UD1 */
        (opcode >= 0xC8 && opcode <= 0xCF)) { /* BSWAP */
      has_modrm = 0;
    } else {
      has_modrm = 1;
      if (opcode >= 0x80 && opcode <= 0x8F) {
        has_modrm = 0; /* Jcc rel32: no ModRM */
        imm_size = 4; /* Jcc rel32 */
        reloc_offset = len;
      }
      if (opcode == 0x70 ||
          (opcode >= 0x71 && opcode <= 0x73) ||
          opcode == 0xA4 || opcode == 0xAC ||
          opcode == 0xBA ||
          opcode == 0xC2 || opcode == 0xC4 ||
          opcode == 0xC5 || opcode == 0xC6) {
        imm_size = 1; /* PSHUFD, PSRLW/PSLLW/PSRAW, SHLD/SHRD, BT, CMPPS/PINSRW/PEXTRW/SHUFPS */
      }
      if (map == 3 && !(opcode >= 0x4A && opcode <= 0x4C)) {
        imm_size = 1; /* 0F 3A map: imm8 except BLENDVPS/VPBLENDVB/VPBLENDVPS */
      }
    }
  }

#ifdef SAMPGDK_64BIT
  /* VEX/EVEX overrides: everything except VZEROUPPER/VZEROALL has ModRM,
   * nothing has a relative branch, and the imm rules follow the map. */
  if (vex_map >= 0) {
    if (vex_map == 1 && opcode == 0x77) {
      has_modrm = 0; /* VZEROUPPER/VZEROALL */
    } else {
      has_modrm = 1;
    }
    reloc_offset = 0;
    imm_size = 0;
    moffs_size = 0;
    group3 = 0;
    if (vex_map == 3 && !(opcode >= 0x4A && opcode <= 0x4C)) {
      imm_size = 1; /* EVEX.0F3A imm8 (BLENDV* variants take XMM0) */
    } else if (vex_map == 1 && (opcode == 0x70 || opcode == 0x71 ||
                                opcode == 0x72 || opcode == 0x73 ||
                                opcode == 0xC2 || opcode == 0xC4 ||
                                opcode == 0xC5 || opcode == 0xC6)) {
      imm_size = 1; /* VEX.0F imm8 forms (incl. vpsrlq/vpsllq shifts) */
    }
  }
#endif

  /* Parse ModRM if present */
  if (has_modrm) {
    int modrm = code[len++];
    int mod = modrm >> 6;
    int rm = modrm & 7;
    int sib = (mod != 3 && rm == 4);

    if (sib) {
      int base = code[len] & 7; /* SIB byte */
      len++;
      if (mod == 0 && base == 5) {
        len += 4; /* SIB base=101 with mod=00: disp32 (absolute address) */
      }
    }

    if (mod == 1) {
      len += 1; /* disp8 */
    } else if (mod == 2) {
      len += 4; /* disp32 */
    } else if (mod == 0 && rm == 5 && !sib) {
      /* RIP-relative on x64, or absolute disp32 on x86 */
      len += 4; /* disp32 */
#ifdef SAMPGDK_64BIT
      if (!addr32) {
        /* RIP-relative: the trampoline runs at a different address, so the
         * disp32 must be adjusted by -(trampoline - src) just like rel32
         * CALL/JMP. mod=00 rm=101 with the 0x67 address-size override is
         * absolute (EAX-relative addressing is not RIP-relative).
         * reloc_offset points at the disp32 field (len now includes it). */
        riprel = 1;
        reloc_offset = len - 4;
      }
#endif
    }

    if (group3) {
      int reg = (modrm >> 3) & 7;
      if (reg == 0 || reg == 1) { /* TEST r/m, imm */
        imm_size = (group3 == 0xF7) ? 4 : 1;
      }
    }
  }

  /* Add immediate */
  if (moffs_size > 0) {
    len += moffs_size;
  }
  if (imm_size == 1) {
    len += 1;
  } else if (imm_size == 2) {
    len += 2;
  } else if (imm_size == 3) {
    len += 3; /* ENTER imm16,imm8 */
  } else if (imm_size == 4) {
    if (opsize16 && !riprel && !reloc_offset) {
      len += 2; /* 0x66 operand-size override shrinks imm32 to imm16 */
    } else if (rex & 0x08 && !two_byte && opcode >= 0xB8 && opcode <= 0xBF) {
      len += 8; /* MOV r64, imm64 */
    } else {
      len += 4;
    }
  } else if (imm_size == 6) {
    len += 6; /* far JMP ptr16:32 */
  } else if (imm_size == 8) {
    len += 8; /* MOV r64, imm64 (REX.W) */
  }

  /* Set relocation offset for relative CALL/JMP */
  if (reloc != NULL && reloc_offset > 0) {
    *reloc = reloc_offset;
  }

  return len;
}

static void _sampgdk_hook_write_jmp(void *src, void *dst, int32_t offset) {
  struct _sampgdk_hook_jmp jmp;

#ifdef SAMPGDK_64BIT
  jmp.opcode = 0xFF;
  jmp.modrm  = 0x25;
  jmp.disp   = 0;
  jmp.target = (uintptr_t)dst;
#else
  jmp.opcode = 0xE9;
  jmp.offset = (int32_t)((uint8_t *)dst - ((uint8_t *)src + sizeof(jmp)));
#endif

  memcpy((uint8_t *)src + offset, &jmp, sizeof(jmp));
}

sampgdk_hook_t sampgdk_hook_new(void *src, void *dst) {
  struct _sampgdk_hook *hook;
  size_t orig_size = 0;
  size_t insn_len;

  if ((hook = (sampgdk_hook_t)malloc(sizeof(*hook))) == NULL) {
    return NULL;
  }

  _sampgdk_hook_unprotect(src, _SAMPGDK_HOOK_JMP_SIZE);
  _sampgdk_hook_unprotect(hook->trampoline, _SAMPGDK_HOOK_TRAMPOLINE_SIZE);

  /* We can't just jump to src + 5 as we could end up in the middle of
   * some instruction. So we need to determine the instruction length.
   */
  while (orig_size < _SAMPGDK_HOOK_JMP_SIZE) {
    uint8_t *insn = (uint8_t *)src + orig_size;
    int reloc = 0;

    if ((insn_len = _sampgdk_hook_disasm(insn, &reloc)) == 0) {
      sampgdk_log_error("Unsupported instr at offset %zu (byte: 0x%02X), func=%p, JMP_SIZE=%d",
                        orig_size, insn[0], src, _SAMPGDK_HOOK_JMP_SIZE);
      break;
    }

    memcpy(hook->trampoline + orig_size, insn, insn_len);

    /* If the original code contains a relative JMP/CALL or RIP-relative
     * memory operand, relocate it by -(trampoline - src). Note: this only
     * works when the trampoline is within +/-2GB of src; the heap address
     * from malloc usually is not, so hooking a function whose prologue
     * contains such an instruction requires the trampoline to be allocated
     * near src (see the note in sampgdk_hook_new). */
    if (reloc != 0) {
      int32_t *offset = (int32_t *)(hook->trampoline + orig_size + reloc);
      *offset -= (int32_t)((intptr_t)hook->trampoline - (intptr_t)src);
    }

    orig_size += insn_len;
  }

  if (insn_len == 0) {
    free(hook);
    return NULL;
  }

#ifdef SAMPGDK_64BIT
  /* Absolute JMP (FF 25 + 8B addr): dst is used directly as target. */
  _sampgdk_hook_write_jmp(hook->trampoline, (uint8_t *)src + orig_size, (int32_t)orig_size);
#else
  /* Relative JMP (E9 + rel32): formula is dst - (src_param + 5).
   * To jump to src + orig_size, pass dst = src (not src + orig_size). */
  _sampgdk_hook_write_jmp(hook->trampoline, src, (int32_t)orig_size);
#endif
  _sampgdk_hook_write_jmp(src, dst, 0);

#if SAMPGDK_WINDOWS
  FlushInstructionCache(GetCurrentProcess(), src, _SAMPGDK_HOOK_JMP_SIZE);
  FlushInstructionCache(GetCurrentProcess(), hook->trampoline,
                        _SAMPGDK_HOOK_TRAMPOLINE_SIZE);
#else
  __builtin___clear_cache((char *)src, (char *)src + _SAMPGDK_HOOK_JMP_SIZE);
  __builtin___clear_cache((char *)hook->trampoline,
                          (char *)hook->trampoline + _SAMPGDK_HOOK_TRAMPOLINE_SIZE);
#endif

  return hook;
}

void sampgdk_hook_free(sampgdk_hook_t hook) {
  free(hook);
}

void *sampgdk_hook_trampoline(sampgdk_hook_t hook) {
  return hook->trampoline;
}
