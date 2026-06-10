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
#ifdef SAMPGDK_64BIT
  int rex = 0;
#endif

  /* Consume legacy prefixes */
  while (1) {
    uint8_t b = code[len];
    /* LOCK/REPNE/REPE */  if (b == 0xF0 || b == 0xF2 || b == 0xF3) { len++; continue; }
    /* CS/SS/DS/ES/FS/GS */ if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { len++; continue; }
    /* Operand-size override */ if (b == 0x66) { len++; continue; }
    /* Address-size override */ if (b == 0x67) { len++; continue; }
    break;
  }

#ifdef SAMPGDK_64BIT
  /* REX prefix (0x40-0x4F) */
  if ((code[len] & 0xF0) == 0x40) {
    rex = code[len] & 0x0F;  /* save full REX for W bit */
    len++;
  }
#endif

  /* Check for 3-byte opcode (VEX/EVEX-like): 0x62 = EVEX, 0xC4/C5 = VEX */
  /* 0x62: EVEX prefix (4 bytes total). Skip the prefix bytes. */
  if (code[len] == 0x62) {
    /* EVEX: 62 + P0 + P1 + opcode */
    len += 3;  /* +3 for the EVEX prefix bytes after the 62 */
    /* Then the actual opcode follows - we'll fall through to handle it */
    /* But EVEX instructions still have ModRM etc., so continue */
  }
  /* 0xC4: VEX 3-byte, 0xC5: VEX 2-byte */
  if (code[len] == 0xC4) {
    len += 2; /* skip VEX.2 + VEX.3 */
  }
  if (code[len] == 0xC5) {
    len += 1; /* skip VEX.2 */
  }

  /* XOP prefix (0x8F with reg_opcode=0) - rare, but skip it */
  if (code[len] == 0x8F && ((code[len + 1] >> 3) & 7) == 0) {
    len += 2; /* skip XOP.2 + XOP.3 */
  }

  /* Check for 2-byte opcode (0x0F prefix) */
  int two_byte = 0;
  if (code[len] == 0x0F) {
    two_byte = 1;
    len++;
    /* 0x0F 0x38 or 0x0F 0x3A = 3-byte opcode */
    if (code[len] == 0x38 || code[len] == 0x3A) {
      len++;
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
  int reloc_offset = 0;

  /* Determine ModRM and immediate based on opcode */
  /* Single-byte opcodes (not 0x0F prefix) */
  if (!two_byte) {
    if ((opcode >= 0x00 && opcode <= 0x03) ||  /* ADD/OR/ADC/SBB r/m8,r8 etc */
        (opcode >= 0x08 && opcode <= 0x0B) ||  /* OR/ADC/SBB/AND r/m8,r8 */
        (opcode >= 0x10 && opcode <= 0x13) ||  /* ADC/SBB/AND/SUB r/m8,r8 */
        (opcode >= 0x18 && opcode <= 0x1B) ||  /* SBB/AND/SUB/XOR r/m8,r8 */
        (opcode >= 0x20 && opcode <= 0x23) ||  /* AND/SUB/XOR/CMP r/m8,r8 */
        (opcode >= 0x28 && opcode <= 0x2B) ||  /* SUB/XOR/CMP/ADD r/m8,r8 */
        (opcode >= 0x30 && opcode <= 0x33) ||  /* XOR/CMP/ADD/OR r/m8,r8 */
        (opcode >= 0x38 && opcode <= 0x3B) ||  /* CMP/ADD/OR/ADC r/m8,r8 */
        opcode == 0x08 || opcode == 0x09 ||    /* OR r/m8,r8 / OR r/m32,r32 */
        opcode == 0x0A || opcode == 0x0B ||
        opcode == 0x1C || opcode == 0x1D ||    /* SBB AL/EAX, imm */
        opcode == 0x2C || opcode == 0x2D ||    /* SUB AL/EAX, imm */
        opcode == 0x34 || opcode == 0x35 ||    /* XOR AL/EAX, imm */
        opcode == 0x3C || opcode == 0x3D) {    /* CMP AL/EAX, imm */
      has_modrm = 0;
      /* Some of these are AL/EAX imm forms: 0x04/0x05, 0x0C/0x0D, 0x14/0x15, 0x1C/0x1D, 0x24/0x25, 0x2C/0x2D, 0x34/0x35, 0x3C/0x3D */
      if ((opcode & 0xFD) == 0x04 || (opcode & 0xFD) == 0x0C || (opcode & 0xFD) == 0x14 ||
          (opcode & 0xFD) == 0x1C || (opcode & 0xFD) == 0x24 || (opcode & 0xFD) == 0x2C ||
          (opcode & 0xFD) == 0x34 || (opcode & 0xFD) == 0x3C) {
        if (opcode & 1) imm_size = 4; /* imm32 for EAX forms */
        else imm_size = 1;            /* imm8 for AL forms */
      }
    }
    else if ((opcode >= 0x06 && opcode <= 0x07) ||  /* PUSH/POP ES (x86) */
             (opcode >= 0x0E && opcode <= 0x0F) ||  /* PUSH/POP CS (x86) / 0x0F is 2-byte prefix! */
             (opcode >= 0x16 && opcode <= 0x17) ||  /* PUSH/POP SS */
             (opcode >= 0x1E && opcode <= 0x1F)) {  /* PUSH/POP DS */
      has_modrm = 0; /* segment register push/pop, 1 byte */
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
        has_modrm = 0; /* BOUND (x86) / EVEX prefix */
      } else if (opcode == 0x63) {
        has_modrm = 1; /* MOVSXD (x64) / ARPL (x86) */
      } else if (opcode == 0x68 || opcode == 0x6A) {
        has_modrm = 0;
        imm_size = (opcode == 0x68) ? 4 : 1; /* PUSH imm32/imm8 */
      } else if (opcode == 0x69 || opcode == 0x6B) {
        has_modrm = 1;
        imm_size = (opcode == 0x69) ? 4 : 1; /* IMUL r32,r/m32,imm32/imm8 */
      } else {
        has_modrm = 0; /* 0x64/0x65 FS/GS prefixes, 0x66/0x67 overrides - already consumed above! */
      }
    }
    else if (opcode >= 0x70 && opcode <= 0x7F) {
      has_modrm = 0;
      imm_size = 1; /* Jcc rel8 */
    }
    else if (opcode >= 0x80 && opcode <= 0x83) {
      has_modrm = 1; /* Group1 r/m, imm8/32 */
      imm_size = (opcode == 0x80 || opcode == 0x82) ? 1 : (opcode == 0x81 ? 4 : 1);
    }
    else if (opcode == 0x84 || opcode == 0x85) {
      has_modrm = 1; /* TEST r/m8,r8 / TEST r/m32,r32 */
    }
    else if (opcode == 0x86 || opcode == 0x87) {
      has_modrm = 1; /* XCHG r8,r/m8 / XCHG r32,r/m32 */
    }
    else if (opcode >= 0x88 && opcode <= 0x8F) {
      has_modrm = 1; /* MOV/LEA/POP various */
      if (opcode == 0x8D) has_modrm = 1; /* LEA */
      if (opcode == 0x8F) has_modrm = 1; /* POP r/m32 */
    }
    else if (opcode >= 0x90 && opcode <= 0x97) {
      has_modrm = 0; /* NOP/XCHG */
    }
    else if (opcode >= 0x98 && opcode <= 0x9F) {
      has_modrm = 0; /* CBW/CWD/CDQ/CWDE/CDQE/WAIT/PUSHF/POPF/SAHF/LAHF */
    }
    else if (opcode >= 0xA0 && opcode <= 0xA7) {
      has_modrm = 0; /* MOV moffs / MOVS/CMPS */
      if (opcode >= 0xA0 && opcode <= 0xA3) {
        imm_size = (opcode & 1) ? 4 : 1; /* MOV AL/EAX, moffs / MOV moffs, AL/EAX */
      }
    }
    else if (opcode >= 0xA8 && opcode <= 0xAF) {
      has_modrm = 0; /* TEST AL/EAX, imm / STOS/SCAS/LODS */
      if (opcode == 0xA8 || opcode == 0xA9) {
        imm_size = (opcode == 0xA9) ? 4 : 1; /* TEST AL/EAX, imm */
      }
    }
    else if (opcode >= 0xB0 && opcode <= 0xBF) {
      has_modrm = 0; /* MOV r8/r32, imm8/imm32 */
      if (opcode >= 0xB0 && opcode <= 0xB7) imm_size = 1;
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
      has_modrm = 0; /* LES/LDS (x86) / VEX prefix - already handled above */
    }
    else if (opcode >= 0xC6 && opcode <= 0xC7) {
      has_modrm = 1;
      imm_size = (opcode == 0xC6) ? 1 : 4; /* MOV r/m8,imm8 / MOV r/m32,imm32 */
    }
    else if (opcode >= 0xC8 && opcode <= 0xCF) {
      has_modrm = 0;
      if (opcode == 0xC8) imm_size = 4; /* ENTER imm16,imm8 */
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
      imm_size = (opcode == 0xEB) ? 1 : 0; /* JMP far / JMP rel8 */
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
      /* Group3: reg field determines if imm is present */
      /* /0 (TEST) has imm, /2-/7 (NOT/NEG/MUL/IMUL/DIV/IDIV) don't */
      /* But for length estimation, always assume imm is present */
      imm_size = (opcode == 0xF7) ? 4 : 1;
    }
    else if (opcode >= 0xF8 && opcode <= 0xFD) {
      has_modrm = 0; /* CLC/STC/CLI/STI/CLD/STD */
    }
    else if (opcode >= 0xFE && opcode <= 0xFF) {
      has_modrm = 1; /* Group4/5 (INC/DEC/CALL/JMP r/m) */
    }
  } else {
    /* 0x0F-prefixed (two-byte) opcodes */
    /* Most 0x0F-prefixed opcodes have ModRM. Exceptions: */
    if (opcode == 0x31 || opcode == 0x32 ||  /* RDTSC/RDTSCP */
        opcode == 0x33 || opcode == 0x34 ||  /* RDMSR/SYSENTER */
        opcode == 0x35 || opcode == 0x37 ||  /* SYSEXIT/GETSEC */
        opcode == 0x05 || opcode == 0x06 ||  /* SYSCALL/SYS RET */
        opcode == 0x01 || opcode == 0x02 ||  /* SGDT/SIDT/LGDT/LIDT/SMSW */
        opcode == 0x08 || opcode == 0x09 ||  /* INVD/WBINVD */
        opcode == 0x0B || opcode == 0x0D ||  /* UD2/PREFETCH */
        opcode == 0x0E || opcode == 0x0F ||
        opcode == 0x30 || opcode == 0x77 ||  /* WRMSR/EMMS */
        opcode == 0x7F || opcode == 0x90 ||  /* SFENCE/PAUSE */
        opcode == 0x92 || opcode == 0x93 ||
        opcode == 0xA0 || opcode == 0xA1 ||  /* PUSH/POP FS */
        opcode == 0xA8 || opcode == 0xA9 ||  /* PUSH/POP GS */
        opcode == 0x06 || opcode == 0x07) {  /* CLTS/LOADALL */
      has_modrm = 0;
    } else {
      has_modrm = 1;
      /* 0x0F-prefix Jcc with rel32: 0x80-0x8F */
      if (opcode >= 0x80 && opcode <= 0x8F) {
        imm_size = 4; /* Jcc rel32 */
        reloc_offset = len;
      }
      /* 0x0F-prefix Jcc with rel8: 0x70-0x7F */
      if (opcode >= 0x70 && opcode <= 0x7F) {
        imm_size = 1; /* Jcc rel8 */
      }
      /* SETcc r/m8: 0x90-0x9F */
      if (opcode >= 0x90 && opcode <= 0x9F) {
        has_modrm = 1;
      }
    }
  }

  /* Parse ModRM if present */
  if (has_modrm) {
    int modrm = code[len++];
    int mod = modrm >> 6;
    int rm = modrm & 7;

    if (mod != 3 && rm == 4) {
      len++; /* SIB byte */
    }

    if (mod == 1) {
      len += 1; /* disp8 */
    } else if (mod == 2) {
      len += 4; /* disp32 */
    } else if (mod == 0 && rm == 5) {
      /* RIP-relative on x64, or absolute disp32 on x86 */
      len += 4; /* disp32 */
    }
  }

  /* Add immediate */
  if (imm_size == 1) {
    len += 1;
  } else if (imm_size == 2) {
    len += 2;
  } else if (imm_size == 4) {
#ifdef SAMPGDK_64BIT
    /* MOV r64, imm64 (opcode B8-BF with REX.W) */
    if (rex & 0x08 && !two_byte && opcode >= 0xB8 && opcode <= 0xBF) {
      len += 8;
    } else
#endif
    len += 4;
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

    /* If the original code contains a relative JMP/CALL relocate its
     * destination address.
     */
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

  return hook;
}

void sampgdk_hook_free(sampgdk_hook_t hook) {
  free(hook);
}

void *sampgdk_hook_trampoline(sampgdk_hook_t hook) {
  return hook->trampoline;
}
