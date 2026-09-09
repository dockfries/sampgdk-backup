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
#if SAMPGDK_WINDOWS
  uint8_t trampoline[_SAMPGDK_HOOK_TRAMPOLINE_SIZE];
#else
  uint8_t *trampoline;
  size_t trampoline_size;
#endif
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

/* The "unprotect" function name is misleading, but was kept here to make
 * the code more consistent with Windows memory handling.
 * This function changes the memory regions permissions to read/write
 * (without execute) */
static int _sampgdk_hook_set_prot(void *address, size_t size, int prot) {
  long pagesize = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)address;
  uintptr_t page_start = start & ~((uintptr_t)pagesize - 1);
  size_t offset = start - page_start;
  size_t span = size + offset;

  return mprotect((void *)page_start, span, prot);
}

static void *_sampgdk_hook_unprotect(void *address, size_t size) {
  if (_sampgdk_hook_set_prot(address, size, PROT_READ | PROT_WRITE) != 0) {
    return NULL;
  }

  return address;
}

/* This function changes the memory region's permissions to read/execute
 * (without write)
 * SELinux does not like memory region's that are both writable and
 * executable at the same time, so we just remove the write permission for
 * that memory region */
static void *_sampgdk_hook_protect(void *address, size_t size) {
  if (_sampgdk_hook_set_prot(address, size, PROT_READ | PROT_EXEC) != 0) {
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
  size_t insn_len = 0;

  if ((hook = (sampgdk_hook_t)malloc(sizeof(*hook))) == NULL) {
    return NULL;
  }

#if SAMPGDK_WINDOWS

  /* In Windows, we can simply allocate the trampoline in heap memory,
   * no special memory handling required */
  _sampgdk_hook_unprotect(src, _SAMPGDK_HOOK_JMP_SIZE);
  _sampgdk_hook_unprotect(hook->trampoline, _SAMPGDK_HOOK_TRAMPOLINE_SIZE);

#else

  /* To prevent SELinux from triggering an "execheap" AVC denial, the
   * trampoline must be allocated in a separate memory region with mmap.
   * We can then control the memory region's permissions properly. */
  long pagesize = sysconf(_SC_PAGESIZE);

  hook->trampoline_size = (size_t)pagesize;
  hook->trampoline = mmap(
    NULL,
    hook->trampoline_size,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS,
    -1,
    0
  );

  if (hook->trampoline == MAP_FAILED) {
    free(hook);
    return NULL;
  }

#endif

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
#if !SAMPGDK_WINDOWS
    munmap(hook->trampoline, hook->trampoline_size);
#endif
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

#if !SAMPGDK_WINDOWS
  /* On Linux we are using a separate memory region allocated by mmap for
   * the trampoline. To prevent SELinux from triggering an "execheap" AVC
   * denial, we need to change the permissions for the trampoline to
   * read/execute only. */
  if (_sampgdk_hook_protect(hook->trampoline, hook->trampoline_size) == NULL) {
    munmap(hook->trampoline, hook->trampoline_size);
    free(hook);
    return NULL;
  }
#endif

  if (_sampgdk_hook_unprotect(src, _SAMPGDK_HOOK_JMP_SIZE) == NULL) {
#if !SAMPGDK_WINDOWS
    munmap(hook->trampoline, hook->trampoline_size);
#endif
    free(hook);
    return NULL;
  }

  _sampgdk_hook_write_jmp(src, dst, 0);

#if !SAMPGDK_WINDOWS
  /* Set src back to read/execute. If this fails the hook is unusable: src
   * has already been patched, so there is no safe way to roll it back. */
  if (_sampgdk_hook_protect(src, _SAMPGDK_HOOK_JMP_SIZE) == NULL) {
    sampgdk_log_error("mprotect src->RX failed, hook aborted (src may be left writable)");
    munmap(hook->trampoline, hook->trampoline_size);
    free(hook);
    return NULL;
  }
#endif

  return hook;
}

void sampgdk_hook_free(sampgdk_hook_t hook) {
#if !SAMPGDK_WINDOWS
  /* On Linux we need to release the trampoline memory region that we
   * previously allocated using mmap */
  munmap(hook->trampoline, hook->trampoline_size);
#endif

  free(hook);
}

void *sampgdk_hook_trampoline(sampgdk_hook_t hook) {
  return hook->trampoline;
}
