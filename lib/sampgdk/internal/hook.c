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

#include <Zydis/Zydis.h>

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
  /* Instruction-length + relocation-offset disassembler built on Zydis.
   * Returns the instruction length in bytes and, via *reloc, the byte offset
   * (within the instruction) of the 32-bit relative field that must be
   * re-based when the instruction is copied into the trampoline: the rel32
   * of a relative CALL/JMP/Jcc, or the disp32 of a RIP-relative operand.
   * rel8/rel16 branches and other short forms carry no such field and are
   * copied verbatim.  Returns 0 when the instruction cannot be decoded; the
   * caller then refuses to hook the function. */
  ZydisDecoder decoder;
  ZydisDecodedInstruction insn;
  ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
  int reloc_offset = 0;
  int i;

  if (reloc != NULL) {
    *reloc = 0;
  }

#ifdef SAMPGDK_64BIT
  /* The trampoline's own patch form: "FF 25 00 00 00 00 <8-byte target>" is
   * an absolute indirect jump (jmp [rip+0]) followed by the absolute target
   * address.  Zydis decodes only the 6-byte jump and would then mis-decode
   * the trailing address as code, so recognize the full 14-byte form up
   * front and leave it untouched (it is position-independent).  This is what
   * lets a second hook on an already-hooked function keep chaining. */
  if (code[0] == 0xFF && code[1] == 0x25 &&
      code[2] == 0 && code[3] == 0 && code[4] == 0 && code[5] == 0) {
    return 14;
  }
#endif

#ifdef SAMPGDK_64BIT
  ZydisMachineMode machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
  ZydisStackWidth stack_width = ZYDIS_STACK_WIDTH_64;
#else
  ZydisMachineMode machine_mode = ZYDIS_MACHINE_MODE_LEGACY_32;
  ZydisStackWidth stack_width = ZYDIS_STACK_WIDTH_32;
#endif

  if (ZYAN_FAILED(ZydisDecoderInit(&decoder, machine_mode, stack_width))) {
    return 0;
  }

  if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, code,
      ZYDIS_MAX_INSTRUCTION_LENGTH, &insn, operands))) {
    return 0;
  }

  /* Relative branch: re-base the 32-bit rel32 field (E8/E9 rel32, 0F 8x
   * Jcc rel32, ...).  Short branches (rel8/rel16) are not re-based. */
  for (i = 0; i < 2; i++) {
    if (insn.raw.imm[i].is_relative && insn.raw.imm[i].size == 32) {
      reloc_offset = insn.raw.imm[i].offset;
      break;
    }
  }

  /* RIP-relative memory operand (64-bit): Zydis flags it with the
   * IS_RELATIVE attribute and its disp32 is the field to re-base.  In 32-bit
   * mode mod=00 rm=101 is an absolute address, so no attribute is set. */
  if (reloc_offset == 0 && (insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE) &&
      insn.raw.disp.size == 32) {
    reloc_offset = insn.raw.disp.offset;
  }

  if (reloc != NULL) {
    *reloc = reloc_offset;
  }

  return insn.length;
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

  /* No explicit instruction-cache flush is needed on x86/x64: the
   * hardware's cache-coherency protocol (plus the natural serialization of
   * a call to the patched function, which is always on another thread or
   * after a synchronization point in practice) makes the new bytes visible
   * to every core. The kernel flush APIs would be no-ops here anyway;
   * they only matter on architectures with explicit I-cache maintenance
   * (e.g. ARM), which sampgdk does not target. */

  return hook;
}

void sampgdk_hook_free(sampgdk_hook_t hook) {
  free(hook);
}

void *sampgdk_hook_trampoline(sampgdk_hook_t hook) {
  return hook->trampoline;
}
