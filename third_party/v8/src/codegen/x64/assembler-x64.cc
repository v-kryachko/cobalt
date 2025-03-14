// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/x64/assembler-x64.h"

#include <cstring>

#if V8_TARGET_ARCH_X64

#if V8_LIBC_MSVCRT
#include <intrin.h>  // _xgetbv()
#endif
#if V8_OS_MACOSX
#include <sys/sysctl.h>
#endif

#include "src/base/bits.h"
#include "src/base/cpu.h"
#include "src/base/platform/wrappers.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/string-constants.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/init/v8.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// Implementation of CpuFeatures

namespace {

V8_INLINE uint64_t xgetbv(unsigned int xcr) {
#if V8_LIBC_MSVCRT
  return _xgetbv(xcr);
#else
  unsigned eax, edx;
  // Check xgetbv; this uses a .byte sequence instead of the instruction
  // directly because older assemblers do not include support for xgetbv and
  // there is no easy way to conditionally compile based on the assembler
  // used.
  __asm__ volatile(".byte 0x0F, 0x01, 0xD0" : "=a"(eax), "=d"(edx) : "c"(xcr));
  return static_cast<uint64_t>(eax) | (static_cast<uint64_t>(edx) << 32);
#endif
}

bool OSHasAVXSupport() {
#if V8_OS_MACOSX
  // Mac OS X up to 10.9 has a bug where AVX transitions were indeed being
  // caused by ISRs, so we detect that here and disable AVX in that case.
  char buffer[128];
  size_t buffer_size = arraysize(buffer);
  int ctl_name[] = {CTL_KERN, KERN_OSRELEASE};
  if (sysctl(ctl_name, 2, buffer, &buffer_size, nullptr, 0) != 0) {
    FATAL("V8 failed to get kernel version");
  }
  // The buffer now contains a string of the form XX.YY.ZZ, where
  // XX is the major kernel version component.
  char* period_pos = strchr(buffer, '.');
  DCHECK_NOT_NULL(period_pos);
  *period_pos = '\0';
  long kernel_version_major = strtol(buffer, nullptr, 10);  // NOLINT
  if (kernel_version_major <= 13) return false;
#endif  // V8_OS_MACOSX
  // Check whether OS claims to support AVX.
  uint64_t feature_mask = xgetbv(0);  // XCR_XFEATURE_ENABLED_MASK
  return (feature_mask & 0x6) == 0x6;
}

}  // namespace

void CpuFeatures::ProbeImpl(bool cross_compile) {
  base::CPU cpu;
  CHECK(cpu.has_sse2());  // SSE2 support is mandatory.
  CHECK(cpu.has_cmov());  // CMOV support is mandatory.

  // Only use statically determined features for cross compile (snapshot).
  if (cross_compile) return;

  if (cpu.has_sse42() && FLAG_enable_sse4_2) supported_ |= 1u << SSE4_2;
  if (cpu.has_sse41() && FLAG_enable_sse4_1) {
    supported_ |= 1u << SSE4_1;
    supported_ |= 1u << SSSE3;
  }
  if (cpu.has_ssse3() && FLAG_enable_ssse3) supported_ |= 1u << SSSE3;
  if (cpu.has_sse3() && FLAG_enable_sse3) supported_ |= 1u << SSE3;
  // SAHF is not generally available in long mode.
  if (cpu.has_sahf() && FLAG_enable_sahf) supported_ |= 1u << SAHF;
  if (cpu.has_avx() && FLAG_enable_avx && cpu.has_osxsave() &&
      OSHasAVXSupport()) {
    supported_ |= 1u << AVX;
  }
  if (cpu.has_avx2() && FLAG_enable_avx2 && IsSupported(AVX)) {
    supported_ |= 1u << AVX2;
  }
  if (cpu.has_fma3() && FLAG_enable_fma3 && cpu.has_osxsave() &&
      OSHasAVXSupport()) {
    supported_ |= 1u << FMA3;
  }
  if (cpu.has_bmi1() && FLAG_enable_bmi1) supported_ |= 1u << BMI1;
  if (cpu.has_bmi2() && FLAG_enable_bmi2) supported_ |= 1u << BMI2;
  if (cpu.has_lzcnt() && FLAG_enable_lzcnt) supported_ |= 1u << LZCNT;
  if (cpu.has_popcnt() && FLAG_enable_popcnt) supported_ |= 1u << POPCNT;
  if (strcmp(FLAG_mcpu, "auto") == 0) {
    if (cpu.is_atom()) supported_ |= 1u << ATOM;
  } else if (strcmp(FLAG_mcpu, "atom") == 0) {
    supported_ |= 1u << ATOM;
  }
}

void CpuFeatures::PrintTarget() {}
void CpuFeatures::PrintFeatures() {
  printf(
      "SSE3=%d SSSE3=%d SSE4_1=%d SSE4_2=%d SAHF=%d AVX=%d AVX2=%d FMA3=%d "
      "BMI1=%d "
      "BMI2=%d "
      "LZCNT=%d "
      "POPCNT=%d ATOM=%d\n",
      CpuFeatures::IsSupported(SSE3), CpuFeatures::IsSupported(SSSE3),
      CpuFeatures::IsSupported(SSE4_1), CpuFeatures::IsSupported(SSE4_2),
      CpuFeatures::IsSupported(SAHF), CpuFeatures::IsSupported(AVX),
      CpuFeatures::IsSupported(AVX2), CpuFeatures::IsSupported(FMA3),
      CpuFeatures::IsSupported(BMI1), CpuFeatures::IsSupported(BMI2),
      CpuFeatures::IsSupported(LZCNT), CpuFeatures::IsSupported(POPCNT),
      CpuFeatures::IsSupported(ATOM));
}

// -----------------------------------------------------------------------------
// Implementation of RelocInfo

uint32_t RelocInfo::wasm_call_tag() const {
  DCHECK(rmode_ == WASM_CALL || rmode_ == WASM_STUB_CALL);
  return ReadUnalignedValue<uint32_t>(pc_);
}

// -----------------------------------------------------------------------------
// Implementation of Operand

Operand::Operand(Operand operand, int32_t offset) {
  DCHECK_GE(operand.data().len, 1);
  // Operand encodes REX ModR/M [SIB] [Disp].
  byte modrm = operand.data().buf[0];
  DCHECK_LT(modrm, 0xC0);  // Disallow mode 3 (register target).
  bool has_sib = ((modrm & 0x07) == 0x04);
  byte mode = modrm & 0xC0;
  int disp_offset = has_sib ? 2 : 1;
  int base_reg = (has_sib ? operand.data().buf[1] : modrm) & 0x07;
  // Mode 0 with rbp/r13 as ModR/M or SIB base register always has a 32-bit
  // displacement.
  bool is_baseless = (mode == 0) && (base_reg == 0x05);  // No base or RIP base.
  int32_t disp_value = 0;
  if (mode == 0x80 || is_baseless) {
    // Mode 2 or mode 0 with rbp/r13 as base: Word displacement.
    disp_value = ReadUnalignedValue<int32_t>(
        reinterpret_cast<Address>(&operand.data().buf[disp_offset]));
  } else if (mode == 0x40) {
    // Mode 1: Byte displacement.
    disp_value = static_cast<signed char>(operand.data().buf[disp_offset]);
  }

  // Write new operand with same registers, but with modified displacement.
  DCHECK(offset >= 0 ? disp_value + offset > disp_value
                     : disp_value + offset < disp_value);  // No overflow.
  disp_value += offset;
  data_.rex = operand.data().rex;
  if (!is_int8(disp_value) || is_baseless) {
    // Need 32 bits of displacement, mode 2 or mode 1 with register rbp/r13.
    data_.buf[0] = (modrm & 0x3F) | (is_baseless ? 0x00 : 0x80);
    data_.len = disp_offset + 4;
    WriteUnalignedValue(reinterpret_cast<Address>(&data_.buf[disp_offset]),
                        disp_value);
  } else if (disp_value != 0 || (base_reg == 0x05)) {
    // Need 8 bits of displacement.
    data_.buf[0] = (modrm & 0x3F) | 0x40;  // Mode 1.
    data_.len = disp_offset + 1;
    data_.buf[disp_offset] = static_cast<byte>(disp_value);
  } else {
    // Need no displacement.
    data_.buf[0] = (modrm & 0x3F);  // Mode 0.
    data_.len = disp_offset;
  }
  if (has_sib) {
    data_.buf[1] = operand.data().buf[1];
  }
}

bool Operand::AddressUsesRegister(Register reg) const {
  int code = reg.code();
  DCHECK_NE(data_.buf[0] & 0xC0, 0xC0);  // Always a memory operand.
  // Start with only low three bits of base register. Initial decoding
  // doesn't distinguish on the REX.B bit.
  int base_code = data_.buf[0] & 0x07;
  if (base_code == rsp.code()) {
    // SIB byte present in buf_[1].
    // Check the index register from the SIB byte + REX.X prefix.
    int index_code = ((data_.buf[1] >> 3) & 0x07) | ((data_.rex & 0x02) << 2);
    // Index code (including REX.X) of 0x04 (rsp) means no index register.
    if (index_code != rsp.code() && index_code == code) return true;
    // Add REX.B to get the full base register code.
    base_code = (data_.buf[1] & 0x07) | ((data_.rex & 0x01) << 3);
    // A base register of 0x05 (rbp) with mod = 0 means no base register.
    if (base_code == rbp.code() && ((data_.buf[0] & 0xC0) == 0)) return false;
    return code == base_code;
  } else {
    // A base register with low bits of 0x05 (rbp or r13) and mod = 0 means
    // no base register.
    if (base_code == rbp.code() && ((data_.buf[0] & 0xC0) == 0)) return false;
    base_code |= ((data_.rex & 0x01) << 3);
    return code == base_code;
  }
}

void Assembler::AllocateAndInstallRequestedHeapObjects(Isolate* isolate) {
  DCHECK_IMPLIES(isolate == nullptr, heap_object_requests_.empty());
  for (auto& request : heap_object_requests_) {
    Address pc = reinterpret_cast<Address>(buffer_start_) + request.offset();
    switch (request.kind()) {
      case HeapObjectRequest::kHeapNumber: {
        Handle<HeapNumber> object =
            isolate->factory()->NewHeapNumber<AllocationType::kOld>(
                request.heap_number());
        WriteUnalignedValue(pc, object);
        break;
      }
      case HeapObjectRequest::kStringConstant: {
        const StringConstantBase* str = request.string();
        CHECK_NOT_NULL(str);
        Handle<String> allocated = str->AllocateStringConstant(isolate);
        WriteUnalignedValue(pc, allocated);
        break;
      }
    }
  }
}

// Partial Constant Pool.
bool ConstPool::AddSharedEntry(uint64_t data, int offset) {
  auto existing = entries_.find(data);
  if (existing == entries_.end()) {
    entries_.insert(std::make_pair(data, offset + kMoveImm64Offset));
    return false;
  }

  // Make sure this is called with strictly ascending offsets.
  DCHECK_GT(offset + kMoveImm64Offset, existing->second);

  entries_.insert(std::make_pair(data, offset + kMoveRipRelativeDispOffset));
  return true;
}

bool ConstPool::TryRecordEntry(intptr_t data, RelocInfo::Mode mode) {
  if (!FLAG_partial_constant_pool) return false;
  DCHECK_WITH_MSG(
      FLAG_text_is_readable,
      "The partial constant pool requires a readable .text section");
  if (!RelocInfo::IsShareableRelocMode(mode)) return false;

  // Currently, partial constant pool only handles the following kinds of
  // RelocInfo.
  if (mode != RelocInfo::NONE && mode != RelocInfo::EXTERNAL_REFERENCE &&
      mode != RelocInfo::OFF_HEAP_TARGET)
    return false;

  uint64_t raw_data = static_cast<uint64_t>(data);
  int offset = assm_->pc_offset();
  return AddSharedEntry(raw_data, offset);
}

bool ConstPool::IsMoveRipRelative(Address instr) {
  return (ReadUnalignedValue<uint32_t>(instr) & kMoveRipRelativeMask) ==
         kMoveRipRelativeInstr;
}

void ConstPool::Clear() { entries_.clear(); }

void ConstPool::PatchEntries() {
  for (EntryMap::iterator iter = entries_.begin(); iter != entries_.end();
       iter = entries_.upper_bound(iter->first)) {
    std::pair<EntryMap::iterator, EntryMap::iterator> range =
        entries_.equal_range(iter->first);
    int constant_entry_offset = 0;
    for (EntryMap::iterator it = range.first; it != range.second; it++) {
      if (it == range.first) {
        constant_entry_offset = it->second;
        continue;
      }

      DCHECK_GT(constant_entry_offset, 0);
      DCHECK_LT(constant_entry_offset, it->second);
      int32_t disp32 =
          constant_entry_offset - (it->second + kRipRelativeDispSize);
      Address disp_addr = assm_->addr_at(it->second);

      // Check if the instruction is actually a rip-relative move.
      DCHECK(IsMoveRipRelative(disp_addr - kMoveRipRelativeDispOffset));
      // The displacement of the rip-relative move should be 0 before patching.
      DCHECK(ReadUnalignedValue<uint32_t>(disp_addr) == 0);
      WriteUnalignedValue(disp_addr, disp32);
    }
  }
  Clear();
}

void Assembler::PatchConstPool() {
  // There is nothing to do if there are no pending entries.
  if (constpool_.IsEmpty()) {
    return;
  }
  constpool_.PatchEntries();
}

bool Assembler::UseConstPoolFor(RelocInfo::Mode rmode) {
  if (!FLAG_partial_constant_pool) return false;
  return (rmode == RelocInfo::NONE || rmode == RelocInfo::EXTERNAL_REFERENCE ||
          rmode == RelocInfo::OFF_HEAP_TARGET);
}

// -----------------------------------------------------------------------------
// Implementation of Assembler.

Assembler::Assembler(const AssemblerOptions& options,
                     std::unique_ptr<AssemblerBuffer> buffer)
    : AssemblerBase(options, std::move(buffer)), constpool_(this) {
  reloc_info_writer.Reposition(buffer_start_ + buffer_->size(), pc_);
  if (CpuFeatures::IsSupported(SSE4_2)) {
    EnableCpuFeature(SSE4_1);
  }
  if (CpuFeatures::IsSupported(SSE4_1)) {
    EnableCpuFeature(SSSE3);
  }

#if defined(V8_OS_WIN_X64)
  if (options.collect_win64_unwind_info) {
    xdata_encoder_ = std::make_unique<win64_unwindinfo::XdataEncoder>(*this);
  }
#endif
}

void Assembler::GetCode(Isolate* isolate, CodeDesc* desc,
                        SafepointTableBuilder* safepoint_table_builder,
                        int handler_table_offset) {
  // As a crutch to avoid having to add manual Align calls wherever we use a
  // raw workflow to create Code objects (mostly in tests), add another Align
  // call here. It does no harm - the end of the Code object is aligned to the
  // (larger) kCodeAlignment anyways.
  // TODO(jgruber): Consider moving responsibility for proper alignment to
  // metadata table builders (safepoint, handler, constant pool, code
  // comments).
  DataAlign(Code::kMetadataAlignment);

  PatchConstPool();
  DCHECK(constpool_.IsEmpty());

  const int code_comments_size = WriteCodeComments();

  // At this point overflow() may be true, but the gap ensures
  // that we are still not overlapping instructions and relocation info.
  DCHECK(pc_ <= reloc_info_writer.pos());  // No overlap.

  AllocateAndInstallRequestedHeapObjects(isolate);

  // Set up code descriptor.
  // TODO(jgruber): Reconsider how these offsets and sizes are maintained up to
  // this point to make CodeDesc initialization less fiddly.

  static constexpr int kConstantPoolSize = 0;
  const int instruction_size = pc_offset();
  const int code_comments_offset = instruction_size - code_comments_size;
  const int constant_pool_offset = code_comments_offset - kConstantPoolSize;
  const int handler_table_offset2 = (handler_table_offset == kNoHandlerTable)
                                        ? constant_pool_offset
                                        : handler_table_offset;
  const int safepoint_table_offset =
      (safepoint_table_builder == kNoSafepointTable)
          ? handler_table_offset2
          : safepoint_table_builder->GetCodeOffset();
  const int reloc_info_offset =
      static_cast<int>(reloc_info_writer.pos() - buffer_->start());
  CodeDesc::Initialize(desc, this, safepoint_table_offset,
                       handler_table_offset2, constant_pool_offset,
                       code_comments_offset, reloc_info_offset);
}

void Assembler::FinalizeJumpOptimizationInfo() {
  // Collection stage
  auto jump_opt = jump_optimization_info();
  if (jump_opt && jump_opt->is_collecting()) {
    auto& bitmap = jump_opt->farjmp_bitmap();
    int num = static_cast<int>(farjmp_positions_.size());
    if (num && bitmap.empty()) {
      bool can_opt = false;

      bitmap.resize((num + 31) / 32, 0);
      for (int i = 0; i < num; i++) {
        int disp_pos = farjmp_positions_[i];
        int disp = long_at(disp_pos);
        if (is_int8(disp)) {
          bitmap[i / 32] |= 1 << (i & 31);
          can_opt = true;
        }
      }
      if (can_opt) {
        jump_opt->set_optimizable();
      }
    }
  }
}

#if defined(V8_OS_WIN_X64)
win64_unwindinfo::BuiltinUnwindInfo Assembler::GetUnwindInfo() const {
  DCHECK(options().collect_win64_unwind_info);
  DCHECK_NOT_NULL(xdata_encoder_);
  return win64_unwindinfo::BuiltinUnwindInfo();
}
#endif

void Assembler::Align(int m) {
  DCHECK(base::bits::IsPowerOfTwo(m));
  int delta = (m - (pc_offset() & (m - 1))) & (m - 1);
  Nop(delta);
}

void Assembler::CodeTargetAlign() {
  Align(16);  // Preferred alignment of jump targets on x64.
}

bool Assembler::IsNop(Address addr) {
  byte* a = reinterpret_cast<byte*>(addr);
  while (*a == 0x66) a++;
  if (*a == 0x90) return true;
  if (a[0] == 0xF && a[1] == 0x1F) return true;
  return false;
}

void Assembler::bind_to(Label* L, int pos) {
  DCHECK(!L->is_bound());                  // Label may only be bound once.
  DCHECK(0 <= pos && pos <= pc_offset());  // Position must be valid.
  if (L->is_linked()) {
    int current = L->pos();
    int next = long_at(current);
    while (next != current) {
      if (current >= 4 && long_at(current - 4) == 0) {
        // Absolute address.
        intptr_t imm64 = reinterpret_cast<intptr_t>(buffer_start_ + pos);
        WriteUnalignedValue(addr_at(current - 4), imm64);
        internal_reference_positions_.push_back(current - 4);
      } else {
        // Relative address, relative to point after address.
        int imm32 = pos - (current + sizeof(int32_t));
        long_at_put(current, imm32);
      }
      current = next;
      next = long_at(next);
    }
    // Fix up last fixup on linked list.
    if (current >= 4 && long_at(current - 4) == 0) {
      // Absolute address.
      intptr_t imm64 = reinterpret_cast<intptr_t>(buffer_start_ + pos);
      WriteUnalignedValue(addr_at(current - 4), imm64);
      internal_reference_positions_.push_back(current - 4);
    } else {
      // Relative address, relative to point after address.
      int imm32 = pos - (current + sizeof(int32_t));
      long_at_put(current, imm32);
    }
  }
  while (L->is_near_linked()) {
    int fixup_pos = L->near_link_pos();
    int offset_to_next =
        static_cast<int>(*reinterpret_cast<int8_t*>(addr_at(fixup_pos)));
    DCHECK_LE(offset_to_next, 0);
    int disp = pos - (fixup_pos + sizeof(int8_t));
    CHECK(is_int8(disp));
    set_byte_at(fixup_pos, disp);
    if (offset_to_next < 0) {
      L->link_to(fixup_pos + offset_to_next, Label::kNear);
    } else {
      L->UnuseNear();
    }
  }

  // Optimization stage
  auto jump_opt = jump_optimization_info();
  if (jump_opt && jump_opt->is_optimizing()) {
    auto it = label_farjmp_maps_.find(L);
    if (it != label_farjmp_maps_.end()) {
      auto& pos_vector = it->second;
      for (auto fixup_pos : pos_vector) {
        int disp = pos - (fixup_pos + sizeof(int8_t));
        CHECK(is_int8(disp));
        set_byte_at(fixup_pos, disp);
      }
      label_farjmp_maps_.erase(it);
    }
  }
  L->bind_to(pos);
}

void Assembler::bind(Label* L) { bind_to(L, pc_offset()); }

void Assembler::record_farjmp_position(Label* L, int pos) {
  auto& pos_vector = label_farjmp_maps_[L];
  pos_vector.push_back(pos);
}

bool Assembler::is_optimizable_farjmp(int idx) {
  if (predictable_code_size()) return false;

  auto jump_opt = jump_optimization_info();
  CHECK(jump_opt->is_optimizing());

  auto& bitmap = jump_opt->farjmp_bitmap();
  CHECK(idx < static_cast<int>(bitmap.size() * 32));
  return !!(bitmap[idx / 32] & (1 << (idx & 31)));
}

void Assembler::GrowBuffer() {
  DCHECK(buffer_overflow());

  // Compute new buffer size.
  DCHECK_EQ(buffer_start_, buffer_->start());
  int old_size = buffer_->size();
  int new_size = 2 * old_size;

  // Some internal data structures overflow for very large buffers,
  // they must ensure that kMaximalBufferSize is not too large.
  if (new_size > kMaximalBufferSize) {
    V8::FatalProcessOutOfMemory(nullptr, "Assembler::GrowBuffer");
  }

  // Set up new buffer.
  std::unique_ptr<AssemblerBuffer> new_buffer = buffer_->Grow(new_size);
  DCHECK_EQ(new_size, new_buffer->size());
  byte* new_start = new_buffer->start();

  // Copy the data.
  intptr_t pc_delta = new_start - buffer_start_;
  intptr_t rc_delta = (new_start + new_size) - (buffer_start_ + old_size);
  size_t reloc_size = (buffer_start_ + old_size) - reloc_info_writer.pos();
  MemMove(new_start, buffer_start_, pc_offset());
  MemMove(rc_delta + reloc_info_writer.pos(), reloc_info_writer.pos(),
          reloc_size);

  // Switch buffers.
  buffer_ = std::move(new_buffer);
  buffer_start_ = new_start;
  pc_ += pc_delta;
  reloc_info_writer.Reposition(reloc_info_writer.pos() + rc_delta,
                               reloc_info_writer.last_pc() + pc_delta);

  // Relocate internal references.
  for (auto pos : internal_reference_positions_) {
    Address p = reinterpret_cast<Address>(buffer_start_ + pos);
    WriteUnalignedValue(p, ReadUnalignedValue<intptr_t>(p) + pc_delta);
  }

  DCHECK(!buffer_overflow());
}

void Assembler::emit_operand(int code, Operand adr) {
  DCHECK(is_uint3(code));
  const unsigned length = adr.data().len;
  DCHECK_GT(length, 0);

  // Emit updated ModR/M byte containing the given register.
  DCHECK_EQ(adr.data().buf[0] & 0x38, 0);
  *pc_++ = adr.data().buf[0] | code << 3;

  // Recognize RIP relative addressing.
  if (adr.data().buf[0] == 5) {
    DCHECK_EQ(9u, length);
    Label* label = ReadUnalignedValue<Label*>(
        reinterpret_cast<Address>(&adr.data().buf[1]));
    if (label->is_bound()) {
      int offset =
          label->pos() - pc_offset() - sizeof(int32_t) + adr.data().addend;
      DCHECK_GE(0, offset);
      emitl(offset);
    } else if (label->is_linked()) {
      emitl(label->pos());
      label->link_to(pc_offset() - sizeof(int32_t));
    } else {
      DCHECK(label->is_unused());
      int32_t current = pc_offset();
      emitl(current);
      label->link_to(current);
    }
  } else {
    // Emit the rest of the encoded operand.
    for (unsigned i = 1; i < length; i++) *pc_++ = adr.data().buf[i];
  }
}

// Assembler Instruction implementations.

void Assembler::arithmetic_op(byte opcode, Register reg, Operand op, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(reg, op, size);
  emit(opcode);
  emit_operand(reg, op);
}

void Assembler::arithmetic_op(byte opcode, Register reg, Register rm_reg,
                              int size) {
  EnsureSpace ensure_space(this);
  DCHECK_EQ(opcode & 0xC6, 2);
  if (rm_reg.low_bits() == 4) {  // Forces SIB byte.
    // Swap reg and rm_reg and change opcode operand order.
    emit_rex(rm_reg, reg, size);
    emit(opcode ^ 0x02);
    emit_modrm(rm_reg, reg);
  } else {
    emit_rex(reg, rm_reg, size);
    emit(opcode);
    emit_modrm(reg, rm_reg);
  }
}

void Assembler::arithmetic_op_16(byte opcode, Register reg, Register rm_reg) {
  EnsureSpace ensure_space(this);
  DCHECK_EQ(opcode & 0xC6, 2);
  if (rm_reg.low_bits() == 4) {  // Forces SIB byte.
    // Swap reg and rm_reg and change opcode operand order.
    emit(0x66);
    emit_optional_rex_32(rm_reg, reg);
    emit(opcode ^ 0x02);
    emit_modrm(rm_reg, reg);
  } else {
    emit(0x66);
    emit_optional_rex_32(reg, rm_reg);
    emit(opcode);
    emit_modrm(reg, rm_reg);
  }
}

void Assembler::arithmetic_op_16(byte opcode, Register reg, Operand rm_reg) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(reg, rm_reg);
  emit(opcode);
  emit_operand(reg, rm_reg);
}

void Assembler::arithmetic_op_8(byte opcode, Register reg, Operand op) {
  EnsureSpace ensure_space(this);
  if (!reg.is_byte_register()) {
    emit_rex_32(reg, op);
  } else {
    emit_optional_rex_32(reg, op);
  }
  emit(opcode);
  emit_operand(reg, op);
}

void Assembler::arithmetic_op_8(byte opcode, Register reg, Register rm_reg) {
  EnsureSpace ensure_space(this);
  DCHECK_EQ(opcode & 0xC6, 2);
  if (rm_reg.low_bits() == 4) {  // Forces SIB byte.
    // Swap reg and rm_reg and change opcode operand order.
    if (!rm_reg.is_byte_register() || !reg.is_byte_register()) {
      // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
      emit_rex_32(rm_reg, reg);
    }
    emit(opcode ^ 0x02);
    emit_modrm(rm_reg, reg);
  } else {
    if (!reg.is_byte_register() || !rm_reg.is_byte_register()) {
      // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
      emit_rex_32(reg, rm_reg);
    }
    emit(opcode);
    emit_modrm(reg, rm_reg);
  }
}

void Assembler::immediate_arithmetic_op(byte subcode, Register dst,
                                        Immediate src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  if (is_int8(src.value_) && RelocInfo::IsNone(src.rmode_)) {
    emit(0x83);
    emit_modrm(subcode, dst);
    emit(src.value_);
  } else if (dst == rax) {
    emit(0x05 | (subcode << 3));
    emit(src);
  } else {
    emit(0x81);
    emit_modrm(subcode, dst);
    emit(src);
  }
}

void Assembler::immediate_arithmetic_op(byte subcode, Operand dst,
                                        Immediate src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  if (is_int8(src.value_) && RelocInfo::IsNone(src.rmode_)) {
    emit(0x83);
    emit_operand(subcode, dst);
    emit(src.value_);
  } else {
    emit(0x81);
    emit_operand(subcode, dst);
    emit(src);
  }
}

void Assembler::immediate_arithmetic_op_16(byte subcode, Register dst,
                                           Immediate src) {
  EnsureSpace ensure_space(this);
  emit(0x66);  // Operand size override prefix.
  emit_optional_rex_32(dst);
  if (is_int8(src.value_)) {
    emit(0x83);
    emit_modrm(subcode, dst);
    emit(src.value_);
  } else if (dst == rax) {
    emit(0x05 | (subcode << 3));
    emitw(src.value_);
  } else {
    emit(0x81);
    emit_modrm(subcode, dst);
    emitw(src.value_);
  }
}

void Assembler::immediate_arithmetic_op_16(byte subcode, Operand dst,
                                           Immediate src) {
  EnsureSpace ensure_space(this);
  emit(0x66);  // Operand size override prefix.
  emit_optional_rex_32(dst);
  if (is_int8(src.value_)) {
    emit(0x83);
    emit_operand(subcode, dst);
    emit(src.value_);
  } else {
    emit(0x81);
    emit_operand(subcode, dst);
    emitw(src.value_);
  }
}

void Assembler::immediate_arithmetic_op_8(byte subcode, Operand dst,
                                          Immediate src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  DCHECK(is_int8(src.value_) || is_uint8(src.value_));
  emit(0x80);
  emit_operand(subcode, dst);
  emit(src.value_);
}

void Assembler::immediate_arithmetic_op_8(byte subcode, Register dst,
                                          Immediate src) {
  EnsureSpace ensure_space(this);
  if (!dst.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst);
  }
  DCHECK(is_int8(src.value_) || is_uint8(src.value_));
  emit(0x80);
  emit_modrm(subcode, dst);
  emit(src.value_);
}

void Assembler::shift(Register dst, Immediate shift_amount, int subcode,
                      int size) {
  EnsureSpace ensure_space(this);
  DCHECK(size == kInt64Size ? is_uint6(shift_amount.value_)
                            : is_uint5(shift_amount.value_));
  if (shift_amount.value_ == 1) {
    emit_rex(dst, size);
    emit(0xD1);
    emit_modrm(subcode, dst);
  } else {
    emit_rex(dst, size);
    emit(0xC1);
    emit_modrm(subcode, dst);
    emit(shift_amount.value_);
  }
}

void Assembler::shift(Operand dst, Immediate shift_amount, int subcode,
                      int size) {
  EnsureSpace ensure_space(this);
  DCHECK(size == kInt64Size ? is_uint6(shift_amount.value_)
                            : is_uint5(shift_amount.value_));
  if (shift_amount.value_ == 1) {
    emit_rex(dst, size);
    emit(0xD1);
    emit_operand(subcode, dst);
  } else {
    emit_rex(dst, size);
    emit(0xC1);
    emit_operand(subcode, dst);
    emit(shift_amount.value_);
  }
}

void Assembler::shift(Register dst, int subcode, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xD3);
  emit_modrm(subcode, dst);
}

void Assembler::shift(Operand dst, int subcode, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xD3);
  emit_operand(subcode, dst);
}

void Assembler::bswapl(Register dst) {
  EnsureSpace ensure_space(this);
  emit_rex_32(dst);
  emit(0x0F);
  emit(0xC8 + dst.low_bits());
}

void Assembler::bswapq(Register dst) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst);
  emit(0x0F);
  emit(0xC8 + dst.low_bits());
}

void Assembler::btq(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0xA3);
  emit_operand(src, dst);
}

void Assembler::btsq(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0xAB);
  emit_operand(src, dst);
}

void Assembler::btsq(Register dst, Immediate imm8) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst);
  emit(0x0F);
  emit(0xBA);
  emit_modrm(0x5, dst);
  emit(imm8.value_);
}

void Assembler::btrq(Register dst, Immediate imm8) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst);
  emit(0x0F);
  emit(0xBA);
  emit_modrm(0x6, dst);
  emit(imm8.value_);
}

void Assembler::bsrl(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_modrm(dst, src);
}

void Assembler::bsrl(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_operand(dst, src);
}

void Assembler::bsrq(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_modrm(dst, src);
}

void Assembler::bsrq(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_operand(dst, src);
}

void Assembler::bsfl(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_modrm(dst, src);
}

void Assembler::bsfl(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_operand(dst, src);
}

void Assembler::bsfq(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_modrm(dst, src);
}

void Assembler::bsfq(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_operand(dst, src);
}

void Assembler::pblendw(XMMRegister dst, Operand src, uint8_t mask) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0E);
  emit(mask);
}

void Assembler::pblendw(XMMRegister dst, XMMRegister src, uint8_t mask) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0E);
  emit(mask);
}

void Assembler::palignr(XMMRegister dst, Operand src, uint8_t mask) {
  ssse3_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0F);
  emit(mask);
}

void Assembler::palignr(XMMRegister dst, XMMRegister src, uint8_t mask) {
  ssse3_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0F);
  emit(mask);
}

void Assembler::call(Label* L) {
  EnsureSpace ensure_space(this);
  // 1110 1000 #32-bit disp.
  emit(0xE8);
  if (L->is_bound()) {
    int offset = L->pos() - pc_offset() - sizeof(int32_t);
    DCHECK_LE(offset, 0);
    emitl(offset);
  } else if (L->is_linked()) {
    emitl(L->pos());
    L->link_to(pc_offset() - sizeof(int32_t));
  } else {
    DCHECK(L->is_unused());
    int32_t current = pc_offset();
    emitl(current);
    L->link_to(current);
  }
}

void Assembler::call(Address entry, RelocInfo::Mode rmode) {
  DCHECK(RelocInfo::IsRuntimeEntry(rmode));
  EnsureSpace ensure_space(this);
  // 1110 1000 #32-bit disp.
  emit(0xE8);
  emit_runtime_entry(entry, rmode);
}

void Assembler::call(Handle<Code> target, RelocInfo::Mode rmode) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  DCHECK(target->IsExecutable());
  EnsureSpace ensure_space(this);
  // 1110 1000 #32-bit disp.
  emit(0xE8);
  RecordRelocInfo(rmode);
  int code_target_index = AddCodeTarget(target);
  emitl(code_target_index);
}

void Assembler::near_call(intptr_t disp, RelocInfo::Mode rmode) {
  EnsureSpace ensure_space(this);
  emit(0xE8);
  DCHECK(is_int32(disp));
  RecordRelocInfo(rmode);
  emitl(static_cast<int32_t>(disp));
}

void Assembler::near_jmp(intptr_t disp, RelocInfo::Mode rmode) {
  EnsureSpace ensure_space(this);
  emit(0xE9);
  DCHECK(is_int32(disp));
  if (!RelocInfo::IsNone(rmode)) RecordRelocInfo(rmode);
  emitl(static_cast<int32_t>(disp));
}

void Assembler::call(Register adr) {
  EnsureSpace ensure_space(this);
  // Opcode: FF /2 r64.
  emit_optional_rex_32(adr);
  emit(0xFF);
  emit_modrm(0x2, adr);
}

void Assembler::call(Operand op) {
  EnsureSpace ensure_space(this);
  // Opcode: FF /2 m64.
  emit_optional_rex_32(op);
  emit(0xFF);
  emit_operand(0x2, op);
}

void Assembler::clc() {
  EnsureSpace ensure_space(this);
  emit(0xF8);
}

void Assembler::cld() {
  EnsureSpace ensure_space(this);
  emit(0xFC);
}

void Assembler::cdq() {
  EnsureSpace ensure_space(this);
  emit(0x99);
}

void Assembler::cmovq(Condition cc, Register dst, Register src) {
  if (cc == always) {
    movq(dst, src);
  } else if (cc == never) {
    return;
  }
  // No need to check CpuInfo for CMOV support, it's a required part of the
  // 64-bit architecture.
  DCHECK_GE(cc, 0);  // Use mov for unconditional moves.
  EnsureSpace ensure_space(this);
  // Opcode: REX.W 0f 40 + cc /r.
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x40 + cc);
  emit_modrm(dst, src);
}

void Assembler::cmovq(Condition cc, Register dst, Operand src) {
  if (cc == always) {
    movq(dst, src);
  } else if (cc == never) {
    return;
  }
  DCHECK_GE(cc, 0);
  EnsureSpace ensure_space(this);
  // Opcode: REX.W 0f 40 + cc /r.
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x40 + cc);
  emit_operand(dst, src);
}

void Assembler::cmovl(Condition cc, Register dst, Register src) {
  if (cc == always) {
    movl(dst, src);
  } else if (cc == never) {
    return;
  }
  DCHECK_GE(cc, 0);
  EnsureSpace ensure_space(this);
  // Opcode: 0f 40 + cc /r.
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x40 + cc);
  emit_modrm(dst, src);
}

void Assembler::cmovl(Condition cc, Register dst, Operand src) {
  if (cc == always) {
    movl(dst, src);
  } else if (cc == never) {
    return;
  }
  DCHECK_GE(cc, 0);
  EnsureSpace ensure_space(this);
  // Opcode: 0f 40 + cc /r.
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x40 + cc);
  emit_operand(dst, src);
}

void Assembler::cmpb_al(Immediate imm8) {
  DCHECK(is_int8(imm8.value_) || is_uint8(imm8.value_));
  EnsureSpace ensure_space(this);
  emit(0x3C);
  emit(imm8.value_);
}

void Assembler::lock() {
  EnsureSpace ensure_space(this);
  emit(0xF0);
}

void Assembler::xaddb(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_8(src, dst);
  emit(0x0F);
  emit(0xC0);
  emit_operand(src, dst);
}

void Assembler::xaddw(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0xC1);
  emit_operand(src, dst);
}

void Assembler::xaddl(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0xC1);
  emit_operand(src, dst);
}

void Assembler::xaddq(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex(src, dst, kInt64Size);
  emit(0x0F);
  emit(0xC1);
  emit_operand(src, dst);
}

void Assembler::cmpxchgb(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  if (!src.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(src, dst);
  } else {
    emit_optional_rex_32(src, dst);
  }
  emit(0x0F);
  emit(0xB0);
  emit_operand(src, dst);
}

void Assembler::cmpxchgw(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0xB1);
  emit_operand(src, dst);
}

void Assembler::emit_cmpxchg(Operand dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, dst, size);
  emit(0x0F);
  emit(0xB1);
  emit_operand(src, dst);
}

void Assembler::mfence() {
  EnsureSpace ensure_space(this);
  emit(0x0F);
  emit(0xAE);
  emit(0xF0);
}

void Assembler::lfence() {
  EnsureSpace ensure_space(this);
  emit(0x0F);
  emit(0xAE);
  emit(0xE8);
}

void Assembler::cpuid() {
  EnsureSpace ensure_space(this);
  emit(0x0F);
  emit(0xA2);
}

void Assembler::cqo() {
  EnsureSpace ensure_space(this);
  emit_rex_64();
  emit(0x99);
}

void Assembler::emit_dec(Register dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xFF);
  emit_modrm(0x1, dst);
}

void Assembler::emit_dec(Operand dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xFF);
  emit_operand(1, dst);
}

void Assembler::decb(Register dst) {
  EnsureSpace ensure_space(this);
  if (!dst.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst);
  }
  emit(0xFE);
  emit_modrm(0x1, dst);
}

void Assembler::decb(Operand dst) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  emit(0xFE);
  emit_operand(1, dst);
}

void Assembler::hlt() {
  EnsureSpace ensure_space(this);
  emit(0xF4);
}

void Assembler::emit_idiv(Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, size);
  emit(0xF7);
  emit_modrm(0x7, src);
}

void Assembler::emit_div(Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, size);
  emit(0xF7);
  emit_modrm(0x6, src);
}

void Assembler::emit_imul(Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, size);
  emit(0xF7);
  emit_modrm(0x5, src);
}

void Assembler::emit_imul(Operand src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, size);
  emit(0xF7);
  emit_operand(0x5, src);
}

void Assembler::emit_imul(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  emit(0x0F);
  emit(0xAF);
  emit_modrm(dst, src);
}

void Assembler::emit_imul(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  emit(0x0F);
  emit(0xAF);
  emit_operand(dst, src);
}

void Assembler::emit_imul(Register dst, Register src, Immediate imm, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  if (is_int8(imm.value_)) {
    emit(0x6B);
    emit_modrm(dst, src);
    emit(imm.value_);
  } else {
    emit(0x69);
    emit_modrm(dst, src);
    emitl(imm.value_);
  }
}

void Assembler::emit_imul(Register dst, Operand src, Immediate imm, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  if (is_int8(imm.value_)) {
    emit(0x6B);
    emit_operand(dst, src);
    emit(imm.value_);
  } else {
    emit(0x69);
    emit_operand(dst, src);
    emitl(imm.value_);
  }
}

void Assembler::emit_inc(Register dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xFF);
  emit_modrm(0x0, dst);
}

void Assembler::emit_inc(Operand dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xFF);
  emit_operand(0, dst);
}

void Assembler::int3() {
  EnsureSpace ensure_space(this);
  emit(0xCC);
}

void Assembler::j(Condition cc, Label* L, Label::Distance distance) {
  if (cc == always) {
    jmp(L, distance);
    return;
  } else if (cc == never) {
    return;
  }
  EnsureSpace ensure_space(this);
  DCHECK(is_uint4(cc));
  if (L->is_bound()) {
    const int short_size = 2;
    const int long_size = 6;
    int offs = L->pos() - pc_offset();
    DCHECK_LE(offs, 0);
    // Determine whether we can use 1-byte offsets for backwards branches,
    // which have a max range of 128 bytes.

    // We also need to check predictable_code_size() flag here, because on x64,
    // when the full code generator recompiles code for debugging, some places
    // need to be padded out to a certain size. The debugger is keeping track of
    // how often it did this so that it can adjust return addresses on the
    // stack, but if the size of jump instructions can also change, that's not
    // enough and the calculated offsets would be incorrect.
    if (is_int8(offs - short_size) && !predictable_code_size()) {
      // 0111 tttn #8-bit disp.
      emit(0x70 | cc);
      emit((offs - short_size) & 0xFF);
    } else {
      // 0000 1111 1000 tttn #32-bit disp.
      emit(0x0F);
      emit(0x80 | cc);
      emitl(offs - long_size);
    }
  } else if (distance == Label::kNear) {
    // 0111 tttn #8-bit disp
    emit(0x70 | cc);
    byte disp = 0x00;
    if (L->is_near_linked()) {
      int offset = L->near_link_pos() - pc_offset();
      DCHECK(is_int8(offset));
      disp = static_cast<byte>(offset & 0xFF);
    }
    L->link_to(pc_offset(), Label::kNear);
    emit(disp);
  } else {
    auto jump_opt = jump_optimization_info();
    if (V8_UNLIKELY(jump_opt)) {
      if (jump_opt->is_optimizing() && is_optimizable_farjmp(farjmp_num_++)) {
        // 0111 tttn #8-bit disp
        emit(0x70 | cc);
        record_farjmp_position(L, pc_offset());
        emit(0);
        return;
      }
      if (jump_opt->is_collecting()) {
        farjmp_positions_.push_back(pc_offset() + 2);
      }
    }
    if (L->is_linked()) {
      // 0000 1111 1000 tttn #32-bit disp.
      emit(0x0F);
      emit(0x80 | cc);
      emitl(L->pos());
      L->link_to(pc_offset() - sizeof(int32_t));
    } else {
      DCHECK(L->is_unused());
      emit(0x0F);
      emit(0x80 | cc);
      int32_t current = pc_offset();
      emitl(current);
      L->link_to(current);
    }
  }
}

void Assembler::j(Condition cc, Address entry, RelocInfo::Mode rmode) {
  DCHECK(RelocInfo::IsRuntimeEntry(rmode));
  EnsureSpace ensure_space(this);
  DCHECK(is_uint4(cc));
  emit(0x0F);
  emit(0x80 | cc);
  emit_runtime_entry(entry, rmode);
}

void Assembler::j(Condition cc, Handle<Code> target, RelocInfo::Mode rmode) {
  if (cc == always) {
    jmp(target, rmode);
    return;
  } else if (cc == never) {
    return;
  }
  EnsureSpace ensure_space(this);
  DCHECK(is_uint4(cc));
  // 0000 1111 1000 tttn #32-bit disp.
  emit(0x0F);
  emit(0x80 | cc);
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  RecordRelocInfo(rmode);
  int code_target_index = AddCodeTarget(target);
  emitl(code_target_index);
}

void Assembler::jmp_rel(int32_t offset) {
  EnsureSpace ensure_space(this);
  // The offset is encoded relative to the next instruction.
  constexpr int32_t kShortJmpDisplacement = 1 + sizeof(int8_t);
  constexpr int32_t kNearJmpDisplacement = 1 + sizeof(int32_t);
  DCHECK_LE(std::numeric_limits<int32_t>::min() + kNearJmpDisplacement, offset);
  if (is_int8(offset - kShortJmpDisplacement) && !predictable_code_size()) {
    // 0xEB #8-bit disp.
    emit(0xEB);
    emit(offset - kShortJmpDisplacement);
  } else {
    // 0xE9 #32-bit disp.
    emit(0xE9);
    emitl(offset - kNearJmpDisplacement);
  }
}

void Assembler::jmp(Label* L, Label::Distance distance) {
  const int long_size = sizeof(int32_t);

  if (L->is_bound()) {
    int offset = L->pos() - pc_offset();
    DCHECK_LE(offset, 0);  // backward jump.
    jmp_rel(offset);
    return;
  }

  EnsureSpace ensure_space(this);
  if (distance == Label::kNear) {
    emit(0xEB);
    byte disp = 0x00;
    if (L->is_near_linked()) {
      int offset = L->near_link_pos() - pc_offset();
      DCHECK(is_int8(offset));
      disp = static_cast<byte>(offset & 0xFF);
    }
    L->link_to(pc_offset(), Label::kNear);
    emit(disp);
  } else {
    auto jump_opt = jump_optimization_info();
    if (V8_UNLIKELY(jump_opt)) {
      if (jump_opt->is_optimizing() && is_optimizable_farjmp(farjmp_num_++)) {
        emit(0xEB);
        record_farjmp_position(L, pc_offset());
        emit(0);
        return;
      }
      if (jump_opt->is_collecting()) {
        farjmp_positions_.push_back(pc_offset() + 1);
      }
    }
    if (L->is_linked()) {
      // 1110 1001 #32-bit disp.
      emit(0xE9);
      emitl(L->pos());
      L->link_to(pc_offset() - long_size);
    } else {
      // 1110 1001 #32-bit disp.
      DCHECK(L->is_unused());
      emit(0xE9);
      int32_t current = pc_offset();
      emitl(current);
      L->link_to(current);
    }
  }
}

void Assembler::jmp(Handle<Code> target, RelocInfo::Mode rmode) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  EnsureSpace ensure_space(this);
  // 1110 1001 #32-bit disp.
  emit(0xE9);
  RecordRelocInfo(rmode);
  int code_target_index = AddCodeTarget(target);
  emitl(code_target_index);
}

void Assembler::jmp(Register target) {
  EnsureSpace ensure_space(this);
  // Opcode FF/4 r64.
  emit_optional_rex_32(target);
  emit(0xFF);
  emit_modrm(0x4, target);
}

void Assembler::jmp(Operand src) {
  EnsureSpace ensure_space(this);
  // Opcode FF/4 m64.
  emit_optional_rex_32(src);
  emit(0xFF);
  emit_operand(0x4, src);
}

void Assembler::emit_lea(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  emit(0x8D);
  emit_operand(dst, src);
}

void Assembler::load_rax(Address value, RelocInfo::Mode mode) {
  EnsureSpace ensure_space(this);
  emit(0x48);  // REX.W
  emit(0xA1);
  emit(Immediate64(value, mode));
}

void Assembler::load_rax(ExternalReference ref) {
  load_rax(ref.address(), RelocInfo::EXTERNAL_REFERENCE);
}

void Assembler::leave() {
  EnsureSpace ensure_space(this);
  emit(0xC9);
}

void Assembler::movb(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  if (!dst.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst, src);
  } else {
    emit_optional_rex_32(dst, src);
  }
  emit(0x8A);
  emit_operand(dst, src);
}

void Assembler::movb(Register dst, Immediate imm) {
  EnsureSpace ensure_space(this);
  if (!dst.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst);
  }
  emit(0xB0 + dst.low_bits());
  emit(imm.value_);
}

void Assembler::movb(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  if (!src.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(src, dst);
  } else {
    emit_optional_rex_32(src, dst);
  }
  emit(0x88);
  emit_operand(src, dst);
}

void Assembler::movb(Operand dst, Immediate imm) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  emit(0xC6);
  emit_operand(0x0, dst);
  emit(static_cast<byte>(imm.value_));
}

void Assembler::movw(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x8B);
  emit_operand(dst, src);
}

void Assembler::movw(Operand dst, Register src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(src, dst);
  emit(0x89);
  emit_operand(src, dst);
}

void Assembler::movw(Operand dst, Immediate imm) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst);
  emit(0xC7);
  emit_operand(0x0, dst);
  emit(static_cast<byte>(imm.value_ & 0xFF));
  emit(static_cast<byte>(imm.value_ >> 8));
}

void Assembler::emit_mov(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  emit(0x8B);
  emit_operand(dst, src);
}

void Assembler::emit_mov(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  if (src.low_bits() == 4) {
    emit_rex(src, dst, size);
    emit(0x89);
    emit_modrm(src, dst);
  } else {
    emit_rex(dst, src, size);
    emit(0x8B);
    emit_modrm(dst, src);
  }

#if defined(V8_OS_WIN_X64)
  if (xdata_encoder_ && dst == rbp && src == rsp) {
    xdata_encoder_->onMovRbpRsp();
  }
#endif
}

void Assembler::emit_mov(Operand dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(src, dst, size);
  emit(0x89);
  emit_operand(src, dst);
}

void Assembler::emit_mov(Register dst, Immediate value, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  if (size == kInt64Size) {
    emit(0xC7);
    emit_modrm(0x0, dst);
  } else {
    DCHECK_EQ(size, kInt32Size);
    emit(0xB8 + dst.low_bits());
  }
  emit(value);
}

void Assembler::emit_mov(Operand dst, Immediate value, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xC7);
  emit_operand(0x0, dst);
  emit(value);
}

void Assembler::emit_mov(Register dst, Immediate64 value, int size) {
  DCHECK_EQ(size, kInt64Size);
  if (constpool_.TryRecordEntry(value.value_, value.rmode_)) {
    // Emit rip-relative move with offset = 0
    Label label;
    emit_mov(dst, Operand(&label, 0), size);
    bind(&label);
  } else {
    EnsureSpace ensure_space(this);
    emit_rex(dst, size);
    emit(0xB8 | dst.low_bits());
    emit(value);
  }
}

void Assembler::movq_imm64(Register dst, int64_t value) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, kInt64Size);
  emit(0xB8 | dst.low_bits());
  emitq(static_cast<uint64_t>(value));
}

void Assembler::movq_heap_number(Register dst, double value) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, kInt64Size);
  emit(0xB8 | dst.low_bits());
  RequestHeapObject(HeapObjectRequest(value));
  emit(Immediate64(kNullAddress, RelocInfo::FULL_EMBEDDED_OBJECT));
}

void Assembler::movq_string(Register dst, const StringConstantBase* str) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, kInt64Size);
  emit(0xB8 | dst.low_bits());
  RequestHeapObject(HeapObjectRequest(str));
  emit(Immediate64(kNullAddress, RelocInfo::FULL_EMBEDDED_OBJECT));
}

// Loads the ip-relative location of the src label into the target location
// (as a 32-bit offset sign extended to 64-bit).
void Assembler::movl(Operand dst, Label* src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  emit(0xC7);
  emit_operand(0, dst);
  if (src->is_bound()) {
    int offset = src->pos() - pc_offset() - sizeof(int32_t);
    DCHECK_LE(offset, 0);
    emitl(offset);
  } else if (src->is_linked()) {
    emitl(src->pos());
    src->link_to(pc_offset() - sizeof(int32_t));
  } else {
    DCHECK(src->is_unused());
    int32_t current = pc_offset();
    emitl(current);
    src->link_to(current);
  }
}

void Assembler::movsxbl(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  if (!src.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst, src);
  } else {
    emit_optional_rex_32(dst, src);
  }
  emit(0x0F);
  emit(0xBE);
  emit_modrm(dst, src);
}

void Assembler::movsxbl(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBE);
  emit_operand(dst, src);
}

void Assembler::movsxbq(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBE);
  emit_operand(dst, src);
}

void Assembler::movsxbq(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBE);
  emit_modrm(dst, src);
}

void Assembler::movsxwl(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBF);
  emit_modrm(dst, src);
}

void Assembler::movsxwl(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBF);
  emit_operand(dst, src);
}

void Assembler::movsxwq(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBF);
  emit_operand(dst, src);
}

void Assembler::movsxwq(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBF);
  emit_modrm(dst, src);
}

void Assembler::movsxlq(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x63);
  emit_modrm(dst, src);
}

void Assembler::movsxlq(Register dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(dst, src);
  emit(0x63);
  emit_operand(dst, src);
}

void Assembler::emit_movzxb(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  // 32 bit operations zero the top 32 bits of 64 bit registers.  Therefore
  // there is no need to make this a 64 bit operation.
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xB6);
  emit_operand(dst, src);
}

void Assembler::emit_movzxb(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  // 32 bit operations zero the top 32 bits of 64 bit registers.  Therefore
  // there is no need to make this a 64 bit operation.
  if (!src.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(dst, src);
  } else {
    emit_optional_rex_32(dst, src);
  }
  emit(0x0F);
  emit(0xB6);
  emit_modrm(dst, src);
}

void Assembler::emit_movzxw(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  // 32 bit operations zero the top 32 bits of 64 bit registers.  Therefore
  // there is no need to make this a 64 bit operation.
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xB7);
  emit_operand(dst, src);
}

void Assembler::emit_movzxw(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  // 32 bit operations zero the top 32 bits of 64 bit registers.  Therefore
  // there is no need to make this a 64 bit operation.
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xB7);
  emit_modrm(dst, src);
}

void Assembler::repmovsb() {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit(0xA4);
}

void Assembler::repmovsw() {
  EnsureSpace ensure_space(this);
  emit(0x66);  // Operand size override.
  emit(0xF3);
  emit(0xA4);
}

void Assembler::emit_repmovs(int size) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex(size);
  emit(0xA5);
}

void Assembler::repstosl() {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit(0xAB);
}

void Assembler::repstosq() {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64();
  emit(0xAB);
}

void Assembler::mull(Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src);
  emit(0xF7);
  emit_modrm(0x4, src);
}

void Assembler::mull(Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src);
  emit(0xF7);
  emit_operand(0x4, src);
}

void Assembler::mulq(Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(src);
  emit(0xF7);
  emit_modrm(0x4, src);
}

void Assembler::negb(Register reg) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_8(reg);
  emit(0xF6);
  emit_modrm(0x3, reg);
}

void Assembler::negw(Register reg) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(reg);
  emit(0xF7);
  emit_modrm(0x3, reg);
}

void Assembler::negl(Register reg) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(reg);
  emit(0xF7);
  emit_modrm(0x3, reg);
}

void Assembler::negq(Register reg) {
  EnsureSpace ensure_space(this);
  emit_rex_64(reg);
  emit(0xF7);
  emit_modrm(0x3, reg);
}

void Assembler::negb(Operand op) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(op);
  emit(0xF6);
  emit_operand(0x3, op);
}

void Assembler::negw(Operand op) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(op);
  emit(0xF7);
  emit_operand(0x3, op);
}

void Assembler::negl(Operand op) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(op);
  emit(0xF7);
  emit_operand(0x3, op);
}

void Assembler::negq(Operand op) {
  EnsureSpace ensure_space(this);
  emit_rex_64(op);
  emit(0xF7);
  emit_operand(0x3, op);
}

void Assembler::nop() {
  EnsureSpace ensure_space(this);
  emit(0x90);
}

void Assembler::emit_not(Register dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xF7);
  emit_modrm(0x2, dst);
}

void Assembler::emit_not(Operand dst, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, size);
  emit(0xF7);
  emit_operand(2, dst);
}

void Assembler::Nop(int n) {
  DCHECK_LE(0, n);
  // The recommended muti-byte sequences of NOP instructions from the Intel 64
  // and IA-32 Architectures Software Developer's Manual.
  //
  // Len Assembly                                    Byte Sequence
  // 2   66 NOP                                      66 90H
  // 3   NOP DWORD ptr [EAX]                         0F 1F 00H
  // 4   NOP DWORD ptr [EAX + 00H]                   0F 1F 40 00H
  // 5   NOP DWORD ptr [EAX + EAX*1 + 00H]           0F 1F 44 00 00H
  // 6   66 NOP DWORD ptr [EAX + EAX*1 + 00H]        66 0F 1F 44 00 00H
  // 7   NOP DWORD ptr [EAX + 00000000H]             0F 1F 80 00 00 00 00H
  // 8   NOP DWORD ptr [EAX + EAX*1 + 00000000H]     0F 1F 84 00 00 00 00 00H
  // 9   66 NOP DWORD ptr [EAX + EAX*1 + 00000000H]  66 0F 1F 84 00 00 00 00 00H

  constexpr const char* kNopSequences =
      "\x66\x90"                               // length 1 (@1) / 2 (@0)
      "\x0F\x1F\x00"                           // length 3 (@2)
      "\x0F\x1F\x40\x00"                       // length 4 (@5)
      "\x66\x0F\x1F\x44\x00\x00"               // length 5 (@10) / 6 (@9)
      "\x0F\x1F\x80\x00\x00\x00\x00"           // length 7 (@15)
      "\x66\x0F\x1F\x84\x00\x00\x00\x00\x00";  // length 8 (@23) / 9 (@22)
  constexpr int8_t kNopOffsets[10] = {0, 1, 0, 2, 5, 10, 9, 15, 23, 22};

  do {
    EnsureSpace ensure_space(this);
    int nop_bytes = std::min(n, 9);
    const char* sequence = kNopSequences + kNopOffsets[nop_bytes];
    base::Memcpy(pc_, sequence, nop_bytes);
    pc_ += nop_bytes;
    n -= nop_bytes;
  } while (n);
}

void Assembler::popq(Register dst) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  emit(0x58 | dst.low_bits());
}

void Assembler::popq(Operand dst) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst);
  emit(0x8F);
  emit_operand(0, dst);
}

void Assembler::popfq() {
  EnsureSpace ensure_space(this);
  emit(0x9D);
}

void Assembler::pushq(Register src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src);
  emit(0x50 | src.low_bits());

#if defined(V8_OS_WIN_X64)
  if (xdata_encoder_ && src == rbp) {
    xdata_encoder_->onPushRbp();
  }
#endif
}

void Assembler::pushq(Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src);
  emit(0xFF);
  emit_operand(6, src);
}

void Assembler::pushq(Immediate value) {
  EnsureSpace ensure_space(this);
  if (is_int8(value.value_)) {
    emit(0x6A);
    emit(value.value_);  // Emit low byte of value.
  } else {
    emit(0x68);
    emitl(value.value_);
  }
}

void Assembler::pushq_imm32(int32_t imm32) {
  EnsureSpace ensure_space(this);
  emit(0x68);
  emitl(imm32);
}

void Assembler::pushfq() {
  EnsureSpace ensure_space(this);
  emit(0x9C);
}

void Assembler::ret(int imm16) {
  EnsureSpace ensure_space(this);
  DCHECK(is_uint16(imm16));
  if (imm16 == 0) {
    emit(0xC3);
  } else {
    emit(0xC2);
    emit(imm16 & 0xFF);
    emit((imm16 >> 8) & 0xFF);
  }
}

void Assembler::ud2() {
  EnsureSpace ensure_space(this);
  emit(0x0F);
  emit(0x0B);
}

void Assembler::setcc(Condition cc, Register reg) {
  if (cc > last_condition) {
    movb(reg, Immediate(cc == always ? 1 : 0));
    return;
  }
  EnsureSpace ensure_space(this);
  DCHECK(is_uint4(cc));
  if (!reg.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(reg);
  }
  emit(0x0F);
  emit(0x90 | cc);
  emit_modrm(0x0, reg);
}

void Assembler::shld(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0xA5);
  emit_modrm(src, dst);
}

void Assembler::shrd(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0xAD);
  emit_modrm(src, dst);
}

void Assembler::xchgb(Register reg, Operand op) {
  EnsureSpace ensure_space(this);
  if (!reg.is_byte_register()) {
    // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
    emit_rex_32(reg, op);
  } else {
    emit_optional_rex_32(reg, op);
  }
  emit(0x86);
  emit_operand(reg, op);
}

void Assembler::xchgw(Register reg, Operand op) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(reg, op);
  emit(0x87);
  emit_operand(reg, op);
}

void Assembler::emit_xchg(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  if (src == rax || dst == rax) {  // Single-byte encoding
    Register other = src == rax ? dst : src;
    emit_rex(other, size);
    emit(0x90 | other.low_bits());
  } else if (dst.low_bits() == 4) {
    emit_rex(dst, src, size);
    emit(0x87);
    emit_modrm(dst, src);
  } else {
    emit_rex(src, dst, size);
    emit(0x87);
    emit_modrm(src, dst);
  }
}

void Assembler::emit_xchg(Register dst, Operand src, int size) {
  EnsureSpace ensure_space(this);
  emit_rex(dst, src, size);
  emit(0x87);
  emit_operand(dst, src);
}

void Assembler::store_rax(Address dst, RelocInfo::Mode mode) {
  EnsureSpace ensure_space(this);
  emit(0x48);  // REX.W
  emit(0xA3);
  emit(Immediate64(dst, mode));
}

void Assembler::store_rax(ExternalReference ref) {
  store_rax(ref.address(), RelocInfo::EXTERNAL_REFERENCE);
}

void Assembler::sub_sp_32(uint32_t imm) {
  emit_rex_64();
  emit(0x81);  // using a literal 32-bit immediate.
  emit_modrm(0x5, rsp);
  emitl(imm);
}

void Assembler::testb(Register dst, Register src) {
  EnsureSpace ensure_space(this);
  emit_test(dst, src, sizeof(int8_t));
}

void Assembler::testb(Register reg, Immediate mask) {
  DCHECK(is_int8(mask.value_) || is_uint8(mask.value_));
  emit_test(reg, mask, sizeof(int8_t));
}

void Assembler::testb(Operand op, Immediate mask) {
  DCHECK(is_int8(mask.value_) || is_uint8(mask.value_));
  emit_test(op, mask, sizeof(int8_t));
}

void Assembler::testb(Operand op, Register reg) {
  emit_test(op, reg, sizeof(int8_t));
}

void Assembler::testw(Register dst, Register src) {
  emit_test(dst, src, sizeof(uint16_t));
}

void Assembler::testw(Register reg, Immediate mask) {
  emit_test(reg, mask, sizeof(int16_t));
}

void Assembler::testw(Operand op, Immediate mask) {
  emit_test(op, mask, sizeof(int16_t));
}

void Assembler::testw(Operand op, Register reg) {
  emit_test(op, reg, sizeof(int16_t));
}

void Assembler::emit_test(Register dst, Register src, int size) {
  EnsureSpace ensure_space(this);
  if (src.low_bits() == 4) std::swap(dst, src);
  if (size == sizeof(int16_t)) {
    emit(0x66);
    size = sizeof(int32_t);
  }
  bool byte_operand = size == sizeof(int8_t);
  if (byte_operand) {
    size = sizeof(int32_t);
    if (!src.is_byte_register() || !dst.is_byte_register()) {
      emit_rex_32(dst, src);
    }
  } else {
    emit_rex(dst, src, size);
  }
  emit(byte_operand ? 0x84 : 0x85);
  emit_modrm(dst, src);
}

void Assembler::emit_test(Register reg, Immediate mask, int size) {
  if (is_uint8(mask.value_)) {
    size = sizeof(int8_t);
  } else if (is_uint16(mask.value_)) {
    size = sizeof(int16_t);
  }
  EnsureSpace ensure_space(this);
  bool half_word = size == sizeof(int16_t);
  if (half_word) {
    emit(0x66);
    size = sizeof(int32_t);
  }
  bool byte_operand = size == sizeof(int8_t);
  if (byte_operand) {
    size = sizeof(int32_t);
    if (!reg.is_byte_register()) emit_rex_32(reg);
  } else {
    emit_rex(reg, size);
  }
  if (reg == rax) {
    emit(byte_operand ? 0xA8 : 0xA9);
  } else {
    emit(byte_operand ? 0xF6 : 0xF7);
    emit_modrm(0x0, reg);
  }
  if (byte_operand) {
    emit(mask.value_);
  } else if (half_word) {
    emitw(mask.value_);
  } else {
    emit(mask);
  }
}

void Assembler::emit_test(Operand op, Immediate mask, int size) {
  if (is_uint8(mask.value_)) {
    size = sizeof(int8_t);
  } else if (is_uint16(mask.value_)) {
    size = sizeof(int16_t);
  }
  EnsureSpace ensure_space(this);
  bool half_word = size == sizeof(int16_t);
  if (half_word) {
    emit(0x66);
    size = sizeof(int32_t);
  }
  bool byte_operand = size == sizeof(int8_t);
  if (byte_operand) {
    size = sizeof(int32_t);
  }
  emit_rex(rax, op, size);
  emit(byte_operand ? 0xF6 : 0xF7);
  emit_operand(rax, op);  // Operation code 0
  if (byte_operand) {
    emit(mask.value_);
  } else if (half_word) {
    emitw(mask.value_);
  } else {
    emit(mask);
  }
}

void Assembler::emit_test(Operand op, Register reg, int size) {
  EnsureSpace ensure_space(this);
  if (size == sizeof(int16_t)) {
    emit(0x66);
    size = sizeof(int32_t);
  }
  bool byte_operand = size == sizeof(int8_t);
  if (byte_operand) {
    size = sizeof(int32_t);
    if (!reg.is_byte_register()) {
      // Register is not one of al, bl, cl, dl.  Its encoding needs REX.
      emit_rex_32(reg, op);
    } else {
      emit_optional_rex_32(reg, op);
    }
  } else {
    emit_rex(reg, op, size);
  }
  emit(byte_operand ? 0x84 : 0x85);
  emit_operand(reg, op);
}

// FPU instructions.

void Assembler::fld(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xD9, 0xC0, i);
}

void Assembler::fld1() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xE8);
}

void Assembler::fldz() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xEE);
}

void Assembler::fldpi() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xEB);
}

void Assembler::fldln2() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xED);
}

void Assembler::fld_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xD9);
  emit_operand(0, adr);
}

void Assembler::fld_d(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDD);
  emit_operand(0, adr);
}

void Assembler::fstp_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xD9);
  emit_operand(3, adr);
}

void Assembler::fstp_d(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDD);
  emit_operand(3, adr);
}

void Assembler::fstp(int index) {
  DCHECK(is_uint3(index));
  EnsureSpace ensure_space(this);
  emit_farith(0xDD, 0xD8, index);
}

void Assembler::fild_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDB);
  emit_operand(0, adr);
}

void Assembler::fild_d(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDF);
  emit_operand(5, adr);
}

void Assembler::fistp_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDB);
  emit_operand(3, adr);
}

void Assembler::fisttp_s(Operand adr) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDB);
  emit_operand(1, adr);
}

void Assembler::fisttp_d(Operand adr) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDD);
  emit_operand(1, adr);
}

void Assembler::fist_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDB);
  emit_operand(2, adr);
}

void Assembler::fistp_d(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDF);
  emit_operand(7, adr);
}

void Assembler::fabs() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xE1);
}

void Assembler::fchs() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xE0);
}

void Assembler::fcos() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xFF);
}

void Assembler::fsin() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xFE);
}

void Assembler::fptan() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF2);
}

void Assembler::fyl2x() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF1);
}

void Assembler::f2xm1() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF0);
}

void Assembler::fscale() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xFD);
}

void Assembler::fninit() {
  EnsureSpace ensure_space(this);
  emit(0xDB);
  emit(0xE3);
}

void Assembler::fadd(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDC, 0xC0, i);
}

void Assembler::fsub(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDC, 0xE8, i);
}

void Assembler::fisub_s(Operand adr) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(adr);
  emit(0xDA);
  emit_operand(4, adr);
}

void Assembler::fmul(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDC, 0xC8, i);
}

void Assembler::fdiv(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDC, 0xF8, i);
}

void Assembler::faddp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDE, 0xC0, i);
}

void Assembler::fsubp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDE, 0xE8, i);
}

void Assembler::fsubrp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDE, 0xE0, i);
}

void Assembler::fmulp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDE, 0xC8, i);
}

void Assembler::fdivp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDE, 0xF8, i);
}

void Assembler::fprem() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF8);
}

void Assembler::fprem1() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF5);
}

void Assembler::fxch(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xD9, 0xC8, i);
}

void Assembler::fincstp() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xF7);
}

void Assembler::ffree(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDD, 0xC0, i);
}

void Assembler::ftst() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xE4);
}

void Assembler::fucomp(int i) {
  EnsureSpace ensure_space(this);
  emit_farith(0xDD, 0xE8, i);
}

void Assembler::fucompp() {
  EnsureSpace ensure_space(this);
  emit(0xDA);
  emit(0xE9);
}

void Assembler::fucomi(int i) {
  EnsureSpace ensure_space(this);
  emit(0xDB);
  emit(0xE8 + i);
}

void Assembler::fucomip() {
  EnsureSpace ensure_space(this);
  emit(0xDF);
  emit(0xE9);
}

void Assembler::fcompp() {
  EnsureSpace ensure_space(this);
  emit(0xDE);
  emit(0xD9);
}

void Assembler::fnstsw_ax() {
  EnsureSpace ensure_space(this);
  emit(0xDF);
  emit(0xE0);
}

void Assembler::fwait() {
  EnsureSpace ensure_space(this);
  emit(0x9B);
}

void Assembler::frndint() {
  EnsureSpace ensure_space(this);
  emit(0xD9);
  emit(0xFC);
}

void Assembler::fnclex() {
  EnsureSpace ensure_space(this);
  emit(0xDB);
  emit(0xE2);
}

void Assembler::sahf() {
  // TODO(X64): Test for presence. Not all 64-bit intel CPU's have sahf
  // in 64-bit mode. Test CpuID.
  DCHECK(IsEnabled(SAHF));
  EnsureSpace ensure_space(this);
  emit(0x9E);
}

void Assembler::emit_farith(int b1, int b2, int i) {
  DCHECK(is_uint8(b1) && is_uint8(b2));  // wrong opcode
  DCHECK(is_uint3(i));                   // illegal stack offset
  emit(b1);
  emit(b2 + i);
}

// SSE 2 operations.

void Assembler::movd(XMMRegister dst, Register src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::movd(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::movd(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0x7E);
  emit_sse_operand(src, dst);
}

void Assembler::movq(XMMRegister dst, Register src) {
  // Mixing AVX and non-AVX is expensive, catch those cases
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::movq(XMMRegister dst, Operand src) {
  // Mixing AVX and non-AVX is expensive, catch those cases
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::movq(Register dst, XMMRegister src) {
  // Mixing AVX and non-AVX is expensive, catch those cases
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0x7E);
  emit_sse_operand(src, dst);
}

void Assembler::movq(XMMRegister dst, XMMRegister src) {
  // Mixing AVX and non-AVX is expensive, catch those cases
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  if (dst.low_bits() == 4) {
    // Avoid unnecessary SIB byte.
    emit(0xF3);
    emit_optional_rex_32(dst, src);
    emit(0x0F);
    emit(0x7E);
    emit_sse_operand(dst, src);
  } else {
    emit(0x66);
    emit_optional_rex_32(src, dst);
    emit(0x0F);
    emit(0xD6);
    emit_sse_operand(src, dst);
  }
}

void Assembler::movdqa(Operand dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0x7F);
  emit_sse_operand(src, dst);
}

void Assembler::movdqa(XMMRegister dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x6F);
  emit_sse_operand(dst, src);
}

void Assembler::movdqa(XMMRegister dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0x7F);
  emit_sse_operand(src, dst);
}

void Assembler::movdqu(Operand dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0x7F);
  emit_sse_operand(src, dst);
}

void Assembler::movdqu(XMMRegister dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x6F);
  emit_sse_operand(dst, src);
}

void Assembler::movdqu(XMMRegister dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x6F);
  emit_sse_operand(dst, src);
}

void Assembler::pinsrw(XMMRegister dst, Register src, uint8_t imm8) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC4);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::pinsrw(XMMRegister dst, Operand src, uint8_t imm8) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC4);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::pextrq(Register dst, XMMRegister src, int8_t imm8) {
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(src, dst);
  emit(0x0F);
  emit(0x3A);
  emit(0x16);
  emit_sse_operand(src, dst);
  emit(imm8);
}

void Assembler::pinsrq(XMMRegister dst, Register src, uint8_t imm8) {
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x3A);
  emit(0x22);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::pinsrq(XMMRegister dst, Operand src, uint8_t imm8) {
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x3A);
  emit(0x22);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::pinsrd(XMMRegister dst, Register src, uint8_t imm8) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x22, imm8);
}

void Assembler::pinsrd(XMMRegister dst, Operand src, uint8_t imm8) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x22);
  emit(imm8);
}

void Assembler::pinsrb(XMMRegister dst, Register src, uint8_t imm8) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x20, imm8);
}

void Assembler::pinsrb(XMMRegister dst, Operand src, uint8_t imm8) {
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x20);
  emit(imm8);
}

void Assembler::insertps(XMMRegister dst, XMMRegister src, byte imm8) {
  DCHECK(is_uint8(imm8));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x21);
  emit(imm8);
}

void Assembler::insertps(XMMRegister dst, Operand src, byte imm8) {
  DCHECK(is_uint8(imm8));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x21);
  emit(imm8);
}

void Assembler::movsd(Operand dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);  // double
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0x11);  // store
  emit_sse_operand(src, dst);
}

void Assembler::movsd(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);  // double
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);  // load
  emit_sse_operand(dst, src);
}

void Assembler::movsd(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);  // double
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);  // load
  emit_sse_operand(dst, src);
}

void Assembler::movaps(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  if (src.low_bits() == 4) {
    // Try to avoid an unnecessary SIB byte.
    emit_optional_rex_32(src, dst);
    emit(0x0F);
    emit(0x29);
    emit_sse_operand(src, dst);
  } else {
    emit_optional_rex_32(dst, src);
    emit(0x0F);
    emit(0x28);
    emit_sse_operand(dst, src);
  }
}

void Assembler::shufps(XMMRegister dst, XMMRegister src, byte imm8) {
  DCHECK(is_uint8(imm8));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC6);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::movapd(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  if (src.low_bits() == 4) {
    // Try to avoid an unnecessary SIB byte.
    emit(0x66);
    emit_optional_rex_32(src, dst);
    emit(0x0F);
    emit(0x29);
    emit_sse_operand(src, dst);
  } else {
    emit(0x66);
    emit_optional_rex_32(dst, src);
    emit(0x0F);
    emit(0x28);
    emit_sse_operand(dst, src);
  }
}

void Assembler::movupd(XMMRegister dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);
  emit_sse_operand(dst, src);
}

void Assembler::movupd(Operand dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0x11);
  emit_sse_operand(src, dst);
}

void Assembler::ucomiss(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2E);
  emit_sse_operand(dst, src);
}

void Assembler::ucomiss(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2E);
  emit_sse_operand(dst, src);
}

void Assembler::movss(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);  // single
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);  // load
  emit_sse_operand(dst, src);
}

void Assembler::movss(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);  // single
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);  // load
  emit_sse_operand(dst, src);
}

void Assembler::movss(Operand src, XMMRegister dst) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);  // single
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x11);  // store
  emit_sse_operand(dst, src);
}

void Assembler::movlps(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x12);
  emit_sse_operand(dst, src);
}

void Assembler::movlps(Operand src, XMMRegister dst) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x13);
  emit_sse_operand(dst, src);
}

void Assembler::movhps(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x16);
  emit_sse_operand(dst, src);
}

void Assembler::movhps(Operand src, XMMRegister dst) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x17);
  emit_sse_operand(dst, src);
}

void Assembler::cmpps(XMMRegister dst, XMMRegister src, int8_t cmp) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC2);
  emit_sse_operand(dst, src);
  emit(cmp);
}

void Assembler::cmpps(XMMRegister dst, Operand src, int8_t cmp) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC2);
  emit_sse_operand(dst, src);
  emit(cmp);
}

void Assembler::cmppd(XMMRegister dst, XMMRegister src, int8_t cmp) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC2);
  emit_sse_operand(dst, src);
  emit(cmp);
}

void Assembler::cmppd(XMMRegister dst, Operand src, int8_t cmp) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC2);
  emit_sse_operand(dst, src);
  emit(cmp);
}

void Assembler::cvttss2si(Register dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_operand(dst, src);
}

void Assembler::cvttss2si(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttsd2si(Register dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_operand(dst, src);
}

void Assembler::cvttsd2si(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttss2siq(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttss2siq(Register dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttsd2siq(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttsd2siq(Register dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2C);
  emit_sse_operand(dst, src);
}

void Assembler::cvttps2dq(XMMRegister dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x5B);
  emit_sse_operand(dst, src);
}

void Assembler::cvttps2dq(XMMRegister dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x5B);
  emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2sd(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2sd(XMMRegister dst, Register src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2ss(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2ss(XMMRegister dst, Register src) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtqsi2ss(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtqsi2ss(XMMRegister dst, Register src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtqsi2sd(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtqsi2sd(XMMRegister dst, Register src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtss2sd(XMMRegister dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x5A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtss2sd(XMMRegister dst, Operand src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x5A);
  emit_sse_operand(dst, src);
}

void Assembler::cvtsd2si(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x2D);
  emit_sse_operand(dst, src);
}

void Assembler::cvtsd2siq(Register dst, XMMRegister src) {
  DCHECK(!IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0x2D);
  emit_sse_operand(dst, src);
}

void Assembler::haddps(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x7C);
  emit_sse_operand(dst, src);
}

void Assembler::haddps(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x7C);
  emit_sse_operand(dst, src);
}

void Assembler::cmpltsd(XMMRegister dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xC2);
  emit_sse_operand(dst, src);
  emit(0x01);  // LT == 1
}

void Assembler::roundss(XMMRegister dst, XMMRegister src, RoundingMode mode) {
  DCHECK(!IsEnabled(AVX));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0A);
  // Mask precision exception.
  emit(static_cast<byte>(mode) | 0x8);
}

void Assembler::roundsd(XMMRegister dst, XMMRegister src, RoundingMode mode) {
  DCHECK(!IsEnabled(AVX));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x0B);
  // Mask precision exception.
  emit(static_cast<byte>(mode) | 0x8);
}

void Assembler::roundps(XMMRegister dst, XMMRegister src, RoundingMode mode) {
  DCHECK(!IsEnabled(AVX));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x08);
  // Mask precision exception.
  emit(static_cast<byte>(mode) | 0x8);
}

void Assembler::roundpd(XMMRegister dst, XMMRegister src, RoundingMode mode) {
  DCHECK(!IsEnabled(AVX));
  sse4_instr(dst, src, 0x66, 0x0F, 0x3A, 0x09);
  // Mask precision exception.
  emit(static_cast<byte>(mode) | 0x8);
}

void Assembler::movmskpd(Register dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x50);
  emit_sse_operand(dst, src);
}

void Assembler::movmskps(Register dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x50);
  emit_sse_operand(dst, src);
}

void Assembler::pmovmskb(Register dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xD7);
  emit_sse_operand(dst, src);
}

// AVX instructions

void Assembler::vmovddup(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, kF2, k0F, kWIG);
  emit(0x12);
  emit_sse_operand(dst, src);
}

void Assembler::vmovddup(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, kF2, k0F, kWIG);
  emit(0x12);
  emit_sse_operand(dst, src);
}

void Assembler::vmovshdup(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, kF3, k0F, kWIG);
  emit(0x16);
  emit_sse_operand(dst, src);
}

void Assembler::vbroadcastss(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, k66, k0F38, kW0);
  emit(0x18);
  emit_sse_operand(dst, src);
}

void Assembler::vbroadcastss(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX2));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, k66, k0F38, kW0);
  emit(0x18);
  emit_sse_operand(dst, src);
}

void Assembler::fma_instr(byte op, XMMRegister dst, XMMRegister src1,
                          XMMRegister src2, VectorLength l, SIMDPrefix pp,
                          LeadingOpcode m, VexW w) {
  DCHECK(IsEnabled(FMA3));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, l, pp, m, w);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::fma_instr(byte op, XMMRegister dst, XMMRegister src1,
                          Operand src2, VectorLength l, SIMDPrefix pp,
                          LeadingOpcode m, VexW w) {
  DCHECK(IsEnabled(FMA3));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, l, pp, m, w);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vmovd(XMMRegister dst, Register src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  XMMRegister isrc = XMMRegister::from_code(src.code());
  emit_vex_prefix(dst, xmm0, isrc, kL128, k66, k0F, kW0);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::vmovd(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, k66, k0F, kW0);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::vmovd(Register dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  XMMRegister idst = XMMRegister::from_code(dst.code());
  emit_vex_prefix(src, xmm0, idst, kL128, k66, k0F, kW0);
  emit(0x7E);
  emit_sse_operand(src, dst);
}

void Assembler::vmovq(XMMRegister dst, Register src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  XMMRegister isrc = XMMRegister::from_code(src.code());
  emit_vex_prefix(dst, xmm0, isrc, kL128, k66, k0F, kW1);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::vmovq(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, k66, k0F, kW1);
  emit(0x6E);
  emit_sse_operand(dst, src);
}

void Assembler::vmovq(Register dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  XMMRegister idst = XMMRegister::from_code(dst.code());
  emit_vex_prefix(src, xmm0, idst, kL128, k66, k0F, kW1);
  emit(0x7E);
  emit_sse_operand(src, dst);
}

void Assembler::vmovdqa(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, k66, k0F, kWIG);
  emit(0x6F);
  emit_sse_operand(dst, src);
}

void Assembler::vmovdqu(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kL128, kF3, k0F, kWIG);
  emit(0x6F);
  emit_sse_operand(dst, src);
}

void Assembler::vmovdqu(Operand dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(src, xmm0, dst, kL128, kF3, k0F, kWIG);
  emit(0x7F);
  emit_sse_operand(src, dst);
}

void Assembler::vmovdqu(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(src, xmm0, dst, kL128, kF3, k0F, kWIG);
  emit(0x7F);
  emit_sse_operand(src, dst);
}

void Assembler::vmovlps(XMMRegister dst, XMMRegister src1, Operand src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, kNone, k0F, kWIG);
  emit(0x12);
  emit_sse_operand(dst, src2);
}

void Assembler::vmovlps(Operand dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(src, xmm0, dst, kL128, kNone, k0F, kWIG);
  emit(0x13);
  emit_sse_operand(src, dst);
}

void Assembler::vmovhps(XMMRegister dst, XMMRegister src1, Operand src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, kNone, k0F, kWIG);
  emit(0x16);
  emit_sse_operand(dst, src2);
}

void Assembler::vmovhps(Operand dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(src, xmm0, dst, kL128, kNone, k0F, kWIG);
  emit(0x17);
  emit_sse_operand(src, dst);
}

void Assembler::vinstr(byte op, XMMRegister dst, XMMRegister src1,
                       XMMRegister src2, SIMDPrefix pp, LeadingOpcode m, VexW w,
                       CpuFeature feature) {
  DCHECK(IsEnabled(feature));
  DCHECK(feature == AVX || feature == AVX2);
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kLIG, pp, m, w);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vinstr(byte op, XMMRegister dst, XMMRegister src1, Operand src2,
                       SIMDPrefix pp, LeadingOpcode m, VexW w,
                       CpuFeature feature) {
  DCHECK(IsEnabled(feature));
  DCHECK(feature == AVX || feature == AVX2);
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kLIG, pp, m, w);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vps(byte op, XMMRegister dst, XMMRegister src1,
                    XMMRegister src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, kNone, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vps(byte op, XMMRegister dst, XMMRegister src1, Operand src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, kNone, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vps(byte op, XMMRegister dst, XMMRegister src1,
                    XMMRegister src2, byte imm8) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, kNone, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
  emit(imm8);
}

void Assembler::vpd(byte op, XMMRegister dst, XMMRegister src1,
                    XMMRegister src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, k66, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vpd(byte op, XMMRegister dst, XMMRegister src1, Operand src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kL128, k66, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vucomiss(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kLIG, kNone, k0F, kWIG);
  emit(0x2E);
  emit_sse_operand(dst, src);
}

void Assembler::vucomiss(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, xmm0, src, kLIG, kNone, k0F, kWIG);
  emit(0x2E);
  emit_sse_operand(dst, src);
}

void Assembler::vpmovmskb(Register dst, XMMRegister src) {
  XMMRegister idst = XMMRegister::from_code(dst.code());
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(idst, xmm0, src, kL128, k66, k0F, kWIG);
  emit(0xD7);
  emit_sse_operand(idst, src);
}

void Assembler::vss(byte op, XMMRegister dst, XMMRegister src1,
                    XMMRegister src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kLIG, kF3, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::vss(byte op, XMMRegister dst, XMMRegister src1, Operand src2) {
  DCHECK(IsEnabled(AVX));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, src1, src2, kLIG, kF3, k0F, kWIG);
  emit(op);
  emit_sse_operand(dst, src2);
}

void Assembler::bmi1q(byte op, Register reg, Register vreg, Register rm) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, kNone, k0F38, kW1);
  emit(op);
  emit_modrm(reg, rm);
}

void Assembler::bmi1q(byte op, Register reg, Register vreg, Operand rm) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, kNone, k0F38, kW1);
  emit(op);
  emit_operand(reg, rm);
}

void Assembler::bmi1l(byte op, Register reg, Register vreg, Register rm) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, kNone, k0F38, kW0);
  emit(op);
  emit_modrm(reg, rm);
}

void Assembler::bmi1l(byte op, Register reg, Register vreg, Operand rm) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, kNone, k0F38, kW0);
  emit(op);
  emit_operand(reg, rm);
}

void Assembler::tzcntq(Register dst, Register src) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_modrm(dst, src);
}

void Assembler::tzcntq(Register dst, Operand src) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_operand(dst, src);
}

void Assembler::tzcntl(Register dst, Register src) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_modrm(dst, src);
}

void Assembler::tzcntl(Register dst, Operand src) {
  DCHECK(IsEnabled(BMI1));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBC);
  emit_operand(dst, src);
}

void Assembler::lzcntq(Register dst, Register src) {
  DCHECK(IsEnabled(LZCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_modrm(dst, src);
}

void Assembler::lzcntq(Register dst, Operand src) {
  DCHECK(IsEnabled(LZCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_operand(dst, src);
}

void Assembler::lzcntl(Register dst, Register src) {
  DCHECK(IsEnabled(LZCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_modrm(dst, src);
}

void Assembler::lzcntl(Register dst, Operand src) {
  DCHECK(IsEnabled(LZCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xBD);
  emit_operand(dst, src);
}

void Assembler::popcntq(Register dst, Register src) {
  DCHECK(IsEnabled(POPCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xB8);
  emit_modrm(dst, src);
}

void Assembler::popcntq(Register dst, Operand src) {
  DCHECK(IsEnabled(POPCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_rex_64(dst, src);
  emit(0x0F);
  emit(0xB8);
  emit_operand(dst, src);
}

void Assembler::popcntl(Register dst, Register src) {
  DCHECK(IsEnabled(POPCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xB8);
  emit_modrm(dst, src);
}

void Assembler::popcntl(Register dst, Operand src) {
  DCHECK(IsEnabled(POPCNT));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xB8);
  emit_operand(dst, src);
}

void Assembler::bmi2q(SIMDPrefix pp, byte op, Register reg, Register vreg,
                      Register rm) {
  DCHECK(IsEnabled(BMI2));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, pp, k0F38, kW1);
  emit(op);
  emit_modrm(reg, rm);
}

void Assembler::bmi2q(SIMDPrefix pp, byte op, Register reg, Register vreg,
                      Operand rm) {
  DCHECK(IsEnabled(BMI2));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, pp, k0F38, kW1);
  emit(op);
  emit_operand(reg, rm);
}

void Assembler::bmi2l(SIMDPrefix pp, byte op, Register reg, Register vreg,
                      Register rm) {
  DCHECK(IsEnabled(BMI2));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, pp, k0F38, kW0);
  emit(op);
  emit_modrm(reg, rm);
}

void Assembler::bmi2l(SIMDPrefix pp, byte op, Register reg, Register vreg,
                      Operand rm) {
  DCHECK(IsEnabled(BMI2));
  EnsureSpace ensure_space(this);
  emit_vex_prefix(reg, vreg, rm, kLZ, pp, k0F38, kW0);
  emit(op);
  emit_operand(reg, rm);
}

void Assembler::rorxq(Register dst, Register src, byte imm8) {
  DCHECK(IsEnabled(BMI2));
  DCHECK(is_uint8(imm8));
  Register vreg = Register::from_code(0);  // VEX.vvvv unused
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, vreg, src, kLZ, kF2, k0F3A, kW1);
  emit(0xF0);
  emit_modrm(dst, src);
  emit(imm8);
}

void Assembler::rorxq(Register dst, Operand src, byte imm8) {
  DCHECK(IsEnabled(BMI2));
  DCHECK(is_uint8(imm8));
  Register vreg = Register::from_code(0);  // VEX.vvvv unused
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, vreg, src, kLZ, kF2, k0F3A, kW1);
  emit(0xF0);
  emit_operand(dst, src);
  emit(imm8);
}

void Assembler::rorxl(Register dst, Register src, byte imm8) {
  DCHECK(IsEnabled(BMI2));
  DCHECK(is_uint8(imm8));
  Register vreg = Register::from_code(0);  // VEX.vvvv unused
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, vreg, src, kLZ, kF2, k0F3A, kW0);
  emit(0xF0);
  emit_modrm(dst, src);
  emit(imm8);
}

void Assembler::rorxl(Register dst, Operand src, byte imm8) {
  DCHECK(IsEnabled(BMI2));
  DCHECK(is_uint8(imm8));
  Register vreg = Register::from_code(0);  // VEX.vvvv unused
  EnsureSpace ensure_space(this);
  emit_vex_prefix(dst, vreg, src, kLZ, kF2, k0F3A, kW0);
  emit(0xF0);
  emit_operand(dst, src);
  emit(imm8);
}

void Assembler::pause() {
  emit(0xF3);
  emit(0x90);
}

void Assembler::movups(XMMRegister dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  if (src.low_bits() == 4) {
    // Try to avoid an unnecessary SIB byte.
    emit_optional_rex_32(src, dst);
    emit(0x0F);
    emit(0x11);
    emit_sse_operand(src, dst);
  } else {
    emit_optional_rex_32(dst, src);
    emit(0x0F);
    emit(0x10);
    emit_sse_operand(dst, src);
  }
}

void Assembler::movups(XMMRegister dst, Operand src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x10);
  emit_sse_operand(dst, src);
}

void Assembler::movups(Operand dst, XMMRegister src) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(src, dst);
  emit(0x0F);
  emit(0x11);
  emit_sse_operand(src, dst);
}

void Assembler::sse_instr(XMMRegister dst, XMMRegister src, byte escape,
                          byte opcode) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(escape);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse_instr(XMMRegister dst, Operand src, byte escape,
                          byte opcode) {
  EnsureSpace ensure_space(this);
  emit_optional_rex_32(dst, src);
  emit(escape);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse2_instr(XMMRegister dst, XMMRegister src, byte prefix,
                           byte escape, byte opcode) {
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse2_instr(XMMRegister dst, Operand src, byte prefix,
                           byte escape, byte opcode) {
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::ssse3_instr(XMMRegister dst, XMMRegister src, byte prefix,
                            byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSSE3));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::ssse3_instr(XMMRegister dst, Operand src, byte prefix,
                            byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSSE3));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse4_instr(XMMRegister dst, Register src, byte prefix,
                           byte escape1, byte escape2, byte opcode,
                           int8_t imm8) {
  DCHECK(is_uint8(imm8));
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
  emit(imm8);
}

void Assembler::sse4_instr(XMMRegister dst, XMMRegister src, byte prefix,
                           byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse4_instr(XMMRegister dst, Operand src, byte prefix,
                           byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse4_instr(Register dst, XMMRegister src, byte prefix,
                           byte escape1, byte escape2, byte opcode,
                           int8_t imm8) {
  DCHECK(is_uint8(imm8));
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(src, dst);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(src, dst);
  emit(imm8);
}

void Assembler::sse4_instr(Operand dst, XMMRegister src, byte prefix,
                           byte escape1, byte escape2, byte opcode,
                           int8_t imm8) {
  DCHECK(is_uint8(imm8));
  DCHECK(IsEnabled(SSE4_1));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(src, dst);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(src, dst);
  emit(imm8);
}

void Assembler::sse4_2_instr(XMMRegister dst, XMMRegister src, byte prefix,
                             byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSE4_2));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::sse4_2_instr(XMMRegister dst, Operand src, byte prefix,
                             byte escape1, byte escape2, byte opcode) {
  DCHECK(IsEnabled(SSE4_2));
  EnsureSpace ensure_space(this);
  emit(prefix);
  emit_optional_rex_32(dst, src);
  emit(escape1);
  emit(escape2);
  emit(opcode);
  emit_sse_operand(dst, src);
}

void Assembler::lddqu(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0xF0);
  emit_sse_operand(dst, src);
}

void Assembler::movddup(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x12);
  emit_sse_operand(dst, src);
}

void Assembler::movddup(XMMRegister dst, Operand src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x12);
  emit_sse_operand(dst, src);
}

void Assembler::movshdup(XMMRegister dst, XMMRegister src) {
  DCHECK(IsEnabled(SSE3));
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x16);
  emit_sse_operand(dst, src);
}

void Assembler::psrldq(XMMRegister dst, uint8_t shift) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst);
  emit(0x0F);
  emit(0x73);
  emit_sse_operand(dst);
  emit(shift);
}

void Assembler::pshufhw(XMMRegister dst, XMMRegister src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::pshufhw(XMMRegister dst, Operand src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0xF3);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::pshuflw(XMMRegister dst, XMMRegister src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::pshuflw(XMMRegister dst, Operand src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0xF2);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::pshufd(XMMRegister dst, XMMRegister src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::pshufd(XMMRegister dst, Operand src, uint8_t shuffle) {
  EnsureSpace ensure_space(this);
  emit(0x66);
  emit_optional_rex_32(dst, src);
  emit(0x0F);
  emit(0x70);
  emit_sse_operand(dst, src);
  emit(shuffle);
}

void Assembler::emit_sse_operand(XMMRegister reg, Operand adr) {
  Register ireg = Register::from_code(reg.code());
  emit_operand(ireg, adr);
}

void Assembler::emit_sse_operand(Register reg, Operand adr) {
  emit_operand(reg, adr);
}

void Assembler::emit_sse_operand(XMMRegister dst, XMMRegister src) {
  emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::emit_sse_operand(XMMRegister dst, Register src) {
  emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::emit_sse_operand(Register dst, XMMRegister src) {
  emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::emit_sse_operand(XMMRegister dst) {
  emit(0xD8 | dst.low_bits());
}

void Assembler::db(uint8_t data) {
  EnsureSpace ensure_space(this);
  emit(data);
}

void Assembler::dd(uint32_t data, RelocInfo::Mode rmode) {
  EnsureSpace ensure_space(this);
  if (!RelocInfo::IsNone(rmode)) {
    DCHECK(RelocInfo::IsDataEmbeddedObject(rmode));
    RecordRelocInfo(rmode);
  }
  emitl(data);
}

void Assembler::dq(uint64_t data, RelocInfo::Mode rmode) {
  EnsureSpace ensure_space(this);
  if (!RelocInfo::IsNone(rmode)) {
    DCHECK(RelocInfo::IsDataEmbeddedObject(rmode));
    RecordRelocInfo(rmode);
  }
  emitq(data);
}

void Assembler::dq(Label* label) {
  EnsureSpace ensure_space(this);
  if (label->is_bound()) {
    internal_reference_positions_.push_back(pc_offset());
    emit(Immediate64(reinterpret_cast<Address>(buffer_start_) + label->pos(),
                     RelocInfo::INTERNAL_REFERENCE));
  } else {
    RecordRelocInfo(RelocInfo::INTERNAL_REFERENCE);
    emitl(0);  // Zero for the first 32bit marks it as 64bit absolute address.
    if (label->is_linked()) {
      emitl(label->pos());
      label->link_to(pc_offset() - sizeof(int32_t));
    } else {
      DCHECK(label->is_unused());
      int32_t current = pc_offset();
      emitl(current);
      label->link_to(current);
    }
  }
}

// Relocation information implementations.

void Assembler::RecordRelocInfo(RelocInfo::Mode rmode, intptr_t data) {
  if (!ShouldRecordRelocInfo(rmode)) return;
  RelocInfo rinfo(reinterpret_cast<Address>(pc_), rmode, data, Code());
  reloc_info_writer.Write(&rinfo);
}

const int RelocInfo::kApplyMask =
    RelocInfo::ModeMask(RelocInfo::CODE_TARGET) |
    RelocInfo::ModeMask(RelocInfo::RUNTIME_ENTRY) |
    RelocInfo::ModeMask(RelocInfo::INTERNAL_REFERENCE) |
    RelocInfo::ModeMask(RelocInfo::WASM_CALL);

bool RelocInfo::IsCodedSpecially() {
  // The deserializer needs to know whether a pointer is specially coded.  Being
  // specially coded on x64 means that it is a relative 32 bit address, as used
  // by branch instructions.
  return (1 << rmode_) & kApplyMask;
}

bool RelocInfo::IsInConstantPool() { return false; }

}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_X64
